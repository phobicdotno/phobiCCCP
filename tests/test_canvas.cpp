// Headless interaction checks for the canvas tools (offscreen platform,
// synthetic mouse/key events through QTest):
// - pen tool: click = corner, click-drag = symmetric curve node, Enter
//   finishes open, clicking the start closes; each path is one undo step;
// - node editor: anchor drag, double-click insert, Delete remove — one undo
//   step each, the document JSON carries the edited control points;
// - convert-to-path and text edits are undoable.
//
// Plain asserts, no test framework; exits 0 on success.

#include "../src/canvas.h"
#include "../src/c2ddocument.h"
#include "../src/element.h"
#include "../src/propertiespanel.h"

#include <QApplication>
#include <QDoubleSpinBox>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QJsonArray>
#include <QJsonObject>
#include <QTest>
#include <QUndoStack>

#include <cmath>
#include <cstdio>
#include <cstdlib>

static int g_checks = 0;

static void check(bool cond, const char *what)
{
    ++g_checks;
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

static QPoint vp(c2d::Canvas *c, QPointF mm) { return c->mapFromScene(mm); }

static void click(c2d::Canvas *c, QPointF mm)
{
    QTest::mouseClick(c->viewport(), Qt::LeftButton, Qt::NoModifier, vp(c, mm));
}

static void drag(c2d::Canvas *c, QPointF from, QPointF to, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QTest::mousePress(c->viewport(), Qt::LeftButton, mods, vp(c, from));
    QTest::mouseMove(c->viewport(), vp(c, (from + to) / 2));
    QTest::mouseMove(c->viewport(), vp(c, to));
    QTest::mouseRelease(c->viewport(), Qt::LeftButton, mods, vp(c, to));
}

static QPointF row(const QJsonObject &o, const char *key, int i)
{
    const QJsonArray p = o.value(QLatin1String(key)).toArray().at(i).toArray();
    return QPointF(p.at(0).toDouble(), p.at(1).toDouble());
}

static bool near(QPointF a, QPointF b, double eps = 1e-6)
{
    return std::fabs(a.x() - b.x()) < eps && std::fabs(a.y() - b.y()) < eps;
}

int main(int argc, char *argv[])
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && qEnvironmentVariableIsEmpty("DISPLAY")
        && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    c2d::Document doc;   // empty document: no board, but elements work
    c2d::Canvas canvas;
    canvas.resize(900, 700);
    canvas.show();
    canvas.setDocument(&doc);
    // Fixed zoom: 3 px per mm, Y flipped like the app, origin near the corner.
    canvas.resetTransform();
    canvas.scale(3, -3);
    canvas.centerOn(QPointF(100, 100));
    QUndoStack *undo = canvas.undoStack();

    // --- pen tool: corner, dragged curve node, corner, Enter --------------------
    canvas.setTool(c2d::Canvas::DrawPath);
    click(&canvas, {10, 10});
    drag(&canvas, {50, 10}, {60, 20});           // pulls symmetric handles
    click(&canvas, {50, 50});
    QTest::keyClick(&canvas, Qt::Key_Return);
    check(doc.elements().size() == 1, "pen tool adds one element on Enter");
    check(undo->count() == 1, "pen path is one undo step");
    {
        const QJsonObject o = doc.elements().first().raw;
        check(o.value("geometryType").toString() == QLatin1String("path"), "pen makes a path");
        const QJsonArray pt = o.value("point_type").toArray();
        check(pt.size() == 3 && pt.at(0).toInt() == 0 && pt.at(1).toInt() == 3 && pt.at(2).toInt() == 3,
              "open path: first, curve into the dragged node, curve out of it");
        check(near(row(o, "points", 1), {50, 10}) && near(row(o, "cp2", 1), {40, 0}),
              "in-handle mirrors the drag (symmetric)");
        check(near(row(o, "cp1", 2), {60, 20}) && near(row(o, "cp2", 2), {50, 50}),
              "out-handle is where the drag ended; corner keeps its anchor as cp2");
        check(near(row(o, "cp1", 1), {10, 10}), "corner start: cp1 is the anchor");
        check(o.value("smooth").toArray().at(1).toInt() == 1, "dragged node is smooth");
    }
    undo->undo();
    check(doc.elements().isEmpty(), "undo removes the pen path");
    undo->redo();
    check(doc.elements().size() == 1, "redo restores it");

    // --- pen tool: close by clicking the start ------------------------------------
    click(&canvas, {100, 100});
    click(&canvas, {140, 100});
    click(&canvas, {140, 140});
    click(&canvas, {100, 100});                  // near the start: closes
    check(doc.elements().size() == 2, "clicking the start closes and adds the path");
    QString closedId;
    {
        const QJsonObject o = doc.elements().at(1).raw;
        closedId = doc.elements().at(1).id;
        const QJsonArray pt = o.value("point_type").toArray();
        check(pt.size() == 5 && pt.at(4).toInt() == 4 && pt.at(3).toInt() == 1,
              "closed polyline: 3 anchors + return row + close row, all lines");
        // Byte-identical to the legacy polyline factory.
        const c2d::Element legacy = c2d::Element::makePath({{100, 100}, {140, 100}, {140, 140}}, true, {});
        for (const char *k : {"points", "cp1", "cp2", "point_type", "smooth"})
            check(o.value(QLatin1String(k)) == legacy.raw.value(QLatin1String(k)),
                  "closed pen polyline matches makePath rows");
    }

    // --- node editor: drag an anchor ------------------------------------------------
    canvas.setTool(c2d::Canvas::NodeEdit);
    canvas.selectElements({closedId});
    const int before = undo->count();
    drag(&canvas, {140, 140}, {150, 150});
    check(undo->count() == before + 1, "anchor drag is one undo step");
    check(near(row(doc.elementById(closedId)->raw, "points", 2), {150, 150}), "anchor moved in the document");
    undo->undo();
    check(near(row(doc.elementById(closedId)->raw, "points", 2), {140, 140}), "anchor drag undoes");
    undo->redo();

    // --- node editor: double-click inserts, Delete removes ---------------------------
    QTest::mouseDClick(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, vp(&canvas, {120, 100}));
    check(doc.elementById(closedId)->raw.value("points").toArray().size() == 6, "double-click inserts a node");
    check(near(row(doc.elementById(closedId)->raw, "points", 1), {120, 100}, 1e-2), "inserted node sits on the segment");
    check(undo->count() == before + 2, "insert is one undo step");
    click(&canvas, {120, 100});                  // select the new node
    QTest::keyClick(&canvas, Qt::Key_Delete);
    check(doc.elementById(closedId)->raw.value("points").toArray().size() == 5, "Delete removes the selected node");
    check(undo->count() == before + 3, "delete is one undo step");
    check(near(row(doc.elementById(closedId)->raw, "points", 0), {100, 100})
          && near(row(doc.elementById(closedId)->raw, "points", 3), {100, 100}),
          "endpoints preserved");
    undo->undo();
    check(doc.elementById(closedId)->raw.value("points").toArray().size() == 6, "delete undoes");
    undo->undo();
    check(doc.elementById(closedId)->raw.value("points").toArray().size() == 5, "insert undoes");

    // --- node editor on the curved path: drag a handle, Alt breaks symmetry -----------
    const QString curvedId = doc.elements().first().id;
    canvas.selectElements({curvedId});
    click(&canvas, {50, 10});                    // select node 1 so its handles win the hit test
    drag(&canvas, {60, 20}, {70, 10});           // out-handle of the symmetric node
    {
        const QJsonObject o = doc.elementById(curvedId)->raw;
        check(near(row(o, "cp1", 2), {70, 10}), "handle drag moves the out-handle");
        check(near(row(o, "cp2", 1), {30, 10}), "symmetric node mirrors the opposite handle");
    }
    drag(&canvas, {70, 10}, {80, 30}, Qt::AltModifier);
    {
        const QJsonObject o = doc.elementById(curvedId)->raw;
        check(near(row(o, "cp1", 2), {80, 30}) && near(row(o, "cp2", 1), {30, 10}),
              "Alt-drag moves only the grabbed handle");
        check(o.value("smooth").toArray().at(1).toInt() == 0, "broken symmetry demotes to corner");
    }

    // --- editing a circle's node turns it into a path ----------------------------------
    canvas.setTool(c2d::Canvas::Select);
    const c2d::Element circle = c2d::Element::makeCircle({200, 200}, 20, doc.defaultLayer());
    doc.addElement(circle);
    canvas.rebuild();
    canvas.setTool(c2d::Canvas::NodeEdit);
    canvas.selectElements({circle.id});
    drag(&canvas, {220, 200}, {230, 200});
    check(doc.elementById(circle.id)->geometryType == QLatin1String("path"),
          "node edit converts the circle to a path");
    check(near(row(doc.elementById(circle.id)->raw, "points", 2), {230, 200})
          && near(row(doc.elementById(circle.id)->raw, "position", 0), {0, 0}),
          "converted path is absolute with the moved anchor");
    undo->undo();
    check(doc.elementById(circle.id)->geometryType == QLatin1String("circle"), "conversion undoes");

    // --- convert to path + text edits ------------------------------------------------------
    const c2d::Element rect = c2d::Element::makeRectangle({300, 300}, 40, 20, doc.defaultLayer());
    doc.addElement(rect);
    canvas.rebuild();
    canvas.convertToPaths({rect.id});
    check(doc.elementById(rect.id)->geometryType == QLatin1String("path"), "convert to path keeps the id");
    check(near(doc.elementById(rect.id)->painterPath.boundingRect().center(), {300, 300}), "converted rect stays put");
    undo->undo();
    check(doc.elementById(rect.id)->geometryType == QLatin1String("rectangle"), "convert undoes");

    const c2d::Element text = c2d::Element::makeText(QStringLiteral("Hi"), {10, 200}, 10,
                                                     QStringLiteral("Helvetica"), doc.defaultLayer());
    doc.addElement(text);
    canvas.rebuild();
    QJsonObject ch;
    ch.insert("arc_enabled", true);
    ch.insert("arc_radius", 30.0);
    canvas.editText(text.id, ch);
    check(doc.elementById(text.id)->raw.value("arc_enabled").toBool()
          && doc.elementById(text.id)->raw.value("arc_radius").toDouble() == 30.0, "editText applies arc keys");
    check(doc.elementById(text.id)->painterPath.boundingRect() != text.painterPath.boundingRect(),
          "arc text re-renders");
    undo->undo();
    check(!doc.elementById(text.id)->raw.value("arc_enabled").toBool(), "text edit undoes");
    const QVector<c2d::Element> glyphs = c2d::Element::toPaths(*doc.elementById(text.id));
    {
        c2d::Toolpath tp;
        tp.uuid = QStringLiteral("{tp-text}");
        tp.type = QStringLiteral("pocket_toolpath");
        tp.json = QJsonObject{{"uuid", tp.uuid}, {"type", tp.type},
                              {"elements", QJsonArray{QJsonObject{{"uuid", text.id}}}}};
        doc.addToolpath(tp);
    }
    auto tpRefs = [&] {
        for (const c2d::Toolpath &t : doc.toolpaths())
            if (t.uuid == QLatin1String("{tp-text}"))
                return int(t.json.value("elements").toArray().size());
        return -1;
    };
    canvas.convertToPaths({text.id});
    check(doc.elementById(text.id)->geometryType == QLatin1String("path")
          && doc.elements().size() == 4 + glyphs.size(), "text converts to one path per contour");
    check(glyphs.size() > 1 && tpRefs() == glyphs.size(),
          "a toolpath on the text machines every converted contour");
    undo->undo();
    check(tpRefs() == 1, "undo puts the toolpath back on the text");
    undo->redo();
    check(tpRefs() == glyphs.size(), "redo retargets the toolpath again");
    undo->undo();
    check(doc.elementById(text.id)->geometryType == QLatin1String("text") && doc.elements().size() == 5,
          "text conversion undoes");

    // --- the node tool selects by clicking -------------------------------------------
    // Regression: NodeEdit fell through into the shape-drawing branch, which
    // accepted the press before the scene saw it, so nothing could be picked
    // with the tool unless it was selected with another one first.
    {
        canvas.setTool(c2d::Canvas::Select);
        const c2d::Element target =
            c2d::Element::makeRectangle({420, 420}, 60, 40, doc.defaultLayer());
        doc.addElement(target);
        canvas.rebuild();
        canvas.selectElements({});                 // nothing selected
        canvas.setTool(c2d::Canvas::NodeEdit);
        click(&canvas, {420, 420});                // click the shape itself
        check(canvas.selectedElementIds() == QStringList{target.id},
              "node tool selects an element by clicking it");
        const int elems = doc.elements().size();
        click(&canvas, {480, 480});                // empty space: deselect, draw nothing
        check(doc.elements().size() == elems,
              "node tool never starts a new shape");
    }

    // --- converting something with no outline leaves it alone -------------------------
    {
        const c2d::Element blank =
            c2d::Element::makeText(QStringLiteral("   "), {500, 500}, 10,
                                   QStringLiteral("DejaVu Sans"), doc.defaultLayer());
        doc.addElement(blank);
        canvas.rebuild();
        const int before = doc.elements().size();
        canvas.convertToPaths({blank.id});
        check(doc.elements().size() == before && doc.elementById(blank.id) != nullptr,
              "convert to path does not delete an element with no outline");
    }

    // --- Fusion-style Create tools ------------------------------------------------------
    {
        canvas.selectElements({});
        // Ellipse: drag across the box; Ctrl drags from the center.
        canvas.setTool(c2d::Canvas::DrawEllipse);
        int n = doc.elements().size();
        drag(&canvas, {160, 150}, {200, 170});
        check(doc.elements().size() == n + 1, "ellipse tool adds one element");
        QRectF b = doc.elements().last().painterPath.boundingRect();
        check(std::fabs(b.width() - 40) < 0.7 && std::fabs(b.height() - 20) < 0.7 &&
              std::fabs(b.center().x() - 180) < 0.4 && std::fabs(b.center().y() - 160) < 0.4,
              "ellipse fills the dragged box");
        drag(&canvas, {180, 100}, {200, 110}, Qt::ControlModifier);
        b = doc.elements().last().painterPath.boundingRect();
        check(std::fabs(b.center().x() - 180) < 0.4 && std::fabs(b.center().y() - 100) < 0.4 &&
              std::fabs(b.width() - 40) < 0.7, "Ctrl-drag: ellipse from the center");

        // Slot: two centers, then the width.
        canvas.setTool(c2d::Canvas::DrawSlot);
        n = doc.elements().size();
        click(&canvas, {160, 200});
        click(&canvas, {200, 200});
        check(doc.elements().size() == n, "slot waits for its third click");
        click(&canvas, {180, 205});
        check(doc.elements().size() == n + 1 && undo->index() == undo->count(), "slot added");
        b = doc.elements().last().painterPath.boundingRect();
        check(std::fabs(b.width() - 50) < 1.0 && std::fabs(b.height() - 10) < 1.0,
              "slot: 40 mm between centers, 10 mm wide");

        // Escape abandons a half-drawn shape.
        click(&canvas, {160, 30});
        QTest::keyClick(&canvas, Qt::Key_Escape);
        click(&canvas, {170, 30});
        click(&canvas, {190, 30});
        check(doc.elements().size() == n + 1, "Esc cancels the clicks so far");
        QTest::keyClick(&canvas, Qt::Key_Escape);

        // 3-point arc.
        canvas.setTool(c2d::Canvas::DrawArc);
        click(&canvas, {230, 120});
        click(&canvas, {190, 120});
        click(&canvas, {210, 140});
        check(doc.elements().size() == n + 2, "arc added");
        const c2d::Element &a = doc.elements().last();
        check(a.geometryType == "path" && a.raw["point_type"].toArray().last().toInt() != 4,
              "arc is an open path");
        b = a.painterPath.boundingRect();
        check(std::fabs(b.width() - 40) < 1.0 && std::fabs(b.height() - 20) < 1.0,
              "arc is the upper half circle");

        // Tangent arc off that arc's end (190, 120), where it arrives heading
        // down: a half circle on to (170, 120), bulging below.
        canvas.setArcMode(c2d::Canvas::ArcTangent);
        int m = doc.elements().size();
        click(&canvas, {191, 121});
        click(&canvas, {170, 120});
        check(doc.elements().size() == m + 1, "tangent arc added");
        b = doc.elements().last().painterPath.boundingRect();
        check(std::fabs(b.width() - 20) < 1.0 && std::fabs(b.height() - 10) < 1.0 && b.bottom() < 121,
              "tangent arc carries on downward into a half circle");
        click(&canvas, {300, 10});   // nothing there: no start picked
        check(doc.elements().size() == m + 1, "tangent arc needs an open end to start from");

        // Center-point arc: center, start (radius 20), a quarter turn on.
        canvas.setArcMode(c2d::Canvas::ArcCenter);
        click(&canvas, {100, 60});
        click(&canvas, {120, 60});
        QTest::mouseMove(canvas.viewport(), vp(&canvas, {115, 75}));
        click(&canvas, {100, 80});
        check(doc.elements().size() == m + 2, "center arc added");
        b = doc.elements().last().painterPath.boundingRect();
        check(std::fabs(b.width() - 20) < 1.0 && std::fabs(b.height() - 20) < 1.0,
              "center arc: the quarter the cursor swept");

        // Overall slot: the clicks are the slot's ends.
        canvas.setTool(c2d::Canvas::DrawSlot);
        canvas.setSlotMode(c2d::Canvas::SlotOverall);
        click(&canvas, {100, 160});
        click(&canvas, {150, 160});
        click(&canvas, {125, 165});
        check(doc.elements().size() == m + 3, "overall slot added");
        b = doc.elements().last().painterPath.boundingRect();
        check(std::fabs(b.width() - 50) < 1.0 && std::fabs(b.height() - 10) < 1.0,
              "overall slot: 50 mm end to end, 10 mm wide");
        canvas.setSlotMode(c2d::Canvas::SlotCenterToCenter);

        // Edge polygon: a hexagon on a 10 mm edge, above it.
        canvas.setTool(c2d::Canvas::DrawPolygon);
        canvas.setPolygonMode(c2d::Canvas::PolyEdge);
        click(&canvas, {100, 100});
        click(&canvas, {110, 100});
        click(&canvas, {105, 104});
        check(doc.elements().size() == m + 4, "edge polygon added");
        const c2d::Element &hex = doc.elements().last();
        check(hex.geometryType == "regular_polygon"
                  && std::fabs(hex.raw["radius"].toDouble() - 10) < 0.5
                  && hex.painterPath.boundingRect().top() > 99,
              "edge polygon: a parametric hexagon standing on the edge");
        canvas.setPolygonMode(c2d::Canvas::PolyInscribed);

        // Point, then make it and the hexagon construction and back.
        canvas.setTool(c2d::Canvas::DrawPoint);
        click(&canvas, {60, 60});
        check(doc.elements().size() == m + 5 && c2d::Element::isPoint(doc.elements().last()),
              "point tool adds a sketch point");
        const QString hexId = hex.id;
        const int before = undo->index();
        canvas.toggleConstruction({hexId});
        check(c2d::Element::isConstruction(*doc.elementById(hexId)) && undo->index() == before + 1,
              "construction toggle: one undo step");
        undo->undo();
        check(!c2d::Element::isConstruction(*doc.elementById(hexId)), "undo restores normal geometry");

        // Fit-point spline: three clicks, Enter.
        canvas.setTool(c2d::Canvas::DrawSpline);
        click(&canvas, {200, 40});
        click(&canvas, {215, 55});
        click(&canvas, {230, 40});
        check(doc.elements().size() == m + 5, "spline waits for Enter");
        QTest::keyClick(&canvas, Qt::Key_Return);
        check(doc.elements().size() == m + 6, "spline added");
        b = doc.elements().last().painterPath.boundingRect();
        check(std::fabs(b.width() - 30) < 1.0 && b.top() > 39 && b.bottom() > 54,
              "spline runs through its points");

        // Conic: start, end, apex; rho 0.5 peaks halfway to the apex.
        canvas.setTool(c2d::Canvas::DrawConic);
        canvas.setConicRho(0.5);
        click(&canvas, {250, 40});
        click(&canvas, {290, 40});
        check(doc.elements().size() == m + 6, "conic waits for the apex");
        click(&canvas, {270, 70});
        check(doc.elements().size() == m + 7, "conic added");
        b = doc.elements().last().painterPath.boundingRect();
        check(std::fabs(b.width() - 40) < 1.0 && std::fabs(b.height() - 15) < 1.0,
              "conic spans start to end, halfway up to the apex");

        canvas.setTool(c2d::Canvas::Select);
    }

    // --- Trim / Extend / Break --------------------------------------------------------
    {
        c2d::Document d2;
        c2d::Canvas cv;
        cv.resize(900, 700);
        cv.show();
        cv.setDocument(&d2);
        cv.resetTransform();
        cv.scale(3, -3);
        cv.centerOn(QPointF(50, 50));
        QUndoStack *u = cv.undoStack();
        const QJsonObject layer = d2.defaultLayer();
        const c2d::Element h = c2d::Element::makePath({{0, 50}, {100, 50}}, false, layer);
        const c2d::Element v1 = c2d::Element::makePath({{30, 0}, {30, 100}}, false, layer);
        const c2d::Element v2 = c2d::Element::makePath({{70, 0}, {70, 100}}, false, layer);
        const c2d::Element shortLine = c2d::Element::makePath({{0, 20}, {20, 20}}, false, layer);
        for (const c2d::Element &e : {h, v1, v2, shortLine})
            d2.addElement(e);
        cv.rebuild();

        cv.setTool(c2d::Canvas::Trim);
        QTest::mouseMove(cv.viewport(), vp(&cv, {50, 50}));
        click(&cv, {50, 50});
        check(u->count() == 1, "trim is one undo step");
        const c2d::PathModel m = c2d::Element::pathModel(*d2.elementById(h.id));
        check(m.subs.size() == 2 && near(m.subs[0].nodes.last().p, {30, 50}) &&
              near(m.subs[1].nodes.first().p, {70, 50}), "trim cut the middle out between the verticals");
        u->undo();
        check(c2d::Element::pathModel(*d2.elementById(h.id)).subs.size() == 1, "undo trim");

        cv.setTool(c2d::Canvas::Break);
        click(&cv, {50, 50});
        check(d2.elements().size() == 6 && u->count() == 1, "break makes three pieces of the line");
        u->undo();
        check(d2.elements().size() == 4, "undo break");

        cv.setTool(c2d::Canvas::Extend);
        click(&cv, {19, 20});
        check(near(c2d::Element::pathModel(*d2.elementById(shortLine.id)).subs[0].nodes.last().p, {30, 20}),
              "extend runs the short line on to the first vertical");
        const int steps = u->count();
        click(&cv, {90, 90});
        check(u->count() == steps, "clicking empty space does nothing");
        cv.setTool(c2d::Canvas::Select);
    }

    // --- Measure: reads out, adds nothing ---------------------------------------------
    {
        QString last;
        const QMetaObject::Connection hint = QObject::connect(
            &canvas, &c2d::Canvas::statusHint, [&last](const QString &m) { last = m; });
        canvas.setTool(c2d::Canvas::Measure);
        const int n = doc.elements().size(), steps = undo->count();
        drag(&canvas, {100, 100}, {130, 140});
        check(doc.elements().size() == n && undo->count() == steps, "measure adds nothing");
        check(last.startsWith(QLatin1String("50.")) || last.startsWith(QLatin1String("49.")),
              "measure reads out the 30-40-50 distance");
        canvas.setTool(c2d::Canvas::Select);
        QObject::disconnect(hint);
    }

    // --- Fusion-style circle and rectangle modes --------------------------------
    {
        using C = c2d::Canvas;
        auto last = [&] { return doc.elements().last().raw; };
        auto center = [&](const QJsonObject &o) {
            const QJsonArray c = o.value("center").toArray();
            return QPointF(c.at(0).toDouble(), c.at(1).toDouble());
        };
        auto isCircle = [&](QPointF c, double r) {
            const QJsonObject o = last();
            return o.value("geometryType").toString() == QLatin1String("circle")
                   && near(center(o), c) && std::abs(o.value("radius").toDouble() - r) < 0.02;
        };
        auto isRect = [&](QPointF c, double w, double h) {
            const QJsonObject o = last();
            return o.value("geometryType").toString() == QLatin1String("rectangle")
                   && near(center(o), c) && std::abs(o.value("width").toDouble() - w) < 0.02
                   && std::abs(o.value("height").toDouble() - h) < 0.02;
        };
        int n = doc.elements().size();

        canvas.setTool(C::DrawCircle);
        canvas.setCircleMode(C::CircleCenterDiameter);
        drag(&canvas, {10, 120}, {15, 120});
        check(doc.elements().size() == ++n && isCircle({10, 120}, 5), "center circle by drag");
        click(&canvas, {40, 120});
        click(&canvas, {40, 126});
        check(doc.elements().size() == ++n && isCircle({40, 120}, 6), "center circle by two clicks");

        canvas.setCircleMode(C::Circle2Point);
        click(&canvas, {60, 120});
        click(&canvas, {80, 120});
        check(doc.elements().size() == ++n && isCircle({70, 120}, 10), "2-point circle");

        canvas.setCircleMode(C::Circle3Point);
        click(&canvas, {100, 120});
        click(&canvas, {110, 130});
        check(doc.elements().size() == n, "3-point circle waits for the third point");
        click(&canvas, {120, 120});
        check(doc.elements().size() == ++n && isCircle({110, 120}, 10), "3-point circle");

        canvas.setTool(C::DrawRect);
        canvas.setRectMode(C::Rect2Point);
        drag(&canvas, {10, 150}, {30, 160});
        check(doc.elements().size() == ++n && isRect({20, 155}, 20, 10), "2-point rectangle");
        canvas.setRectMode(C::RectCenter);
        click(&canvas, {60, 155});
        click(&canvas, {70, 160});
        check(doc.elements().size() == ++n && isRect({60, 155}, 20, 10), "center rectangle");
        canvas.setRectMode(C::Rect3Point);
        click(&canvas, {90, 150});
        click(&canvas, {110, 150});
        click(&canvas, {100, 162});
        check(doc.elements().size() == ++n && isRect({100, 156}, 20, 12), "3-point axis rectangle");
        click(&canvas, {130, 150});
        click(&canvas, {140, 160});
        click(&canvas, {130, 165});
        check(doc.elements().size() == ++n
                  && last().value("geometryType").toString() == QLatin1String("path"),
              "3-point tilted rectangle becomes a closed path");

        // A 40 mm square to be tangent to: edges x 280..320, y 280..320.
        canvas.setRectMode(C::Rect2Point);
        drag(&canvas, {280, 280}, {320, 320});
        n = doc.elements().size();
        canvas.setTool(C::DrawCircle);
        canvas.setCircleMode(C::Circle2Tangent);
        click(&canvas, {300, 280});                 // bottom edge
        click(&canvas, {280, 300});                 // left edge
        check(doc.elements().size() == n, "2-tangent waits for placement");
        click(&canvas, {286, 286});
        check(doc.elements().size() == ++n && isCircle({286, 286}, 6), "2-tangent circle in the corner");

        canvas.setCircleMode(C::Circle3Tangent);
        click(&canvas, {310, 280});                 // bottom
        click(&canvas, {280, 310});                 // left
        click(&canvas, {310, 320});                 // top
        click(&canvas, {298, 300});
        check(doc.elements().size() == ++n && isCircle({300, 300}, 20), "3-tangent circle");

        click(&canvas, {600, 600});                 // empty space: no edge
        QTest::keyClick(&canvas, Qt::Key_Escape);
        check(doc.elements().size() == n, "missed edge pick and Esc add nothing");
    }

    // --- Area tool: click inside a closed shape selects it and its islands ----------
    {
        c2d::Document adoc;
        c2d::Canvas ac;
        ac.resize(900, 700);
        ac.show();
        ac.setDocument(&adoc);
        ac.resetTransform();
        ac.scale(3, -3);
        ac.centerOn(QPointF(100, 100));
        const c2d::Element outer = c2d::Element::makeRectangle({100, 100}, 120, 100, adoc.defaultLayer());
        const c2d::Element hole = c2d::Element::makeCircle({80, 100}, 15, adoc.defaultLayer());
        const c2d::Element inHole = c2d::Element::makeCircle({80, 100}, 5, adoc.defaultLayer());
        const c2d::Element other = c2d::Element::makeRectangle({300, 300}, 20, 20, adoc.defaultLayer());
        for (const auto &e : {outer, hole, inHole, other})
            adoc.addElement(e);
        ac.rebuild();
        ac.setTool(c2d::Canvas::SelectArea);
        click(&ac, {130, 100});                     // inside the rectangle, outside the hole
        QStringList got = ac.selectedElementIds();
        got.sort();
        QStringList want{outer.id, hole.id};
        want.sort();
        check(got == want, "area click selects the rectangle and its island, not the nested one");
        click(&ac, {80, 100});                      // inside the small circle
        check(ac.selectedElementIds() == QStringList{inHole.id}, "area click picks the innermost shape");
        QTest::mouseClick(ac.viewport(), Qt::LeftButton, Qt::ShiftModifier, vp(&ac, {300, 300}));
        got = ac.selectedElementIds();
        got.sort();
        want = {inHole.id, other.id};
        want.sort();
        check(got == want, "Shift adds the next area");
        click(&ac, {600, 600});                     // empty space
        check(ac.selectedElementIds().isEmpty(), "area click on nothing clears");
        // Two overlapping squares: like a fill, a click in the overlap picks
        // just the overlap (added as a closed path), not both squares.
        const c2d::Element sa = c2d::Element::makeRectangle({200, 100}, 40, 40, adoc.defaultLayer());   // 180..220
        const c2d::Element sb = c2d::Element::makeRectangle({230, 100}, 40, 40, adoc.defaultLayer());   // 210..250
        adoc.addElement(sa);
        adoc.addElement(sb);
        ac.rebuild();
        const int before = int(adoc.elements().size());
        click(&ac, {215, 100});
        got = ac.selectedElementIds();
        check(got.size() == 1 && got.first() != sa.id && got.first() != sb.id
                  && int(adoc.elements().size()) == before + 1,
              "area click in an overlap selects just the overlap");
        if (const c2d::Element *f = got.isEmpty() ? nullptr : adoc.elementById(got.first())) {
            const QRectF b = f->painterPath.boundingRect();
            check(std::fabs(b.left() - 210) < 0.05 && std::fabs(b.right() - 220) < 0.05
                      && std::fabs(b.height() - 40) < 0.05,
                  "the overlap is 10 x 40");
        }
        click(&ac, {190, 100});                     // only in the left square
        got = ac.selectedElementIds();
        check(got.size() == 1 && !got.contains(sa.id), "area click beside an overlap leaves the overlap out");
        click(&ac, {300, 300});                     // a plain shape: no new path
        check(ac.selectedElementIds() == QStringList{other.id} && int(adoc.elements().size()) == before + 2,
              "area click on a lone shape selects that shape");
    }

    // --- a CC rectangle's corner style survives a resize --------------------------------
    // Element::regen rebuilt every rectangle as makeRectangle's square: a rounded
    // one lost its corners on the first drag of the resize handle or W x H edit.
    {
        const double k = 0.5522847498307936 * 5;   // 5 mm corners, as CC rounds them
        c2d::SubPath sp;
        sp.closed = true;
        auto node = [](QPointF p, QPointF in, QPointF out) {
            c2d::PathNode n;
            n.p = p; n.in = in; n.out = out;
            return n;
        };
        sp.nodes = {node({20, -5}, {20, -5 - k}, {20, -5}), node({20, 5}, {20, 5}, {20, 5 + k}),
                    node({15, 10}, {15 + k, 10}, {15, 10}), node({-15, 10}, {-15, 10}, {-15 - k, 10}),
                    node({-20, 5}, {-20, 5 + k}, {-20, 5}), node({-20, -5}, {-20, -5}, {-20, -5 - k}),
                    node({-15, -10}, {-15 - k, -10}, {-15, -10}), node({15, -10}, {15, -10}, {15 + k, -10})};
        c2d::PathModel m;
        m.subs.append(sp);
        // Rows relative to the center, as CC stores a rectangle.
        QJsonObject o = c2d::Element::makeRectangle({50, 50}, 40, 20, doc.defaultLayer()).raw;
        const QJsonObject rows = c2d::Element::makeBezierPath(m, doc.defaultLayer()).raw;
        for (const char *key : {"points", "cp1", "cp2", "point_type", "smooth"})
            o.insert(QLatin1String(key), rows.value(QLatin1String(key)));
        o.insert("corner_type", 1);
        o.insert("radius", 5.0);
        const c2d::Element rounded = c2d::Element::fromJson(o);
        check(near(rounded.painterPath.boundingRect().topLeft(), {30, 40}), "rounded rect specimen in place");

        const c2d::Element big = c2d::Element::regen(rounded, {{"width", 60.0}, {"height", 30.0}});
        check(big.geometryType == QLatin1String("rectangle") && big.id == rounded.id
              && big.raw.value("corner_type").toInt() == 1 && big.raw.value("radius").toDouble() == 5.0,
              "resized rounded rect stays a rectangle with its corner style");
        const QRectF bb = big.painterPath.boundingRect();
        check(std::fabs(bb.width() - 60) < 1e-6 && std::fabs(bb.height() - 30) < 1e-6
              && near(bb.center(), {50, 50}), "rounded rect resized about its center");
        const c2d::PathModel bm = c2d::Element::pathModel(big);
        bool corner = false, edge = false;
        for (const c2d::PathNode &n : bm.subs.value(0).nodes) {
            corner = corner || (near(n.p, {80, 40}) && near(n.in, {80, 40 - k}));
            edge = edge || near(n.p, {75, 35});
        }
        check(bm.subs.size() == 1 && bm.subs.first().nodes.size() == 8 && corner && edge,
              "the 5 mm corner arcs keep their radius");

        // Too small for its corners: scaled, as a path, still the size asked for.
        const c2d::Element tiny = c2d::Element::regen(rounded, {{"width", 8.0}});
        const QRectF tb = tiny.painterPath.boundingRect();
        check(tiny.geometryType == QLatin1String("path") && tiny.id == rounded.id
              && std::fabs(tb.width() - 8) < 1e-6 && std::fabs(tb.height() - 20) < 1e-6,
              "a rect too small for its corners is scaled instead");

        // Square corners still regenerate parametrically.
        const c2d::Element sq = c2d::Element::makeRectangle({0, 0}, 10, 10, doc.defaultLayer());
        check(c2d::Element::regen(sq, {{"width", 20.0}}).painterPath.boundingRect().width() == 20,
              "square rect regen unchanged");
    }

    // --- the background toolpath preview never interrupts a drag ---------------------
    // setToolpathPreview used to rebuild the whole scene: a select drag lost
    // its item, a node drag its grab, the pen tool its outline.
    {
        c2d::Document pdoc;
        c2d::Canvas pc;
        pc.resize(900, 700);
        pc.show();
        pc.setDocument(&pdoc);
        pc.resetTransform();
        pc.scale(3, -3);
        pc.centerOn(QPointF(100, 100));
        const c2d::Element box = c2d::Element::makeRectangle({100, 100}, 20, 20, pdoc.defaultLayer());
        pdoc.addElement(box);
        pc.rebuild();
        const QVector<c2d::Op> ops{c2d::Op::rapid(0, 0, 5), c2d::Op::feedTo(0, 0, -1, 100),
                                   c2d::Op::feedTo(50, 0, -1, 100)};
        const int bare = pc.scene()->items().size();
        pc.setToolpathPreview(ops);
        check(pc.scene()->items().size() > bare && pc.hasToolpathPreview(), "preview overlay drawn");

        pc.setTool(c2d::Canvas::Select);
        QTest::mousePress(pc.viewport(), Qt::LeftButton, Qt::NoModifier, vp(&pc, {95, 95}));
        QTest::mouseMove(pc.viewport(), vp(&pc, {100, 95}));
        QTest::mouseMove(pc.viewport(), vp(&pc, {105, 95}));
        pc.setToolpathPreview(ops);                 // the model panel finished a rebuild
        QTest::mouseMove(pc.viewport(), vp(&pc, {115, 95}));
        QTest::mouseRelease(pc.viewport(), Qt::LeftButton, Qt::NoModifier, vp(&pc, {115, 95}));
        check(near(pdoc.elementById(box.id)->painterPath.boundingRect().center(), {120, 100}, 0.5),
              "select drag survives a preview refresh");

        const c2d::Element line = c2d::Element::makePath({{50, 150}, {150, 150}}, false, pdoc.defaultLayer());
        pdoc.addElement(line);
        pc.rebuild();
        pc.selectElements({line.id});
        pc.setTool(c2d::Canvas::NodeEdit);
        QTest::mousePress(pc.viewport(), Qt::LeftButton, Qt::NoModifier, vp(&pc, {150, 150}));
        QTest::mouseMove(pc.viewport(), vp(&pc, {150, 160}));
        pc.setToolpathPreview(ops);
        QTest::mouseMove(pc.viewport(), vp(&pc, {150, 170}));
        QTest::mouseRelease(pc.viewport(), Qt::LeftButton, Qt::NoModifier, vp(&pc, {150, 170}));
        check(near(row(pdoc.elementById(line.id)->raw, "points", 1), {150, 170}, 0.5),
              "node drag survives a preview refresh");

        pc.selectElements({});
        pc.setTool(c2d::Canvas::DrawPath);
        const int elems = pdoc.elements().size();
        click(&pc, {20, 20});
        pc.setToolpathPreview(ops);
        click(&pc, {40, 20});
        QTest::keyClick(&pc, Qt::Key_Return);
        check(pdoc.elements().size() == elems + 1
              && c2d::Element::pathModel(pdoc.elements().last()).subs.value(0).nodes.size() == 2,
              "pen path survives a preview refresh");
        pc.clearToolpathPreview();
        check(!pc.hasToolpathPreview() && pc.scene()->items().size() == pdoc.elements().size(),
              "clearing the preview removes only the overlay");

        // A second press while a resize is live replaces its outline, never orphans it.
        pc.setTool(c2d::Canvas::Select);
        pc.selectElements({box.id});
        auto loose = [&] {
            int n = 0;
            for (QGraphicsItem *it : pc.scene()->items())
                if (!it->data(0).isValid())
                    ++n;
            return n;
        };
        const QPointF handle(130, 110);              // top-right corner of the moved box
        QTest::mousePress(pc.viewport(), Qt::LeftButton, Qt::NoModifier, vp(&pc, handle));
        const int one = loose();
        QTest::mousePress(pc.viewport(), Qt::LeftButton, Qt::NoModifier, vp(&pc, handle));
        check(one == 1 && loose() == 1, "re-grabbing the resize handle keeps one outline");
        QTest::mouseRelease(pc.viewport(), Qt::LeftButton, Qt::NoModifier, vp(&pc, {135, 115}));
        check(loose() == 0 && std::fabs(pdoc.elementById(box.id)->raw.value("width").toDouble() - 30) < 0.5,
              "resize commits and drops its outline");
    }

    // --- open paths are picked within a few screen pixels at any zoom -----------------
    {
        c2d::Document hdoc;
        c2d::Canvas hc;
        hc.resize(900, 700);
        hc.show();
        hc.setDocument(&hdoc);
        hc.resetTransform();
        hc.scale(3, -3);
        hc.centerOn(QPointF(100, 100));
        const c2d::Element line = c2d::Element::makePath({{50, 100}, {150, 100}}, false, hdoc.defaultLayer());
        const c2d::Element pt = c2d::Element::makePoint({100, 130}, hdoc.defaultLayer());
        hdoc.addElement(line);
        hdoc.addElement(pt);
        hc.rebuild();
        hc.setTool(c2d::Canvas::Select);
        click(&hc, {100, 101.7});                    // 5 px off at 3 px/mm
        check(hc.selectedElementIds() == QStringList{line.id}, "open path picked 5 px off");
        click(&hc, {100, 104});                      // 12 px off
        check(hc.selectedElementIds().isEmpty(), "open path not picked 12 px off");
        click(&hc, {101.5, 130});                    // 4.5 px off the point
        check(hc.selectedElementIds() == QStringList{pt.id}, "sketch point picked a few px off");

        hc.resetTransform();
        hc.scale(0.5, -0.5);                         // zoomed far out
        hc.centerOn(QPointF(100, 100));
        click(&hc, {100, 110});                      // 5 px off
        check(hc.selectedElementIds() == QStringList{line.id}, "zoomed out: still picked 5 px off");
        hc.resetTransform();
        hc.scale(40, -40);                           // zoomed far in
        hc.centerOn(QPointF(100, 100));
        click(&hc, {100, 100.5});                    // 20 px off
        check(hc.selectedElementIds().isEmpty(), "zoomed in: not picked 20 px off");

        // Node editor: a double-click 5 px off the open path, inside the item's
        // corridor and the node editor's segment tolerance alike, inserts a node.
        hc.resetTransform();
        hc.scale(3, -3);
        hc.centerOn(QPointF(100, 100));
        hc.selectElements({line.id});
        hc.setTool(c2d::Canvas::NodeEdit);
        QTest::mouseDClick(hc.viewport(), Qt::LeftButton, Qt::NoModifier, vp(&hc, {100, 101.7}));
        check(hdoc.elementById(line.id)->raw.value("points").toArray().size() == 3,
              "double-click near an open path inserts a node");
    }

    // --- delete + undo keeps the z-order; the inlay generator is undoable ------------
    {
        c2d::Document zdoc;
        c2d::Canvas zc;
        zc.setDocument(&zdoc);
        QVector<c2d::Element> els;
        for (int i = 0; i < 4; ++i) {
            els.append(c2d::Element::makeCircle({10.0 + 20 * i, 10}, 5, zdoc.defaultLayer()));
            zdoc.addElement(els.last());
        }
        auto order = [&] {
            QStringList ids;
            for (const c2d::Element &e : zdoc.elements())
                ids << e.id;
            return ids;
        };
        zc.rebuild();
        const QStringList before = order();
        zc.selectElements({els.at(2).id, els.at(0).id});
        zc.deleteSelected();
        check(order() == QStringList({els.at(1).id, els.at(3).id}), "delete removes the selection");
        zc.undoStack()->undo();
        check(order() == before, "undo delete restores the original order");
        zc.undoStack()->redo();
        zc.undoStack()->undo();
        check(order() == before, "redo + undo delete keeps the order");

        const int steps = zc.undoStack()->index();
        const c2d::Element gen = c2d::Element::makeRectangle({50, 50}, 10, 10, zdoc.defaultLayer());
        c2d::Toolpath male;
        male.uuid = QStringLiteral("{male}");
        male.json.insert("name", QStringLiteral("pocket (inlay male)"));
        male.json.insert("elements", QJsonArray{QJsonObject{{"uuid", gen.id}}});
        zc.insertGenerated({gen}, male);
        check(zc.undoStack()->index() == steps + 1 && zdoc.elementById(gen.id)
              && zdoc.toolpathByUuid(male.uuid), "inlay male is one undo step");
        zc.undoStack()->undo();
        check(!zdoc.elementById(gen.id) && !zdoc.toolpathByUuid(male.uuid) && order() == before,
              "undo removes the generated vectors and toolpath");
        zc.undoStack()->redo();
        check(zdoc.elementById(gen.id) && zdoc.toolpathByUuid(male.uuid), "redo puts them back");
    }

    // --- properties: sizes stay positive, the center may go negative ------------------
    {
        c2d::PropertiesPanel panel(&canvas);
        int negative = 0, positive = 0;
        for (QDoubleSpinBox *s : panel.findChildren<QDoubleSpinBox *>()) {
            if (s->suffix() != QStringLiteral(" mm") || s->maximum() != 10000 || s->decimals() != 3)
                continue;
            if (s->minimum() < 0)
                ++negative;
            else if (s->minimum() > 0)
                ++positive;
        }
        check(negative == 2 && positive == 3,
              "center x/y go negative; radius, width and height have a positive minimum");
    }

    std::printf("OK: %d checks passed\n", g_checks);
    return 0;
}
