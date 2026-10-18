// Edit → Vectors → Mirror and the arrays, through the real VectorActions on a
// real canvas (offscreen): each is one undo step, mirror keeps ids in place,
// array copies land in the toolpaths that machine their originals (or not,
// when asked), and undo / redo restore the document and the toolpaths.
//
// Plain asserts, no test framework; exits 0 on success.

#include "../src/c2ddocument.h"
#include "../src/canvas.h"
#include "../src/element.h"
#include "../src/vectoractions.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QMainWindow>
#include <QMenuBar>
#include <QUndoStack>

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

    std::printf("OK: %d checks passed\n", g_checks);
    return 0;
}
