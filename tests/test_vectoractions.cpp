// Edit → Vectors → Mirror and the arrays, through the real VectorActions on a
// real canvas (offscreen): each is one undo step, mirror keeps ids in place,
// array copies land in the toolpaths that machine their originals (or not,
// when asked), and undo / redo restore the document and the toolpaths.
//
// Plain asserts, no test framework; exits 0 on success.

#include "../src/c2ddocument.h"
#include "../src/canvas.h"
#include "../src/element.h"
#include "../src/model3d.h"
#include "../src/parametric.h"
#include "../src/vectoractions.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineF>
#include <QMainWindow>
#include <QMenuBar>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace c2d;

static int g_checks = 0;

static void check(bool cond, const char *what)
{
    ++g_checks;
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

static bool nearRect(const QRectF &a, const QRectF &b, double eps = 1e-6)
{
    return std::fabs(a.left() - b.left()) < eps && std::fabs(a.top() - b.top()) < eps &&
           std::fabs(a.width() - b.width()) < eps && std::fabs(a.height() - b.height()) < eps;
}

static QStringList refs(const Document &d, const QString &uuid)
{
    QStringList out;
    for (const Toolpath &t : d.toolpaths())
        if (t.uuid == uuid)
            for (const QJsonValue &v : t.json.value("elements").toArray())
                out << v.toObject().value("uuid").toString();
    return out;
}

int main(int argc, char *argv[])
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && qEnvironmentVariableIsEmpty("DISPLAY")
        && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    QJsonObject layer;
    layer.insert("name", QStringLiteral("DEFAULT"));
    layer.insert("uuid", QStringLiteral("{layer}"));

    Document doc;
    const Element hole = Element::makeCircle({10, 10}, 2, layer);
    const Element tri = Element::makePath({{20, 0}, {30, 0}, {20, 6}}, true, layer);
    const Element other = Element::makeRectangle({80, 80}, 4, 4, layer);
    doc.addElement(hole);
    doc.addElement(tri);
    doc.addElement(other);
    Toolpath drill;
    drill.uuid = QStringLiteral("{tp-drill}");
    drill.type = QStringLiteral("drill");
    drill.json.insert("elements", QJsonArray{QJsonObject{{"uuid", hole.id}}});
    doc.addToolpath(drill);
    Toolpath unrelated;
    unrelated.uuid = QStringLiteral("{tp-other}");
    unrelated.type = QStringLiteral("contour");
    unrelated.json.insert("elements", QJsonArray{QJsonObject{{"uuid", other.id}}});
    doc.addToolpath(unrelated);

    QMainWindow win;
    auto *canvas = new Canvas;
    win.setCentralWidget(canvas);
    QMenu *edit = win.menuBar()->addMenu(QStringLiteral("&Edit"));
    canvas->setDocument(&doc);
    VectorActions va(canvas, edit, &win);
    win.resize(900, 700);
    win.show();
    QUndoStack *undo = canvas->undoStack();
    const int base = undo->count();

    // --- mirror -----------------------------------------------------------------
    canvas->selectIds({hole.id, tri.id});   // selection box x 8..30
    va.mirror(vec::Axis::Horizontal);
    check(undo->count() == base + 1, "mirror is one undo step");
    check(doc.elements().size() == 3 && doc.elements().at(0).id == hole.id &&
          doc.elements().at(1).id == tri.id, "mirror keeps ids and order");
    check(nearRect(doc.elementById(hole.id)->painterPath.boundingRect(), QRectF(26, 8, 4, 4)),
          "mirrored circle lands on the far side");
    check(nearRect(doc.elementById(tri.id)->painterPath.boundingRect(), QRectF(8, 0, 10, 6)),
          "mirrored path lands on the near side");
    check(refs(doc, drill.uuid) == QStringList{hole.id}, "mirror leaves toolpaths alone");
    undo->undo();
    check(nearRect(doc.elementById(hole.id)->painterPath.boundingRect(), QRectF(8, 8, 4, 4)) &&
          nearRect(doc.elementById(tri.id)->painterPath.boundingRect(), QRectF(20, 0, 10, 6)),
          "undo mirror restores both");
    undo->redo();
    undo->undo();

    // --- grid array, joined to the toolpaths -------------------------------------
    canvas->selectIds({hole.id});
    va.gridArray(3, 2, 1, 1, true);
    check(undo->count() == base + 1, "grid array is one undo step (after the undone mirror)");
    check(doc.elements().size() == 3 + 5, "grid 3x2 adds five copies");
    const QStringList drilled = refs(doc, drill.uuid);
    check(drilled.size() == 6 && drilled.first() == hole.id, "drill toolpath gains the five copies");
    check(refs(doc, unrelated.uuid) == QStringList{other.id}, "unrelated toolpath untouched");
    QRectF all;
    for (const QString &id : drilled)
        all = all.isNull() ? doc.elementById(id)->painterPath.boundingRect()
                           : all.united(doc.elementById(id)->painterPath.boundingRect());
    check(nearRect(all, QRectF(8, 8, 3 * 4 + 2, 2 * 4 + 1)), "grid copies span cols*w + gaps");
    undo->undo();
    check(doc.elements().size() == 3 && refs(doc, drill.uuid) == QStringList{hole.id},
          "undo grid removes copies and toolpath refs");
    undo->redo();
    check(doc.elements().size() == 8 && refs(doc, drill.uuid).size() == 6, "redo grid");
    undo->undo();

    // --- circular array, not joined -----------------------------------------------
    canvas->selectIds({tri.id});
    va.circularArray(QPointF(25, 30), 4, 360, true, false);
    check(doc.elements().size() == 3 + 3, "circular of four adds three copies");
    check(refs(doc, drill.uuid) == QStringList{hole.id}, "unjoined array leaves toolpaths alone");
    // The half-turn copy is the triangle rotated 180 deg about (25, 30).
    QTransform half;
    half.translate(25, 30);
    half.rotate(180);
    half.translate(-25, -30);
    const QRectF want = half.map(tri.painterPath).boundingRect();
    bool found = false;
    for (int i = 3; i < doc.elements().size(); ++i)
        found = found || nearRect(doc.elements().at(i).painterPath.boundingRect(), want, 1e-6);
    check(found, "circular copy sits half way round");
    undo->undo();
    check(doc.elements().size() == 3, "undo circular");

    // --- rotate / scale -----------------------------------------------------------
    canvas->selectIds({hole.id, tri.id});   // selection box (8,0)-(30,12), center (19,6)
    va.rotate(180);
    check(undo->count() == base + 1, "rotate is one undo step");
    check(doc.elementById(hole.id)->geometryType == "circle" &&
          nearRect(doc.elementById(hole.id)->painterPath.boundingRect(), QRectF(26, 0, 4, 4)),
          "half turn about the selection center moves the circle across");
    check(nearRect(doc.elementById(tri.id)->painterPath.boundingRect(), QRectF(8, 6, 10, 6)),
          "half turn flips the triangle into the other corner");
    undo->undo();
    check(nearRect(doc.elementById(hole.id)->painterPath.boundingRect(), QRectF(8, 8, 4, 4)),
          "undo rotate");

    canvas->selectIds({hole.id});
    va.scale(2, 2);
    check(doc.elementById(hole.id)->geometryType == "circle" &&
          nearRect(doc.elementById(hole.id)->painterPath.boundingRect(), QRectF(6, 6, 8, 8)),
          "scaled circle grows about its center and stays a circle");
    check(refs(doc, drill.uuid) == QStringList{hole.id}, "scale keeps the toolpath's vector");
    undo->undo();

    // --- move / copy ----------------------------------------------------------------
    va.moveCopy(5, -1, 0, true);
    check(doc.elements().size() == 3 &&
          nearRect(doc.elementById(hole.id)->painterPath.boundingRect(), QRectF(13, 7, 4, 4)),
          "move by an exact distance");
    undo->undo();
    va.moveCopy(10, 0, 3, true);
    check(doc.elements().size() == 6 && refs(doc, drill.uuid).size() == 4,
          "three stepped copies, all drilled");
    check(nearRect(doc.elements().last().painterPath.boundingRect(), QRectF(38, 8, 4, 4)),
          "the last copy is three steps on");
    undo->undo();
    check(doc.elements().size() == 3 && refs(doc, drill.uuid) == QStringList{hole.id}, "undo copy");

    // --- fillet / chamfer -----------------------------------------------------------
    canvas->selectIds({hole.id, tri.id, other.id});
    va.corners(vec::CornerStyle::Fillet, 0.5);
    check(undo->count() == base + 1, "fillet is one undo step");
    check(doc.elementById(hole.id)->geometryType == "circle", "circle is left alone");
    check(doc.elementById(tri.id)->geometryType == "path" &&
          Element::pathModel(*doc.elementById(tri.id)).subs[0].nodes.size() == 6,
          "each triangle corner is rounded");
    check(doc.elementById(other.id)->geometryType == "path" &&
          refs(doc, unrelated.uuid) == QStringList{other.id}, "filleted rectangle keeps its id");
    undo->undo();
    check(doc.elementById(other.id)->geometryType == "rectangle", "undo fillet");
    canvas->selectIds({hole.id});
    const int before = undo->index();
    va.corners(vec::CornerStyle::Chamfer, 1);
    check(undo->index() == before, "nothing to chamfer, no command");

    // Nothing selected: nothing happens.
    canvas->selectIds({});
    va.mirror(vec::Axis::Vertical);
    va.gridArray(2, 2, 0, 0, true);
    va.rotate(45);
    va.scale(2, 2);
    va.moveCopy(1, 1, 2, true);
    va.corners(vec::CornerStyle::Fillet, 1);
    check(undo->index() == base, "no selection, no command");

    // --- Booleans and Offset leave points and construction geometry alone -----------
    {
        Document bdoc;
        const Element a = Element::makeCircle({100, 100}, 10, layer);
        const Element b = Element::makeCircle({110, 100}, 10, layer);
        const Element mark = Element::makePoint({105, 100}, layer);
        const Element guide = Element::withConstruction(
            Element::makeRectangle({105, 100}, 40, 4, layer), true);
        for (const Element &e : {a, b, mark, guide})
            bdoc.addElement(e);
        canvas->setDocument(&bdoc);
        QUndoStack *bu = canvas->undoStack();
        const int b0 = bu->index();

        canvas->selectIds({a.id, b.id, mark.id, guide.id});
        va.boolean(vec::BoolOp::Union);
        check(bu->index() == b0 + 1, "union is one undo step");
        check(bdoc.elementById(mark.id) && bdoc.elementById(guide.id)
              && !bdoc.elementById(a.id) && !bdoc.elementById(b.id),
              "union welds the circles and keeps the point and the construction rect");
        check(bdoc.elements().size() == 3, "union leaves a single weld beside them");
        const QRectF weld = bdoc.elements().first().painterPath.boundingRect();
        check(nearRect(weld, QRectF(90, 90, 30, 20), 0.05), "weld spans only the two circles");
        bu->undo();

        canvas->selectIds({mark.id, guide.id});
        va.boolean(vec::BoolOp::Union);
        check(bu->index() == b0, "a union of references only does nothing");

        canvas->selectIds({a.id, mark.id, guide.id});
        va.offsetBy(1, false);
        check(bu->index() == b0 + 1 && bdoc.elementById(mark.id) && bdoc.elementById(guide.id)
              && !bdoc.elementById(a.id) && bdoc.elements().size() == 4,
              "offset replaces the circle only");
        check(nearRect(bdoc.elements().at(0).painterPath.boundingRect(), QRectF(89, 89, 22, 22), 0.05),
              "offset grows the circle");
        bu->undo();
        canvas->selectIds({mark.id});
        va.offsetBy(1, true);
        check(bu->index() == b0, "offsetting a point alone does nothing");
        canvas->setDocument(&doc);
    }

    // --- 3D components follow vectors that Union / Offset replace ------------------
    {
        Document mdoc;
        const Element a = Element::makeCircle({100, 100}, 10, layer);
        const Element b = Element::makeCircle({110, 100}, 10, layer);
        const Element c = Element::makeCircle({200, 100}, 10, layer);
        for (const Element &e : {a, b, c})
            mdoc.addElement(e);
        ModelStore store;
        store.attach(&mdoc);
        ModelComponent relief;
        relief.id = Model3D::newId();
        relief.vectorIds = {a.id, c.id};
        ModelComponent untouched;
        untouched.id = Model3D::newId();
        untouched.vectorIds = {c.id};
        store.model.components = {relief, untouched};
        canvas->setDocument(&mdoc);
        QUndoStack *mu = canvas->undoStack();

        canvas->selectIds({a.id, b.id});
        va.boolean(vec::BoolOp::Union);
        const QString weld = mdoc.elements().first().id;
        check(store.model.components.at(0).vectorIds == QStringList({c.id, weld}),
              "union points the component at the weld");
        check(store.model.components.at(1).vectorIds == QStringList({c.id}),
              "a component not using the inputs is left alone");
        mu->undo();
        check(store.model.components.at(0).vectorIds == QStringList({a.id, c.id}),
              "undo gives the component its vectors back");
        mu->redo();
        check(store.model.components.at(0).vectorIds == QStringList({c.id, weld}),
              "redo points it at the weld again");
        mu->undo();

        canvas->selectIds({c.id});
        va.offsetBy(1, false);
        const QString grown = mdoc.elements().last().id;
        check(store.model.components.at(1).vectorIds == QStringList({grown}),
              "offset without keeping the original moves the component to the result");
        mu->undo();
        va.offsetBy(1, true);
        check(store.model.components.at(1).vectorIds == QStringList({c.id}),
              "offset that keeps the original leaves the component on it");
        canvas->setDocument(&doc);
    }

    // --- 3D components follow a text converted to paths (one path per glyph) -----------
    {
        Document mdoc;
        const Element txt = Element::makeText(QStringLiteral("HIM"), {30, 50}, 40,
                                              QStringLiteral("DejaVu Sans"), layer);
        const Element other = Element::makeCircle({200, 100}, 10, layer);
        mdoc.addElement(txt);
        mdoc.addElement(other);
        ModelStore store;
        store.attach(&mdoc);
        ModelComponent relief;
        relief.id = Model3D::newId();
        relief.vectorIds = {txt.id};
        ModelComponent untouched;
        untouched.id = Model3D::newId();
        untouched.vectorIds = {other.id};
        store.model.components = {relief, untouched};
        canvas->setDocument(&mdoc);
        QUndoStack *mu = canvas->undoStack();
        canvas->convertToPaths({txt.id});
        QStringList paths;
        for (const Element &e : mdoc.elements())
            if (e.geometryType == QLatin1String("path"))
                paths << e.id;
        QStringList got = store.model.components.at(0).vectorIds;
        std::sort(got.begin(), got.end());
        std::sort(paths.begin(), paths.end());
        check(paths.size() >= 3 && got == paths,
              "convert to paths points the component at every glyph piece");
        check(store.model.components.at(1).vectorIds == QStringList({other.id}),
              "convert leaves a component on other vectors alone");
        mu->undo();
        check(store.model.components.at(0).vectorIds == QStringList({txt.id}),
              "undo convert gives the component the text back");
        mu->redo();
        got = store.model.components.at(0).vectorIds;
        std::sort(got.begin(), got.end());
        check(got == paths, "redo convert points it at the pieces again");
        mu->undo();
        canvas->setDocument(&doc);
    }

    // Mirror and rotate carry constraints with the geometry: a point on a
    // rectangle's lower left corner ends on the corner that went there,
    // and the shape keeps its size.
    {
        auto ref = [](GeoRef::Kind k, const QString &rid, int index) {
            GeoRef r;
            r.kind = k; r.id = rid; r.index = index;
            return r;
        };
        auto add = [](Document &d, Constraint::Type t, const QVector<GeoRef> &refs, const QString &expr = QString()) {
            Constraint c;
            c.type = t;
            c.refs = refs;
            c.uid = d.parametric().nextUid();
            if (c.isDimension()) { c.name = d.parametric().nextDimensionName(); c.expr = expr; }
            param::prime(d, c);
            d.parametricRef().constraints.append(c);
        };
        auto at = [&](const Document &d, const GeoRef &r) {
            QPointF p;
            check(param::point(d, r, &p), "reference resolves");
            return p;
        };
        auto close = [](QPointF a, QPointF b) { return QLineF(a, b).length() < 1e-4; };
        for (int turn = 0; turn < 2; ++turn) {
            Document cd;
            const Element rect = Element::makeRectangle({50, 50}, 40, 20, layer);
            const Element pt = Element::makePoint({30, 40}, layer);           // on corner 0
            const Element far = Element::makeCircle({120, 50}, 5, layer);
            cd.addElement(rect);
            cd.addElement(pt);
            cd.addElement(far);
            add(cd, Constraint::Coincident, {ref(GeoRef::Point, pt.id, -1), ref(GeoRef::Point, rect.id, 0)});
            canvas->setDocument(&cd);
            canvas->selectIds({rect.id, pt.id, far.id});
            if (turn == 0)
                va.mirror(vec::Axis::Horizontal);
            else
                va.rotate(180);
            // The selection spans x 30..125: centre 77.5.
            const Element *re = cd.elementById(rect.id);
            check(std::fabs(re->raw.value("width").toDouble() - 40) < 1e-6
                      && std::fabs(re->raw.value("height").toDouble() - 20) < 1e-6,
                  turn == 0 ? "mirrored rectangle keeps its size" : "turned rectangle keeps its size");
            // Both moved as one: the point keeps to the corner that took the
            // old lower left's place (lower right, or upper right turned).
            const QPointF c = at(cd, ref(GeoRef::Point, rect.id, -1));
            check(std::fabs(c.y() - 50) < 1 && c.x() > 100, "rectangle lands at its image");
            check(close(at(cd, ref(GeoRef::Point, pt.id, -1)), c + (turn == 0 ? QPointF(20, -10) : QPointF(20, 10))),
                  "the point lands at its image");
            check(param::unsatisfied(cd).isEmpty(), "the coincidence holds on the corner now there");
        }
        {   // a pentagon mirrored: the vertex the point was on is renumbered
            Document cd;
            const Element poly = Element::makePolygon({50, 50}, 20, 5, layer, 10);
            cd.addElement(poly);
            const Element pt = Element::makePoint(at(cd, ref(GeoRef::Point, poly.id, 1)), layer);
            cd.addElement(pt);
            add(cd, Constraint::Coincident, {ref(GeoRef::Point, pt.id, -1), ref(GeoRef::Point, poly.id, 1)});
            canvas->setDocument(&cd);
            canvas->selectIds({poly.id, pt.id});
            va.mirror(vec::Axis::Horizontal);
            check(std::fabs(cd.elementById(poly.id)->raw.value("radius").toDouble() - 20) < 1e-6,
                  "mirrored pentagon keeps its radius");
            check(param::unsatisfied(cd).isEmpty(), "the point stays on its vertex");
        }
        {   // rotate by an angle that isn't a symmetry: the rectangle becomes a
            // path and its width dimension goes with the edge
            Document cd;
            const Element rect = Element::makeRectangle({50, 50}, 40, 20, layer);
            cd.addElement(rect);
            add(cd, Constraint::Distance, {ref(GeoRef::Line, rect.id, 0)}, "40");
            canvas->setDocument(&cd);
            canvas->selectIds({rect.id});
            va.rotate(30);
            check(cd.elementById(rect.id)->geometryType == QLatin1String("path"), "turned rectangle is a path");
            check(cd.parametric().constraints.size() == 1 && param::unsatisfied(cd).isEmpty(),
                  "its width dimension is kept and holds");
            QPointF a, b;
            check(param::line(cd, cd.parametric().constraints.first().refs.first(), &a, &b)
                      && std::fabs(std::atan2(b.y() - a.y(), b.x() - a.x()) * 180 / M_PI - 30) < 1e-6,
                  "on the turned bottom edge, same direction");
        }
        // A rectangle drawn as a path, Horizontal/Vertical on its edges: a
        // turn of 30 degrees turns it (they had to go) and a quarter turn
        // swaps them; neither squeezes it back onto the axes.
        for (double ang : {30.0, 90.0}) {
            Document cd;
            const Element sq = Element::makePath({{10, 10}, {40, 10}, {40, 30}, {10, 30}}, true, layer);
            cd.addElement(sq);
            add(cd, Constraint::Horizontal, {ref(GeoRef::Line, sq.id, 0)});
            add(cd, Constraint::Vertical, {ref(GeoRef::Line, sq.id, 1)});
            add(cd, Constraint::Horizontal, {ref(GeoRef::Line, sq.id, 2)});
            add(cd, Constraint::Vertical, {ref(GeoRef::Line, sq.id, 3)});
            canvas->setDocument(&cd);
            canvas->selectIds({sq.id});
            va.rotate(ang);
            QPointF a, b, c, e;
            const bool lines = param::line(cd, ref(GeoRef::Line, sq.id, 0), &a, &b)
                               && param::line(cd, ref(GeoRef::Line, sq.id, 1), &c, &e);
            check(lines && std::fabs(QLineF(a, b).length() - 30) < 1e-6 && std::fabs(QLineF(c, e).length() - 20) < 1e-6
                      && std::fabs(std::remainder(std::atan2(b.y() - a.y(), b.x() - a.x()) * 180 / M_PI - ang, 360)) < 1e-6,
                  "a constrained rectangle turns whole, its size kept");
            if (ang == 30.0)
                check(cd.parametric().constraints.isEmpty() && canvas->editWarning().contains(QStringLiteral("off the axis")),
                      "turned off the axes, its Horizontal/Vertical go, with a word");
            else
                check(cd.parametric().constraints.size() == 4 && param::unsatisfied(cd).isEmpty()
                          && cd.parametric().constraints.first().type == Constraint::Vertical,
                      "a quarter turn swaps Horizontal and Vertical, all holding");
        }
        // A dimension from the origin to a corner: turning or mirroring the
        // shape moves it whole (the dimension shows red), it is not folded
        // to meet the dimension.
        for (int k = 0; k < 3; ++k) {
            Document cd;
            const Element sq = Element::makePath({{10, 10}, {40, 10}, {40, 30}, {10, 30}}, true, layer);
            cd.addElement(sq);
            add(cd, Constraint::HDistance, {GeoRef::origin(), ref(GeoRef::Point, sq.id, 0)}, "10");
            canvas->setDocument(&cd);
            canvas->selectIds({sq.id});
            if (k == 0)
                va.rotate(180);
            else if (k == 1)
                va.rotate(30);
            else
                va.mirror(vec::Axis::Horizontal);
            QPointF a, b, c, e;
            const bool lines = param::line(cd, ref(GeoRef::Line, sq.id, 0), &a, &b)
                               && param::line(cd, ref(GeoRef::Line, sq.id, 1), &c, &e);
            // (a mirror numbers the nodes the other way round)
            const double l0 = std::min(QLineF(a, b).length(), QLineF(c, e).length()),
                         l1 = std::max(QLineF(a, b).length(), QLineF(c, e).length());
            check(lines && std::fabs(l0 - 20) < 1e-6 && std::fabs(l1 - 30) < 1e-6,
                  "a shape dimensioned from the origin turns or mirrors whole");
            check(canvas->editWarning().contains(QStringLiteral("can't hold")), "and the dimension it breaks shows red");
        }
        // The same with a point tied to a corner: the point follows the turned
        // shape and only the dimension shows red (it failed the whole solve,
        // and the point stayed behind).
        {
            Document cd;
            const Element sq = Element::makePath({{10, 10}, {40, 10}, {40, 30}, {10, 30}}, true, layer);
            const Element pt = Element::makePoint({40, 30}, layer);
            cd.addElement(sq);
            cd.addElement(pt);
            add(cd, Constraint::HDistance, {GeoRef::origin(), ref(GeoRef::Point, sq.id, 0)}, "10");
            add(cd, Constraint::Coincident, {ref(GeoRef::Point, pt.id, -1), ref(GeoRef::Point, sq.id, 2)});
            canvas->setDocument(&cd);
            canvas->selectIds({sq.id});
            va.rotate(30);
            QPointF corner, p;
            param::point(cd, ref(GeoRef::Point, sq.id, 2), &corner);
            param::point(cd, ref(GeoRef::Point, pt.id, -1), &p);
            check(QLineF(corner, p).length() < 1e-6 && param::unsatisfied(cd).size() == 1
                      && canvas->editWarning().contains(QStringLiteral("1 constraint can't hold")),
                  "a point tied to a turned shape follows it past a red dimension");
        }
        // A dimension a turn left red is not met at the next, unrelated edit
        // (moving another line, constraining it): the turned shape stays
        // whole and the dimension stays red.
        // So is adding or editing a parameter the dimension doesn't use.
        for (int k = 0; k < 4; ++k) {
            Document cd;
            const Element sq = Element::makePath({{10, 10}, {40, 10}, {40, 30}, {10, 30}}, true, layer);
            const Element ln = Element::makePath({{100, 100}, {130, 105}}, false, layer);
            cd.addElement(sq);
            cd.addElement(ln);
            cd.parametricRef().params.append({QStringLiteral("depth"), QStringLiteral("6"), QString()});
            add(cd, Constraint::HDistance, {GeoRef::origin(), ref(GeoRef::Point, sq.id, 0)}, "10");
            canvas->setDocument(&cd);
            canvas->selectIds({sq.id});
            va.rotate(180);
            if (k == 0) {
                canvas->moveElementBy(ln.id, 5, 0);
            } else if (k >= 2) {
                Parametric after = cd.parametric();
                if (k == 2)
                    after.params.append({QStringLiteral("width"), QStringLiteral("40"), QString()});
                else
                    after.params.first().expr = QStringLiteral("8");
                canvas->applyParametric(after, QStringLiteral("parameter"), false);
            } else {
                Parametric after = cd.parametric();
                Constraint h;
                h.type = Constraint::Horizontal;
                h.refs = {ref(GeoRef::Line, ln.id, 0)};
                h.uid = after.nextUid();
                after.constraints.append(h);
                canvas->applyParametric(after, QStringLiteral("horizontal"), false);
            }
            QPointF a, b, c, e;
            const bool lines = param::line(cd, ref(GeoRef::Line, sq.id, 0), &a, &b)
                               && param::line(cd, ref(GeoRef::Line, sq.id, 1), &c, &e);
            check(lines && std::fabs(QLineF(a, b).length() - 30) < 1e-6 && std::fabs(QLineF(c, e).length() - 20) < 1e-6
                      && param::unsatisfied(cd).size() == 1,
                  k == 0   ? "a red dimension is not met by folding at the next move of another line"
                  : k == 1 ? "a red dimension is not met by folding when another line is constrained"
                  : k == 2 ? "a red dimension is not met by folding when a parameter is added"
                           : "a red dimension is not met by folding when another parameter changes");
        }
        // Moving a line merely tied to the red shape (Parallel to an edge)
        // does not bring the red dimension back to fold it.
        {
            Document cd;
            const Element sq = Element::makePath({{10, 10}, {40, 10}, {40, 30}, {10, 30}}, true, layer);
            const Element ln = Element::makePath({{100, 100}, {100, 120}}, false, layer);
            cd.addElement(sq);
            cd.addElement(ln);
            add(cd, Constraint::HDistance, {GeoRef::origin(), ref(GeoRef::Point, sq.id, 0)}, "10");
            add(cd, Constraint::Parallel, {ref(GeoRef::Line, ln.id, 0), ref(GeoRef::Line, sq.id, 1)});
            canvas->setDocument(&cd);
            canvas->selectIds({sq.id});
            va.rotate(180);
            canvas->moveElementBy(ln.id, 5, 0);
            QPointF a, b, c, e;
            const bool lines = param::line(cd, ref(GeoRef::Line, sq.id, 0), &a, &b)
                               && param::line(cd, ref(GeoRef::Line, sq.id, 1), &c, &e);
            check(lines && std::fabs(QLineF(a, b).length() - 30) < 1e-6 && std::fabs(QLineF(c, e).length() - 20) < 1e-6
                      && param::unsatisfied(cd).size() == 1,
                  "moving a line tied to a red shape does not fold it");
        }
        // A red dimension left out of a solve stands in at its exact value:
        // one defined from it (d2 = d1) still holds after an unrelated edit.
        {
            Document cd;
            const Element l1 = Element::makePath({{0, 0}, {20, 0}}, false, layer);
            const Element l2 = Element::makePath({{0, 10}, {12.3456, 10}}, false, layer);
            const Element l3 = Element::makePath({{0, 50}, {30, 55}}, false, layer);
            cd.addElement(l1);
            cd.addElement(l2);
            cd.addElement(l3);
            add(cd, Constraint::Fix, {ref(GeoRef::Line, l1.id, 0)});
            add(cd, Constraint::Distance, {ref(GeoRef::Line, l1.id, 0)}, "12.3456");
            add(cd, Constraint::Distance, {ref(GeoRef::Line, l2.id, 0)}, "d1");
            canvas->setDocument(&cd);
            canvas->moveElementBy(l3.id, 5, 0);
            check(param::unsatisfied(cd).size() == 1, "a dimension defined from a red one still holds");
        }
        // Deleting the constraint that kept a red one from holding, elsewhere
        // in its sketch, lets it hold.
        {
            Document cd;
            const Element a = Element::makePath({{0, 0}, {10, 0}}, false, layer);
            const Element b = Element::makePath({{10, 0}, {20, 0}}, false, layer);
            const Element c = Element::makePath({{20, 0}, {20, 3}}, false, layer);
            cd.addElement(a);
            cd.addElement(b);
            cd.addElement(c);
            add(cd, Constraint::Fix, {ref(GeoRef::Line, a.id, 0)});
            add(cd, Constraint::Coincident, {ref(GeoRef::Point, a.id, 1), ref(GeoRef::Point, b.id, 0)});
            add(cd, Constraint::Horizontal, {ref(GeoRef::Line, b.id, 0)});
            const QString blocker = cd.parametric().constraints.last().uid;
            add(cd, Constraint::Distance, {ref(GeoRef::Line, b.id, 0)}, "10");
            add(cd, Constraint::Coincident, {ref(GeoRef::Point, b.id, 1), ref(GeoRef::Point, c.id, 0)});
            add(cd, Constraint::Vertical, {ref(GeoRef::Line, c.id, 0)});
            add(cd, Constraint::Distance, {ref(GeoRef::Line, c.id, 0)}, "3");
            add(cd, Constraint::VDistance, {GeoRef::origin(), ref(GeoRef::Point, c.id, 1)}, "8");
            canvas->setDocument(&cd);
            check(param::unsatisfied(cd).size() == 1, "the blocked dimension starts red");
            canvas->deleteConstraint(blocker);
            check(param::unsatisfied(cd).isEmpty(), "deleting what blocked a red dimension lets it hold");
        }
        // A length's label turns with its edge on a quarter turn and a mirror.
        for (int k = 0; k < 2; ++k) {
            Document cd;
            const Element sq = Element::makePath({{10, 10}, {40, 10}, {40, 30}, {10, 30}}, true, layer);
            cd.addElement(sq);
            add(cd, Constraint::Distance, {ref(GeoRef::Line, sq.id, 0)}, "30");
            cd.parametricRef().constraints.last().label = QPointF(0, -8);
            canvas->setDocument(&cd);
            canvas->selectIds({sq.id});
            if (k == 0)
                va.rotate(90);
            else
                va.mirror(vec::Axis::Horizontal);
            const QPointF l = cd.parametric().constraints.last().label;
            check(k == 0 ? QLineF(l, QPointF(8, 0)).length() < 1e-6 || QLineF(l, QPointF(-8, 0)).length() < 1e-6
                         : std::fabs(std::fabs(l.y()) - 8) < 1e-6 && std::fabs(l.x()) < 1e-6,
                  "a length's label turns with its edge");
        }
        // A 200% x 50% scale (determinant 1) is no rigid move: a length
        // dimension re-solves to hold, as at 200% x 60%.
        for (double sy : {0.5, 0.6}) {
            Document cd;
            const Element sq = Element::makePath({{10, 10}, {30, 10}, {30, 30}, {10, 30}}, true, layer);
            cd.addElement(sq);
            add(cd, Constraint::Distance, {ref(GeoRef::Line, sq.id, 0)}, "20");
            canvas->setDocument(&cd);
            canvas->selectIds({sq.id});
            va.scale(2, sy);
            QPointF a, b;
            check(param::line(cd, ref(GeoRef::Line, sq.id, 0), &a, &b) && std::fabs(QLineF(a, b).length() - 20) < 1e-6
                      && param::unsatisfied(cd).isEmpty(),
                  "a scale with X% times Y% = 1 re-solves its dimensions");
        }
        // A horizontal distance turned off the axis that an expression uses
        // lives on as a parameter: the expression keeps working.
        {
            Document cd;
            const Element sq = Element::makePath({{10, 10}, {40, 10}, {40, 30}, {10, 30}}, true, layer);
            cd.addElement(sq);
            add(cd, Constraint::HDistance, {ref(GeoRef::Point, sq.id, 0), ref(GeoRef::Point, sq.id, 1)}, "30");
            const QString d1 = cd.parametric().constraints.last().name;
            add(cd, Constraint::Distance, {ref(GeoRef::Line, sq.id, 1)}, d1 + QStringLiteral("*2/3"));
            canvas->setDocument(&cd);
            canvas->selectIds({sq.id});
            va.rotate(30);
            bool asParam = false;
            for (const UserParameter &up : cd.parametric().params)
                asParam |= up.name == d1;
            check(asParam && param::unsatisfied(cd).isEmpty(), "a dropped dimension an expression uses becomes a parameter");
        }
        {   // the edit's own warning stays in the status bar after the
            // action's "Rotated ..." / "Mirrored ..." report
            Document cd;
            const Element rect = Element::makeRectangle({50, 50}, 40, 20, layer);
            const Element loose = Element::makeCircle({100, 50}, 5, layer);
            cd.addElement(rect);
            cd.addElement(loose);
            add(cd, Constraint::Fix, {ref(GeoRef::Line, rect.id, 0)});
            canvas->setDocument(&cd);
            QString hint;
            auto conn = QObject::connect(canvas, &Canvas::statusHint, [&](const QString &h) { hint = h; });
            canvas->selectIds({rect.id, loose.id});
            va.rotate(90);
            check(hint.startsWith(QStringLiteral("Rotated")) && hint.contains(QStringLiteral("locked with Fix")),
                  "rotate keeps the Fix warning after its own report");
            va.mirror(vec::Axis::Vertical);
            check(hint.startsWith(QStringLiteral("Mirrored")) && hint.contains(QStringLiteral("locked with Fix")),
                  "mirror keeps the Fix warning after its own report");
            canvas->selectIds({loose.id});
            va.scale(2, 2);
            check(hint.startsWith(QStringLiteral("Scaled")) && !hint.contains(QStringLiteral("Fix")),
                  "no stale warning on an edit that raised none");
            QObject::disconnect(conn);
        }
        canvas->setDocument(&doc);
    }

    // A sketch point only moves under Scale: it never stretches into a path
    // nor grows.
    for (int k = 0; k < 2; ++k) {
        Document cd;
        const Element rect = Element::makeRectangle({50, 50}, 40, 20, layer);
        const Element pt = Element::makePoint({30, 40}, layer);
        cd.addElement(rect);
        cd.addElement(pt);
        canvas->setDocument(&cd);
        canvas->selectIds({rect.id, pt.id});
        if (k == 0)
            va.scale(2, 1);
        else
            va.scale(2, 2);
        const Element *e = cd.elementById(pt.id);
        check(e && Element::isPoint(*e), "scaled sketch point is still a point");
        check(std::fabs(e->raw.value("radius").toDouble() - pt.raw.value("radius").toDouble()) < 1e-9,
              "scaled sketch point keeps its size");
        const QRectF rb = cd.elementById(rect.id)->painterPath.boundingRect();
        const QPointF c = e->painterPath.boundingRect().center();
        check(std::fabs(c.x() - rb.left()) < 1e-6 && std::fabs(c.y() - rb.top()) < 1e-6,
              "scaled sketch point moves with the shape (its corner)");
        canvas->setDocument(&doc);
    }

    std::printf("OK: %d checks passed\n", g_checks);
    return 0;
}
