// Headless interaction checks for the canvas tools (offscreen platform,
// synthetic mouse/key events through QTest):
// - pen tool: click = corner, click-drag = symmetric curve node, Enter
//   finishes open, clicking the start closes; each path is one undo step;
// - node editor: anchor drag, double-click insert, Delete remove — one undo
//   step each, the document JSON carries the edited control points;
// - convert-to-path and text edits are undoable;
// - the toolpath preview overlay: painted, never picked, a strip per bit, the
//   last ring of a pocket; the grid and the zoom bar; the properties panel
//   writes only the field that was edited.
//
// Plain asserts, no test framework; exits 0 on success.

// The pinch's start (pinchStarted) is private: a real pinch gesture cannot
// be made in a test.
#define private public
#include "../src/canvas.h"
#undef private
#include "../src/c2ddocument.h"
#include "../src/element.h"
#include "../src/model3d.h"
#include "../src/propertiespanel.h"
#include "../src/shapestrip.h"
#include "../src/parametric.h"
#include "../src/toolpathfactory.h"

#include <QApplication>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QContextMenuEvent>
#include <QListWidgetItem>
#include <QMenu>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QTemporaryDir>
#include <QTimer>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <QTest>
#include <QUndoStack>
#include <QWheelEvent>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

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

// A mouse move with no button down, straight to this canvas: QTest moves the
// cursor for that, which reaches whichever window comes first under it.
static void hoverAt(c2d::Canvas *c, QPointF mm)
{
    const QPoint p = vp(c, mm);
    QMouseEvent me(QEvent::MouseMove, QPointF(p), QPointF(c->viewport()->mapToGlobal(p)), Qt::NoButton,
                   Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(c->viewport(), &me);
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

    // --- a sketch point is never node-edited or converted into a path ----------------------
    {
        const c2d::Element pt = c2d::Element::makePoint({250, 250}, doc.defaultLayer());
        doc.addElement(pt);
        canvas.rebuild();
        canvas.convertToPaths({pt.id});
        check(c2d::Element::isPoint(*doc.elementById(pt.id)), "convert to path leaves a sketch point a point");
        canvas.setTool(c2d::Canvas::NodeEdit);
        canvas.selectElements({pt.id});
        const double r = pt.raw.value("radius").toDouble();
        drag(&canvas, {250 + r, 250}, {260, 250});
        check(c2d::Element::isPoint(*doc.elementById(pt.id))
                  && std::abs(doc.elementById(pt.id)->raw.value("radius").toDouble() - r) < 1e-9,
              "node edit leaves a sketch point a point");
        canvas.setTool(c2d::Canvas::Select);
        canvas.selectElements({});
        doc.removeElementById(pt.id);   // the counts below are of the shapes before it
        canvas.rebuild();
    }

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
        // A construction line in a group: every broken piece stays both.
        QJsonObject hj = c2d::Element::makePath({{0, 50}, {100, 50}}, false, layer).raw;
        hj.insert("construction", true);
        hj.insert("group_id", "g-break");
        const c2d::Element h = c2d::Element::fromJson(hj);
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
        int kept = 0;
        for (const c2d::Element &e : d2.elements())
            kept += c2d::Element::isConstruction(e) && e.raw.value("group_id").toString() == "g-break";
        check(kept == 3, "broken pieces keep construction and the group");
        u->undo();
        check(d2.elements().size() == 4, "undo break");

        cv.setTool(c2d::Canvas::Extend);
        click(&cv, {19, 20});
        check(near(c2d::Element::pathModel(*d2.elementById(shortLine.id)).subs[0].nodes.last().p, {30, 20}),
              "extend runs the short line on to the first vertical");
        const int steps = u->count();
        click(&cv, {90, 90});
        check(u->count() == steps, "clicking empty space does nothing");

        // The hover outline decodes a path once and keeps it until the scene
        // is rebuilt (it decoded every element on every mouse move). After an
        // edit and after its undo it is the piece between the lines as they
        // are now, and a click cuts exactly that.
        cv.setTool(c2d::Canvas::Trim);
        auto hover = [&](QPointF at) {
            hoverAt(&cv, at + QPointF(1, 1));
            hoverAt(&cv, at);
            for (QGraphicsItem *it : cv.scene()->items())
                if (auto *pi = qgraphicsitem_cast<QGraphicsPathItem *>(it);
                    pi && !pi->data(0).isValid() && pi->pen().color() == QColor(0xe0, 0x50, 0x50))
                    return pi->path().boundingRect();
            return QRectF();
        };
        auto spans = [](const QRectF &r, double x0, double x1) {
            return std::fabs(r.left() - x0) < 1e-6 && std::fabs(r.right() - x1) < 1e-6;
        };
        auto cutOut = [&](double x0, double x1) {
            const c2d::PathModel t = c2d::Element::pathModel(*d2.elementById(h.id));
            return t.subs.size() == 2 && near(t.subs[0].nodes.last().p, {x0, 50})
                   && near(t.subs[1].nodes.first().p, {x1, 50});
        };
        check(spans(hover({50, 50}), 30, 70), "trim hover: the piece between the verticals");
        cv.moveElementBy(v1.id, 10, 0);
        check(spans(hover({50, 50}), 40, 70), "trim hover: follows a line moved since");
        u->undo();
        check(spans(hover({50, 50}), 30, 70), "trim hover: and the undo of the move");
        u->redo();
        check(spans(hover({50, 50}), 40, 70), "trim hover: and its redo");
        click(&cv, {50, 50});
        check(cutOut(40, 70), "trim after a move cuts at the moved line");
        u->undo();                                   // the trim
        u->undo();                                   // the move
        check(spans(hover({50, 50}), 30, 70), "trim hover: back to the start");
        // A click decodes afresh, whatever a hover decoded before it: here a
        // line changes in the document with no rebuild of the scene between.
        QJsonObject shifted = c2d::Element::makePath({{45, 0}, {45, 100}}, false, layer).raw;
        shifted.insert("id", v1.raw.value("id"));
        check(d2.replaceElement(c2d::Element::fromJson(shifted)), "the first vertical is replaced");
        click(&cv, {50, 50});
        check(cutOut(45, 70), "a trim click never cuts by paths decoded for an earlier hover");
        u->undo();
        d2.replaceElement(v1);
        cv.rebuild();

        // Break a line a 3D "From vectors" component is built on: whoever
        // hears documentChanged finds the component on the pieces already.
        // Redo announced the change first and moved the component after it,
        // so a listener (the model's rebuild) read the old vectors.
        {
            c2d::ModelStore store;
            store.attach(&d2);
            c2d::ModelComponent comp;
            comp.id = c2d::Model3D::newId();
            comp.vectorIds = QStringList{h.id};
            store.model.components << comp;
            QList<QStringList> heard;
            const QMetaObject::Connection conn =
                QObject::connect(&cv, &c2d::Canvas::documentChanged, &cv, [&] {
                    QStringList ids = store.model.components.first().vectorIds;
                    ids.sort();
                    heard << ids;
                });
            auto allHeard = [&](const QStringList &want) {
                bool ok = !heard.isEmpty();
                for (const QStringList &ids : std::as_const(heard))
                    ok = ok && ids == want;
                heard.clear();
                return ok;
            };
            cv.setTool(c2d::Canvas::Break);
            click(&cv, {50, 50});
            QStringList pieces;
            for (const c2d::Element &e : d2.elements())
                if (e.raw.value("group_id").toString() == "g-break")
                    pieces << e.id;
            pieces.sort();
            check(pieces.size() == 3 && pieces.contains(h.id), "the line is broken in three");
            check(allHeard(pieces), "break: a listener finds the 3D component on all the pieces");
            u->undo();
            check(d2.elements().size() == 4 && allHeard({h.id}), "undo break: and on the whole line again");
            u->redo();
            check(allHeard(pieces), "redo break: on the pieces again");
            u->undo();
            QObject::disconnect(conn);
        }
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
        // A small boss in a big pocket: the boss is an island, never dropped
        // as a "sliver" of the outline around it.
        const c2d::Element pk = c2d::Element::makeRectangle({100, 300}, 100, 100, adoc.defaultLayer());
        const c2d::Element boss = c2d::Element::makeCircle({100, 300}, 2.5, adoc.defaultLayer());
        adoc.addElement(pk);
        adoc.addElement(boss);
        ac.rebuild();
        click(&ac, {70, 300});
        got = ac.selectedElementIds();
        got.sort();
        want = {pk.id, boss.id};
        want.sort();
        check(got == want, "area click keeps a small boss as an island");
        // A lone shape with sharp tips (a 16-degree diamond) is selected,
        // not copied: fillFace() blunts the tips a little.
        {
            const double h = 50 * std::tan(8 * M_PI / 180);
            const c2d::Element dia = c2d::Element::makePath({{300, 450}, {350, 450 + h}, {400, 450}, {350, 450 - h}},
                                                            true, adoc.defaultLayer());
            adoc.addElement(dia);
            ac.rebuild();
            const int n = int(adoc.elements().size());
            click(&ac, {350, 450});
            check(ac.selectedElementIds() == QStringList{dia.id} && int(adoc.elements().size()) == n,
                  "area click on a sharp diamond selects it");
        }
        // The same diamond as an island in a pocket: the pocket and the
        // diamond are selected, nothing is added.
        {
            const double h = 30 * std::tan(8 * M_PI / 180);
            const c2d::Element pocket = c2d::Element::makeRectangle({600, 450}, 120, 100, adoc.defaultLayer());
            const c2d::Element isl = c2d::Element::makePath({{570, 450}, {600, 450 + h}, {630, 450}, {600, 450 - h}},
                                                            true, adoc.defaultLayer());
            adoc.addElement(pocket);
            adoc.addElement(isl);
            ac.rebuild();
            const int n = int(adoc.elements().size());
            click(&ac, {600, 480});
            QStringList sel = ac.selectedElementIds(), exp{pocket.id, isl.id};
            sel.sort();
            exp.sort();
            check(sel == exp && int(adoc.elements().size()) == n, "area click keeps a sharp diamond as an island");
        }
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

    // --- the toolpath preview: painted, never picked; a strip per bit; the last ring ---
    {
        c2d::Document odoc;
        c2d::Canvas oc;
        oc.resize(900, 700);
        oc.show();
        oc.setDocument(&odoc);
        oc.resetTransform();
        oc.scale(3, -3);
        oc.centerOn(QPointF(100, 100));
        auto bit = [](int n, double dia) { c2d::Op o = c2d::Op::tool(n); o.x = dia; return o; };
        // The overlay's items at one height: rapids 5, swept strips 5.5, cut lines 6.
        auto overlay = [&](double z) {
            QList<QGraphicsPathItem *> out;
            for (QGraphicsItem *it : oc.scene()->items())
                if (!it->data(0).isValid() && it->zValue() == z)
                    if (auto *pi = qgraphicsitem_cast<QGraphicsPathItem *>(it))
                        out << pi;
            return out;
        };
        // Overlay items a hit test finds at a point: by the view, by the scene,
        // and by a small rectangle around it.
        auto picked = [&](QPointF mm) {
            int n = 0;
            for (QGraphicsItem *it : oc.items(vp(&oc, mm)))
                n += !it->data(0).isValid();
            for (QGraphicsItem *it : oc.scene()->items(mm))
                n += !it->data(0).isValid();
            for (QGraphicsItem *it : oc.scene()->items(QRectF(mm.x() - 0.5, mm.y() - 0.5, 1, 1)))
                n += !it->data(0).isValid();
            return n;
        };
        QImage shot;
        auto grab = [&] {
            QApplication::processEvents();
            shot = oc.viewport()->grab().toImage();
        };
        auto px = [&](QPointF mm, QPoint off = {}) {
            return shot.pixelColor((vp(&oc, mm) + off) * shot.devicePixelRatio()).rgb();
        };
        const QRgb yellow = QColor(0xff, 0xd6, 0x00).rgb();
        // A cut line (orange to red) on the yellow strip, within a pixel of the point.
        auto cutLineAt = [&](QPointF mm) {
            for (int dy = -1; dy <= 1; ++dy) {
                const QColor c(px(mm, {0, dy}));
                if (c.red() > 150 && c.green() < 170 && c.blue() < 80)
                    return true;
            }
            return false;
        };

        // A 3 mm bit cuts along y = 40, then a 6 mm bit along y = 80; the
        // rapid between them crosses (50, 60).
        oc.setToolpathPreview({bit(1, 3), c2d::Op::rapid(20, 40, 5), c2d::Op::feedTo(20, 40, -1, 100),
                               c2d::Op::feedTo(80, 40, -1, 100), c2d::Op::rapid(80, 40, 5),
                               bit(2, 6), c2d::Op::rapid(20, 80, 5), c2d::Op::feedTo(20, 80, -2, 100),
                               c2d::Op::feedTo(80, 80, -2, 100), c2d::Op::rapid(80, 80, 5)});
        check(overlay(5).size() == 1 && !overlay(6).isEmpty(), "preview: rapids and cut lines are in the scene");
        check(picked({50, 40}) == 0 && picked({50, 80}) == 0 && picked({50, 60}) == 0
                  && oc.itemAt(vp(&oc, {50, 40})) == nullptr,
              "preview: no hit test finds a strip, a cut line or a rapid");
        // The overlay answers no hit test, so its bounds are its own: they
        // have to hold all that is painted, or it is not painted at all.
        {
            QVector<double> widths;
            bool holds = true;
            for (QGraphicsPathItem *s : overlay(5.5)) {
                const double w = s->pen().widthF();
                widths << w;
                holds = holds && s->boundingRect().contains(
                            s->path().boundingRect().adjusted(-w / 2, -w / 2, w / 2, w / 2));
                // Each strip holds the cuts of its own bit only.
                holds = holds && s->path().boundingRect().center().y() == (w == 3 ? 40 : 80);
            }
            std::sort(widths.begin(), widths.end());
            check(widths == QVector<double>({3, 6}), "preview: each bit sweeps a strip of its own width");
            for (double z : {5.0, 6.0})
                for (QGraphicsPathItem *it : overlay(z))
                    holds = holds && it->boundingRect().contains(it->path().boundingRect())
                            && !it->boundingRect().isEmpty();
            check(holds, "preview: every overlay item's bounds hold what it paints");
        }
        grab();
        check(px({50, 41}) == yellow && px({50, 39}) == yellow, "preview: the 3 mm bit's strip is painted");
        check(px({50, 42.5}) != yellow && px({50, 37.5}) != yellow,
              "preview: and is 3 mm wide, not as wide as the 6 mm bit that follows");
        check(px({50, 82.5}) == yellow && px({50, 77.5}) == yellow && px({50, 84}) != yellow,
              "preview: the 6 mm bit's strip is 6 mm wide");
        check(cutLineAt({50, 40}) && cutLineAt({50, 80}), "preview: the cut lines are painted on the strips");
        {
            // Dashed: some of the pixels along it, none beside it.
            int on = 0, beside = 0;
            for (int k = 20; k <= 80; ++k) {
                const QPointF at(80 - 0.6 * k, 40 + 0.4 * k);
                on += px(at) != px({60, 20});
                beside += px(at + QPointF(2, 3)) != px({60, 20});
            }
            check(on > 10 && beside == 0, "preview: the rapid is painted");
        }

        // Every ring as a line (simplified preview off): painted and not picked either.
        oc.setSimplePreview(false);
        grab();
        check(overlay(5.5).isEmpty() && px({50, 41}) != yellow, "full preview: no strips");
        check(px({50, 40}) != px({50, 20}) || px({50, 40}, {0, 1}) != px({50, 20})
                  || px({50, 40}, {0, -1}) != px({50, 20}),
              "full preview: the cut line is painted");
        check(picked({50, 40}) == 0 && picked({50, 60}) == 0, "full preview: nothing of it is picked");
        oc.setSimplePreview(true);

        // A shape under the overlay is picked through it.
        const c2d::Element under = c2d::Element::makeRectangle({50, 40}, 30, 6, odoc.defaultLayer());
        odoc.addElement(under);
        oc.rebuild();
        check(overlay(5.5).size() == 2 && picked({50, 40}) == 0, "preview: redrawn with the scene, still not picked");
        oc.setTool(c2d::Canvas::Select);
        click(&oc, {50, 40});
        check(oc.selectedElementIds() == QStringList{under.id}, "preview: a click on a strip takes the shape under it");
        odoc.removeElementById(under.id);
        oc.rebuild();

        // One width all through (the bit named again, and a bit of no known
        // width): one strip, as before.
        oc.setToolpathPreview({bit(1, 3), c2d::Op::rapid(20, 40, 5), c2d::Op::feedTo(20, 40, -1, 100),
                               c2d::Op::feedTo(80, 40, -1, 100), c2d::Op::rapid(80, 40, 5),
                               bit(1, 3), c2d::Op::tool(2), c2d::Op::rapid(20, 80, 5),
                               c2d::Op::feedTo(20, 80, -1, 100), c2d::Op::feedTo(80, 80, -1, 100)});
        check(overlay(5.5).size() == 1 && overlay(5.5).first()->pen().widthF() == 3
                  && overlay(5.5).first()->path().boundingRect().height() == 40,
              "preview: one bit, one strip over all its cuts");

        // A pocket: n square rings around (50, 50), ring i with a half side
        // of 5 + 5 i, each closed and linked to the next with the bit down.
        // Returns the rings the simplified preview draws as lines.
        auto ringsDrawn = [&](int n, bool lastLeftOpen = false) {
            QVector<c2d::Op> r{bit(1, 3), c2d::Op::rapid(45, 45, 5), c2d::Op::feedTo(45, 45, -1, 100)};
            for (int i = 0; i < n; ++i) {
                const double h = 5 + 5 * i;
                if (i > 0)
                    r << c2d::Op::feedTo(50 - h, 50 - h, -1, 100);       // the link
                r << c2d::Op::feedTo(50 + h, 50 - h, -1, 100) << c2d::Op::feedTo(50 + h, 50 + h, -1, 100);
                if (lastLeftOpen && i == n - 1)
                    break;
                r << c2d::Op::feedTo(50 - h, 50 + h, -1, 100) << c2d::Op::feedTo(50 - h, 50 - h, -1, 100);
            }
            r << c2d::Op::rapid(r.last().x, r.last().y, 5);
            oc.setToolpathPreview(r);
            QVector<int> drawn;
            for (int i = 0; i < n; ++i) {
                const QPointF corner(55 + 5 * i, 55 + 5 * i);
                bool on = false;
                for (QGraphicsPathItem *it : overlay(6)) {
                    const QPainterPath p = it->path();
                    for (int k = 0; k < p.elementCount(); ++k)
                        on = on || near(QPointF(p.elementAt(k)), corner);
                }
                if (on)
                    drawn << i;
            }
            return drawn;
        };
        check(ringsDrawn(1) == QVector<int>({0}), "rings: a single ring is drawn");
        check(ringsDrawn(2) == QVector<int>({0, 1}), "rings: of two, both (the last was dropped)");
        check(ringsDrawn(4) == QVector<int>({0, 3}), "rings: of four, the first and the last");
        check(ringsDrawn(6) == QVector<int>({0, 5}), "rings: of six, the first and the last, a fifth one");
        check(ringsDrawn(7) == QVector<int>({0, 5, 6}), "rings: of seven, the first, every fifth and the last");
        check(ringsDrawn(12) == QVector<int>({0, 5, 10, 11}), "rings: of twelve likewise");
        check(ringsDrawn(4, true) == QVector<int>({0, 3}), "rings: a last ring that does not close is drawn too");
        {
            // The yellow strip is under all of them all the same.
            ringsDrawn(4);
            const QList<QGraphicsPathItem *> strips = overlay(5.5);
            check(strips.size() == 1 && strips.first()->path().boundingRect() == QRectF(30, 30, 40, 40),
                  "rings: the strip covers every ring");
            grab();
            check(px({60, 50}) == yellow && px({65, 50}) == yellow && cutLineAt({50, 70}) && !cutLineAt({50, 65}),
                  "rings: the rings between are yellow only, the last one has its line");
        }
        oc.setSimplePreview(false);
        check(ringsDrawn(4) == QVector<int>({0, 1, 2, 3}), "rings: the full preview draws every ring");
        oc.setSimplePreview(true);
        // A cut that never closes (a contour) is drawn whole.
        oc.setToolpathPreview({bit(1, 3), c2d::Op::rapid(20, 20, 5), c2d::Op::feedTo(20, 20, -1, 100),
                               c2d::Op::feedTo(60, 20, -1, 100), c2d::Op::feedTo(60, 50, -1, 100),
                               c2d::Op::feedTo(90, 50, -1, 100), c2d::Op::rapid(90, 50, 5)});
        {
            int corners = 0;
            for (QGraphicsPathItem *it : overlay(6)) {
                const QPainterPath p = it->path();
                for (int k = 0; k < p.elementCount(); ++k)
                    if (p.elementAt(k).isLineTo())
                        corners += near(QPointF(p.elementAt(k)), {60, 20}) + near(QPointF(p.elementAt(k)), {60, 50})
                                   + near(QPointF(p.elementAt(k)), {90, 50});
            }
            check(corners == 3, "rings: an open cut is drawn whole");
        }
        oc.clearToolpathPreview();
        check(overlay(5).isEmpty() && overlay(5.5).isEmpty() && overlay(6).isEmpty(),
              "preview: cleared, nothing of it is left");
    }

    // --- the grid: only the lines in the exposed part are walked; the same picture ---
    {
        c2d::Document gdoc;
        gdoc.setParam("width", "100");
        gdoc.setParam("height", "80");
        c2d::Canvas gc;
        gc.resize(900, 700);
        gc.show();
        gc.setDocument(&gdoc);
        check(gdoc.boardWidth() == 100 && gdoc.boardHeight() == 80, "grid: a 100 x 80 board");
        for (double zoom : {3.0, 1.0}) {             // 5 mm lines 15 px apart, then 5 px: majors only
            gc.resetTransform();
            gc.scale(zoom, -zoom);
            gc.centerOn(QPointF(50, 40));
            QApplication::processEvents();
            const QImage full = gc.viewport()->grab().toImage();
            const QRgb fill = full.pixel(vp(&gc, {12.5, 12.5}));
            auto lineAt = [&](QPointF mm) {
                const QPoint p = vp(&gc, mm);
                for (int dx = -1; dx <= 1; ++dx)
                    for (int dy = -1; dy <= 1; ++dy)
                        if (full.pixel(p + QPoint(dx, dy)) != fill)
                            return true;
                return false;
            };
            bool lines = true, gaps = true;
            for (int i = 1; i * 5 < 100; ++i) {
                const bool want = zoom >= 3 || i % 5 == 0;
                lines = lines && lineAt({i * 5.0, 12.5}) == want;
                gaps = gaps && (zoom < 3 || !lineAt({i * 5.0 - 2.5, 12.5}));
            }
            for (int i = 1; i * 5 < 80; ++i) {
                const bool want = zoom >= 3 || i % 5 == 0;
                lines = lines && lineAt({12.5, i * 5.0}) == want;
                gaps = gaps && (zoom < 3 || !lineAt({12.5, i * 5.0 - 2.5}));
            }
            check(lines, zoom >= 3 ? "grid: every 5 mm line is drawn" : "grid: zoomed out, every 25 mm line and no other");
            check(gaps, "grid: nothing between the lines");
            // A part of the view repainted alone is the same picture: a strip
            // inside the board, one over its edges, and small patches walked
            // across the lines.
            bool same = true;
            QVector<QRect> parts{QRect(vp(&gc, {12, 62}), vp(&gc, {38, 18})),
                                 QRect(vp(&gc, {-10, 90}), vp(&gc, {27, 70})),
                                 QRect(vp(&gc, {74, 12}), vp(&gc, {120, -10}))};
            for (int k = 0; k < 40; ++k)
                parts << QRect(vp(&gc, {20, 30}) + QPoint(k, -k), QSize(2, 2));
            for (const QRect &r : std::as_const(parts))
                same = same && gc.viewport()->grab(r).toImage() == full.copy(r);
            check(same, "grid: a part repainted alone matches the whole");
        }

        // The zoom bar (floating, bottom right): it stays where it is while
        // the readout reads the same, and is placed again when it changes or
        // the canvas is resized.
        QWidget *bar = gc.zoomBar();
        auto *readout = bar ? bar->findChild<QLabel *>() : nullptr;
        check(bar && readout && bar->parentWidget() == &gc, "zoom bar: floats on the canvas");
        auto bottomRight = [&] {
            return bar->x() + bar->width() == gc.width() - 10 && bar->y() + bar->height() == gc.height() - 10;
        };
        hoverAt(&gc, {5, 5});
        const QString at5 = readout->text();
        const QRect g5 = bar->geometry();
        check(at5.contains(QStringLiteral("X 5.00")) && bottomRight(), "zoom bar: reads the position, bottom right");
        hoverAt(&gc, {5, 5});
        check(readout->text() == at5 && bar->geometry() == g5, "zoom bar: the same reading leaves it in place");
        hoverAt(&gc, {-123, 70});
        check(readout->text() != at5 && readout->text().contains(QStringLiteral("X -123.00")) && bottomRight(),
              "zoom bar: a new, longer reading is shown and the bar still ends bottom right");
        gc.resize(700, 500);
        QApplication::processEvents();
        check(bottomRight(), "zoom bar: follows a resize though the reading did not change");
        gc.zoomBy(2);
        check(readout->text().contains(QStringLiteral("200%")) && bottomRight(), "zoom bar: shows the new zoom");

        // A very fine grid on a big board, zoomed far in: a paint walks the
        // lines in view, not every line of the board (here 2 x 10^8 of them,
        // which took many seconds per paint).
        gdoc.setParam("width", "1000");
        gdoc.setParam("height", "1000");
        gdoc.setParam("grid_spacing", "0.00001");
        gc.resize(900, 700);
        gc.rebuild();
        gc.resetTransform();
        gc.scale(1e5, -1e5);
        gc.centerOn(QPointF(500, 500));
        QApplication::processEvents();
        QElapsedTimer fine;
        fine.start();
        const QImage fineShot = gc.viewport()->grab().toImage();
        check(!fineShot.isNull() && fine.elapsed() < 2000, "grid: a fine grid on a big board paints promptly");
        {
            // Its majors are 5 px apart at this zoom: lines and gaps across the middle.
            int changes = 0;
            const int yMid = fineShot.height() / 2;
            for (int x = 1; x < fineShot.width(); ++x)
                changes += fineShot.pixel(x, yMid) != fineShot.pixel(x - 1, yMid);
            check(changes > 50, "grid: and its lines are there");
        }
        // And a spacing too small for a line number of any width: no lines, no hang.
        gdoc.setParam("grid_spacing", "1e-300");
        fine.restart();
        gc.viewport()->grab();
        check(fine.elapsed() < 2000, "grid: a spacing of next to nothing paints promptly");
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

    // --- properties: the wheel scrolls past an unfocused field ------------------------
    {
        const c2d::Element sq = c2d::Element::makeRectangle({300, 300}, 10, 10, doc.defaultLayer());
        doc.addElement(sq);
        c2d::PropertiesPanel panel(&canvas);
        panel.setDocument(&doc);
        panel.setSelection({sq.id});
        bool allStrong = true, unchanged = true;
        for (QDoubleSpinBox *s : panel.findChildren<QDoubleSpinBox *>()) {
            allStrong = allStrong && s->focusPolicy() == Qt::StrongFocus;
            const double v = s->value();
            QWheelEvent we(QPointF(5, 5), s->mapToGlobal(QPointF(5, 5)), QPoint(), QPoint(0, 120),
                           Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(s, &we);
            unchanged = unchanged && s->value() == v;
        }
        check(panel.isEnabled() && allStrong && unchanged,
              "properties spin boxes ignore the wheel unless focused");
        doc.removeElementById(sq.id);
    }

    // --- regen: a size of zero or less keeps the shape as it was ----------------------
    {
        const QJsonObject layer = doc.defaultLayer();
        const c2d::Element sq = c2d::Element::makeRectangle({50, 50}, 10, 20, layer);
        const c2d::Element ring = c2d::Element::makeCircle({50, 50}, 5, layer);
        const c2d::Element hex = c2d::Element::makePolygon({50, 50}, 5, 6, layer);
        bool kept = true;
        for (double bad : {0.0, -3.0, qQNaN(), qInf()}) {
            kept = kept && c2d::Element::regen(sq, {{"width", bad}}).raw == sq.raw
                   && c2d::Element::regen(sq, {{"height", bad}}).raw == sq.raw
                   && c2d::Element::regen(ring, {{"radius", bad}}).raw == ring.raw
                   && c2d::Element::regen(hex, {{"radius", bad}}).raw == hex.raw;
        }
        check(kept, "regen ignores a zero, negative or non-finite width, height or radius");
        check(c2d::Element::regen(sq, {{"width", 4.0}}).raw.value("width").toDouble() == 4.0,
              "regen still takes a positive width");
    }

    // --- a turned rectangle's resize handle sits on its turned corner ----------------
    // and dragging it sizes the rectangle along its own axes.
    {
        c2d::Document rdoc;
        QJsonObject o = c2d::Element::makeRectangle({50, 50}, 40, 20, rdoc.defaultLayer()).raw;
        for (const char *key : {"points", "cp1", "cp2"}) {
            QJsonArray rows;
            for (const QJsonValue &v : o.value(QLatin1String(key)).toArray()) {
                const QJsonArray p = v.toArray();
                rows.append(QJsonArray{-p.at(1).toDouble(), p.at(0).toDouble()});   // turned 90 deg
            }
            o.insert(QLatin1String(key), rows);
        }
        o.insert("rotation", 90.0);
        const c2d::Element turned = c2d::Element::fromJson(o);
        rdoc.addElement(turned);
        c2d::Canvas rc;
        rc.resize(900, 700);
        rc.show();
        rc.setDocument(&rdoc);
        rc.resetTransform();
        rc.scale(3, -3);
        rc.centerOn(QPointF(50, 50));
        QTest::qWait(20);
        rc.setTool(c2d::Canvas::Select);
        rc.selectElements({turned.id});
        const QPointF handle(40, 70);                // (20, 10) from the center, turned 90 deg
        QTest::mousePress(rc.viewport(), Qt::LeftButton, Qt::NoModifier, vp(&rc, handle));
        QTest::mouseMove(rc.viewport(), vp(&rc, {38, 75}));
        QTest::mouseRelease(rc.viewport(), Qt::LeftButton, Qt::NoModifier, vp(&rc, {35, 80}));
        const c2d::Element *re = rdoc.elementById(turned.id);
        check(re && std::fabs(re->raw.value("width").toDouble() - 60) < 0.5
                  && std::fabs(re->raw.value("height").toDouble() - 30) < 0.5,
              "turned rectangle resizes along its own width and height");

        // A design handed in afresh starts with nothing selected: the panels,
        // given it after the canvas, show no selection either.
        rc.selectElements({turned.id});
        check(!rc.selectedElementIds().isEmpty(), "rectangle selected");
        rc.setDocument(&rdoc);
        check(rc.selectedElementIds().isEmpty(), "setDocument clears the selection");
    }

    // --- a selected sketch point or small circle moves when dragged -----------------
    // The resize handle used to cover the whole of it, so the drag resized it.
    {
        for (double zoom : {1.0, 3.0, 10.0}) {
            c2d::Document pdoc;
            const c2d::Element pt = c2d::Element::makePoint({50, 50}, pdoc.defaultLayer());
            const c2d::Element hole = c2d::Element::makeCircle({100, 50}, 2, pdoc.defaultLayer());
            pdoc.addElement(pt);
            pdoc.addElement(hole);
            c2d::Canvas pc;
            pc.resize(900, 700);
            pc.show();
            pc.setDocument(&pdoc);
            pc.resetTransform();
            pc.scale(zoom, -zoom);
            pc.centerOn(QPointF(75, 50));
            QTest::qWait(20);
            pc.setTool(c2d::Canvas::Select);
            pc.selectElements({pt.id});
            drag(&pc, {50, 50}, {70, 60});
            const c2d::Element *pe = pdoc.elementById(pt.id);
            const QJsonArray pcen = pe ? pe->raw.value("center").toArray() : QJsonArray();
            check(pe && c2d::Element::isPoint(*pe)
                      && near(QPointF(pcen.at(0).toDouble(), pcen.at(1).toDouble()), {70, 60}, 1e-6)
                      && std::fabs(pe->raw.value("radius").toDouble()
                                   - pt.raw.value("radius").toDouble()) < 1e-9,
                  "dragging a selected sketch point moves it and keeps its size");
            if (zoom < 4) {   // 2 mm radius: under 16 px on screen
                pc.selectElements({hole.id});
                drag(&pc, {100, 50}, {110, 40});
                const c2d::Element *he = pdoc.elementById(hole.id);
                const QJsonArray hc = he->raw.value("center").toArray();
                check(near(QPointF(hc.at(0).toDouble(), hc.at(1).toDouble()), {110, 40}, 1e-6)
                          && std::fabs(he->raw.value("radius").toDouble() - 2) < 1e-9,
                      "dragging a small selected circle moves it and keeps its size");
            }
        }
    }

    // --- Properties: no Radius or Convert for a sketch point; text height as drawn ---
    {
        c2d::Document pdoc;
        const c2d::Element pt = c2d::Element::makePoint({50, 50}, pdoc.defaultLayer());
        const c2d::Element ring = c2d::Element::makeCircle({100, 50}, 5, pdoc.defaultLayer());
        QJsonObject to = c2d::Element::makeText(QStringLiteral("H"), {20, 20}, 10,
                                                QStringLiteral("DejaVu Sans"), pdoc.defaultLayer()).raw;
        to.insert("transform", QJsonArray{2, 0, 0, 0, 2, 0, 20, 20, 1});   // a scale from an older file
        const c2d::Element txt = c2d::Element::fromJson(to);
        pdoc.addElement(pt);
        pdoc.addElement(ring);
        pdoc.addElement(txt);
        c2d::Canvas pc;
        c2d::PropertiesPanel panel(&pc);
        pc.setDocument(&pdoc);
        panel.setDocument(&pdoc);
        panel.show();
        auto shown = [&] {
            QList<QDoubleSpinBox *> out;
            for (QDoubleSpinBox *s : panel.findChildren<QDoubleSpinBox *>())
                if (s->isVisibleTo(&panel))
                    out << s;
            return out;
        };
        auto convertShown = [&] {
            for (QPushButton *b : panel.findChildren<QPushButton *>())
                if (b->text() == QLatin1String("Convert to path"))
                    return b->isVisibleTo(&panel);
            return false;
        };
        panel.setSelection({ring.id});
        check(shown().size() == 3 && convertShown(), "a circle shows Center, Radius and Convert");
        panel.setSelection({pt.id});
        check(shown().size() == 2 && !convertShown(), "a sketch point shows no Radius and no Convert");
        // An X edit on the point keeps its radius (the hidden field still holds it).
        shown().at(0)->setValue(60);
        const c2d::Element *pe = pdoc.elementById(pt.id);
        check(pe && c2d::Element::isPoint(*pe), "editing a point's X keeps it a point");

        panel.setSelection({txt.id});
        const QList<QDoubleSpinBox *> ts = shown();
        check(ts.size() == 3 && std::fabs(ts.at(2)->value() - 20) < 1e-6,
              "text Height shows the drawn size when its transform scales it");
        ts.at(2)->setValue(10);
        const c2d::Element *te = pdoc.elementById(txt.id);
        check(te && std::fabs(te->raw.value("font_height").toDouble() - 5) < 1e-6,
              "setting text Height to 10 draws it 10 mm high");
    }

    // --- Properties: one field edited leaves the others as they are stored ------------
    // Every field was written back as its box showed it: rounded to three
    // decimals, and the rotation cut to the box's 360 degrees.
    {
        c2d::Document pdoc;
        const QJsonObject layer = pdoc.defaultLayer();
        const c2d::Element poly = c2d::Element::makePolygon({30.00049, 20.00049}, 5.12345678, 6, layer, 450);
        const c2d::Element rect = c2d::Element::makeRectangle({60.00049, 20.00049}, 10.00049, 8.00049, layer);
        const c2d::Element ring = c2d::Element::makeCircle({90.00049, 20.00049}, 4.00049, layer);
        // Its box is centred on (20, 42.34567): shown as 42.346.
        const c2d::Element line = c2d::Element::makePath({{10, 40.00017}, {30, 44.69117}}, false, layer);
        for (const c2d::Element &e : {poly, rect, ring, line})
            pdoc.addElement(e);
        c2d::Canvas pc;
        c2d::PropertiesPanel panel(&pc);
        pc.setDocument(&pdoc);
        panel.setDocument(&pdoc);
        panel.show();
        // As in the main window: the panel reads the document again after every change.
        QObject::connect(&pc, &c2d::Canvas::documentChanged, &panel, &c2d::PropertiesPanel::refresh);
        auto shown = [&] {
            QList<QDoubleSpinBox *> out;
            for (QDoubleSpinBox *s : panel.findChildren<QDoubleSpinBox *>())
                if (s->isVisibleTo(&panel))
                    out << s;
            return out;
        };
        auto num = [&](const c2d::Element &e, const char *key) {
            return pdoc.elementById(e.id)->raw.value(QLatin1String(key)).toDouble();
        };
        auto centre = [&](const c2d::Element &e) {
            const QJsonArray c = pdoc.elementById(e.id)->raw.value("center").toArray();
            return QPointF(c.at(0).toDouble(), c.at(1).toDouble());
        };
        QUndoStack *pu = pc.undoStack();

        // Polygon: Center x, y, Radius, Rotation (and Sides).
        panel.setSelection({poly.id});
        check(shown().size() == 4 && shown().at(3)->value() == 360, "polygon: four fields, rotation shown at the box's limit");
        shown().at(0)->setValue(35);
        check(pu->count() == 1 && centre(poly) == QPointF(35, 20.00049), "polygon X: y is not rounded");
        check(num(poly, "radius") == 5.12345678 && num(poly, "rotation") == 450 && num(poly, "num_sides") == 6,
              "polygon X: radius, rotation and sides are as stored");
        shown().at(1)->setValue(25);
        check(pu->count() == 2 && centre(poly) == QPointF(35, 25) && num(poly, "radius") == 5.12345678
                  && num(poly, "rotation") == 450,
              "polygon Y: only y changes");
        shown().at(2)->setValue(7);
        check(pu->count() == 3 && num(poly, "radius") == 7 && num(poly, "rotation") == 450
                  && centre(poly) == QPointF(35, 25),
              "polygon Radius: the rotation beyond 360 is kept");
        QSpinBox *sides = nullptr;
        for (QSpinBox *s : panel.findChildren<QSpinBox *>())
            if (s->isVisibleTo(&panel))
                sides = s;
        check(sides && sides->value() == 6, "polygon: the Sides field");
        sides->setValue(8);
        check(pu->count() == 4 && num(poly, "num_sides") == 8 && num(poly, "rotation") == 450
                  && num(poly, "radius") == 7,
              "polygon Sides: only the sides change");
        shown().at(3)->setValue(90);
        check(pu->count() == 5 && num(poly, "rotation") == 90 && num(poly, "radius") == 7
                  && num(poly, "num_sides") == 8 && centre(poly) == QPointF(35, 25),
              "polygon Rotation: an edit of it is written");

        // Rectangle: Center x, y, W, H.
        panel.setSelection({rect.id});
        check(shown().size() == 4, "rectangle: four fields");
        shown().at(3)->setValue(12);
        check(num(rect, "height") == 12 && num(rect, "width") == 10.00049 && centre(rect) == QPointF(60.00049, 20.00049),
              "rectangle H: width and centre are not rounded");
        shown().at(2)->setValue(20);
        check(num(rect, "width") == 20 && num(rect, "height") == 12 && centre(rect) == QPointF(60.00049, 20.00049),
              "rectangle W: height and centre stay");

        // Circle: Center x, y, Radius.
        panel.setSelection({ring.id});
        shown().at(0)->setValue(95);
        check(centre(ring) == QPointF(95, 20.00049) && num(ring, "radius") == 4.00049,
              "circle X: y and radius are not rounded");

        // A path has no centre of its own: X and Y move its box, and the
        // axis that was not edited moves by nothing at all.
        panel.setSelection({line.id});
        check(shown().size() == 2 && shown().at(1)->value() == 42.346, "path: X and Y, the box centre rounded for show");
        auto shift = [&] {
            const QJsonArray a = pdoc.elementById(line.id)->raw.value("position").toArray();
            return QPointF(a.at(0).toDouble(), a.at(1).toDouble());
        };
        const int before = pu->count();
        shown().at(0)->setValue(25);
        check(pu->count() == before + 1 && shift() == QPointF(5, 0),
              "path X: moved in x only, in y by nothing at all");
        shown().at(1)->setValue(50);
        check(shift().x() == 5
                  && std::fabs(pdoc.elementById(line.id)->painterPath.boundingRect().center().y() - 50) < 1e-9,
              "path Y: moved in y only");
    }

    // --- pen tool: clicking the start after one more straight node does not close --
    {
        c2d::Document pdoc;
        c2d::Canvas pc;
        pc.resize(900, 700);
        pc.show();
        pc.setDocument(&pdoc);
        pc.resetTransform();
        pc.scale(3, -3);
        pc.centerOn(QPointF(50, 50));
        QTest::qWait(20);
        pc.setTool(c2d::Canvas::DrawPath);
        click(&pc, {20, 20});
        click(&pc, {60, 20});
        click(&pc, {20, 20});
        check(pdoc.elements().size() == 1, "two nodes and a click on the start make one shape");
        const c2d::PathModel m = c2d::Element::pathModel(pdoc.elements().first());
        check(m.subs.size() == 1 && !m.subs.first().closed && m.subs.first().nodes.size() == 2,
              "a straight two-node path is not closed (it would enclose nothing)");
        click(&pc, {20, 50});
        click(&pc, {60, 50});
        click(&pc, {40, 80});
        click(&pc, {20, 50});
        check(pdoc.elements().size() == 2
                  && c2d::Element::pathModel(pdoc.elements().last()).subs.first().closed,
              "three nodes still close on the start");
    }

    // --- node editor: deleting a closed triangle's node opens it --------------------
    // Two straight nodes, closed, are a there-and-back with no area (what
    // the pen refuses to make); the delete leaves an open line instead.
    {
        c2d::Document ndoc;
        const c2d::Element tri = c2d::Element::makePath({{20, 20}, {80, 20}, {50, 70}}, true,
                                                       ndoc.defaultLayer());
        ndoc.addElement(tri);
        c2d::Canvas nc;
        nc.resize(900, 700);
        nc.show();
        nc.setDocument(&ndoc);
        nc.resetTransform();
        nc.scale(3, -3);
        nc.centerOn(QPointF(50, 50));
        QTest::qWait(20);
        nc.setTool(c2d::Canvas::NodeEdit);
        nc.selectElements({tri.id});
        click(&nc, {50, 70});
        QTest::keyClick(&nc, Qt::Key_Delete);
        const c2d::PathModel m = c2d::Element::pathModel(*ndoc.elementById(tri.id));
        check(m.subs.size() == 1 && m.subs.first().nodes.size() == 2 && !m.subs.first().closed,
              "deleting a node of a closed triangle leaves an open line");
        check(!m.subs.first().nodes.first().hasIn() && !m.subs.first().nodes.last().hasOut(),
              "the opened line has no handles off its ends");
        nc.undoStack()->undo();
        const c2d::PathModel back = c2d::Element::pathModel(*ndoc.elementById(tri.id));
        check(back.subs.first().nodes.size() == 3 && back.subs.first().closed, "the node delete undoes");
    }

    // A small hole under a big outline that was added AFTER it (a DXF lists the
    // outline last): a click in the hole picks the hole, a click beside it
    // picks the outline, with the Select tool and with Select area.
    {
        c2d::Document pdoc;
        c2d::Canvas pc;
        pc.resize(900, 700);
        pc.show();
        pc.setDocument(&pdoc);
        pc.resetTransform();
        pc.scale(4.7, -4.7);
        pc.centerOn(QPointF(20, 20));
        const c2d::Element hole1 = c2d::Element::makeCircle({6, 6}, 1.55, pdoc.defaultLayer());
        const c2d::Element hole2 = c2d::Element::makeCircle({60, 6}, 1.55, pdoc.defaultLayer());
        const c2d::Element outline = c2d::Element::makeRectangle({40, 40}, 80, 80, pdoc.defaultLayer());
        const c2d::Element mid = c2d::Element::makeRectangle({40, 40}, 60, 60, pdoc.defaultLayer());
        for (const auto &e : {hole1, hole2, outline, mid})   // outline and mid added after the holes
            pdoc.addElement(e);
        pc.rebuild();
        pc.setTool(c2d::Canvas::Select);
        click(&pc, {6, 6});
        check(pc.selectedElementIds() == QStringList{hole1.id}, "a small hole under a later, bigger outline is picked");
        click(&pc, {60, 6});
        check(pc.selectedElementIds() == QStringList{hole2.id}, "the second hole too");
        click(&pc, {20, 20});          // inside both rectangles: the smaller one
        check(pc.selectedElementIds() == QStringList{mid.id}, "nested rectangles: the inner one is picked");
        click(&pc, {75, 70});          // only inside the outline
        check(pc.selectedElementIds() == QStringList{outline.id}, "outside the inner shape, the outline is picked");
    }

    // --- Select band: left to right is a window, right to left a crossing ---------------
    {
        c2d::Document bdoc;
        c2d::Canvas bc;
        bc.resize(900, 700);
        bc.show();
        bc.setDocument(&bdoc);
        bc.resetTransform();
        bc.scale(3, -3);
        bc.centerOn(QPointF(100, 100));
        const c2d::Element big = c2d::Element::makeRectangle({100, 100}, 100, 100, bdoc.defaultLayer()); // 50..150
        const c2d::Element dot = c2d::Element::makeCircle({70, 70}, 5, bdoc.defaultLayer());              // 65..75
        bdoc.addElement(big);
        bdoc.addElement(dot);
        bc.rebuild();
        bc.setTool(c2d::Canvas::Select);
        auto sel = [&] { QStringList ids = bc.selectedElementIds(); ids.sort(); return ids; };
        QStringList both{big.id, dot.id};
        both.sort();

        drag(&bc, {60, 45}, {80, 77});          // left to right over the circle and the square's edge
        check(sel() == QStringList{dot.id}, "window band: only the shape wholly inside");
        drag(&bc, {80, 45}, {60, 77});          // the same box, right to left
        check(sel() == both, "crossing band: every shape whose line it touches");
        drag(&bc, {160, 95}, {140, 105});       // right to left over the square's right edge only
        check(sel() == QStringList{big.id}, "crossing band: the square by its edge");
        drag(&bc, {160, 105}, {140, 95});       // up instead of down: still a crossing
        check(sel() == QStringList{big.id}, "crossing goes by the left/right direction only");
        drag(&bc, {90, 40}, {110, 60});        // left to right over the bottom edge: not wholly inside
        check(sel().isEmpty(), "window band: a shape it only crosses is not taken");
        drag(&bc, {40, 40}, {160, 160});
        check(sel() == both, "window band round everything takes everything");
        bc.selectElements({dot.id});
        drag(&bc, {160, 95}, {140, 105}, Qt::ControlModifier);
        check(sel() == both, "Ctrl band adds to the selection");
        check(bdoc.elementById(big.id)->painterPath.boundingRect() == QRectF(50, 50, 100, 100),
              "a band moves nothing");

        // The design rebuilt while a Ctrl band is dragged (an edit from the
        // pad, a preview): what Ctrl held stays held. The band kept the old
        // scene's items, freed by the rebuild.
        bc.selectElements({dot.id});
        QTest::mousePress(bc.viewport(), Qt::LeftButton, Qt::ControlModifier, vp(&bc, {160, 95}));
        QTest::mouseMove(bc.viewport(), vp(&bc, {150, 100}));
        bc.rebuild();
        QTest::mouseMove(bc.viewport(), vp(&bc, {140, 105}));
        QTest::mouseRelease(bc.viewport(), Qt::LeftButton, Qt::ControlModifier, vp(&bc, {140, 105}));
        check(sel() == both, "Ctrl band across a rebuild still adds to the selection");
        // Another design opened under the band: the band ends with the old one.
        c2d::Document other;
        bc.selectElements({});
        QTest::mousePress(bc.viewport(), Qt::LeftButton, Qt::NoModifier, vp(&bc, {40, 40}));
        QTest::mouseMove(bc.viewport(), vp(&bc, {100, 100}));
        bc.setDocument(&other);
        bc.setDocument(&bdoc);
        QTest::mouseMove(bc.viewport(), vp(&bc, {160, 160}));
        QTest::mouseRelease(bc.viewport(), Qt::LeftButton, Qt::NoModifier, vp(&bc, {160, 160}));
        check(sel().isEmpty(), "a band does not outlive the design it was started on");
    }

    // --- Click-cycling: a second click on the same spot steps to the shape underneath ---
    {
        c2d::Document cdoc;
        c2d::Canvas cc;
        cc.resize(900, 700);
        cc.show();
        cc.setDocument(&cdoc);
        cc.resetTransform();
        cc.scale(3, -3);
        cc.centerOn(QPointF(100, 100));
        const c2d::Element big = c2d::Element::makeRectangle({100, 100}, 100, 100, cdoc.defaultLayer());
        const c2d::Element mid = c2d::Element::makeRectangle({100, 100}, 60, 60, cdoc.defaultLayer());
        const c2d::Element dot = c2d::Element::makeCircle({100, 100}, 5, cdoc.defaultLayer());
        for (const auto &e : {big, mid, dot})
            cdoc.addElement(e);
        cc.rebuild();
        cc.setTool(c2d::Canvas::Select);
        click(&cc, {100, 100});
        check(cc.selectedElementIds() == QStringList{dot.id}, "first click takes the smallest");
        click(&cc, {100, 100});
        check(cc.selectedElementIds() == QStringList{mid.id}, "second click steps to the next shape down");
        click(&cc, {100, 100});
        check(cc.selectedElementIds() == QStringList{big.id}, "third click reaches the outline");
        click(&cc, {100, 100});
        check(cc.selectedElementIds() == QStringList{dot.id}, "and it wraps round");
        click(&cc, {75, 75});                  // inside the outer two only
        click(&cc, {75, 75});
        check(cc.selectedElementIds() == QStringList{big.id}, "click-cycling works where only two overlap");
        click(&cc, {60, 60});                  // alone inside the outline
        click(&cc, {60, 60});
        check(cc.selectedElementIds() == QStringList{big.id}, "a lone shape stays selected on a second click");
    }

    // --- Shape strip operations: reorder, duplicate, delete, each one undo step --------
    {
        c2d::Document sdoc;
        c2d::Canvas sc;
        sc.resize(900, 700);
        sc.show();
        sc.setDocument(&sdoc);
        const c2d::Element a = c2d::Element::makeCircle({10, 10}, 3, sdoc.defaultLayer());
        const c2d::Element b = c2d::Element::makeCircle({30, 10}, 3, sdoc.defaultLayer());
        const c2d::Element c = c2d::Element::makeCircle({50, 10}, 3, sdoc.defaultLayer());
        for (const auto &e : {a, b, c})
            sdoc.addElement(e);
        sc.rebuild();
        auto order = [&] { QStringList o; for (const auto &e : sdoc.elements()) o << e.id; return o; };
        sc.moveElementTo(c.id, 0);
        check(order() == QStringList({c.id, a.id, b.id}), "a shape moves to the front");
        sc.undoStack()->undo();
        check(order() == QStringList({a.id, b.id, c.id}), "and undo restores the order");
        sc.moveElementTo(a.id, 2);
        check(order() == QStringList({b.id, c.id, a.id}), "a shape moves to the end");
        sc.duplicateElements({b.id});
        check(sdoc.elements().size() == 4 && sc.selectedElementIds().size() == 1
                  && sc.selectedElementIds().first() != b.id, "duplicate adds a copy and selects it");
        sc.undoStack()->undo();
        check(sdoc.elements().size() == 3, "undo takes the copy away");
        sc.deleteElements({b.id, c.id});
        check(order() == QStringList({a.id}), "delete takes the listed shapes");
        sc.undoStack()->undo();
        check(sdoc.elements().size() == 3, "undo brings them back");
    }

    // --- A finger's tap that adds something waits for the finger to lift ----------
    // The first finger of a pinch also comes as a press; the pinch swallows
    // its release, so nothing it would have added is ever added. A mouse
    // press still adds at once.
    {
        c2d::Document pdoc;
        c2d::Canvas pc;
        pc.resize(900, 700);
        pc.show();
        pc.setDocument(&pdoc);
        pc.resetTransform();
        pc.scale(3, -3);
        pc.centerOn(QPointF(60, 60));
        pc.setTool(c2d::Canvas::DrawPoint);
        QPointingDevice finger(QStringLiteral("test touchscreen"), 4711, QInputDevice::DeviceType::TouchScreen,
                               QPointingDevice::PointerType::Finger, QInputDevice::Capability::Position, 10, 0);
        auto touch = [&](QEvent::Type t, QPointF mm) {
            const QPoint p = vp(&pc, mm);
            QMouseEvent e(t, QPointF(p), QPointF(pc.viewport()->mapToGlobal(p)), Qt::LeftButton,
                          t == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier, &finger);
            QApplication::sendEvent(pc.viewport(), &e);
        };
        touch(QEvent::MouseButtonPress, {50, 50});
        check(pdoc.elements().isEmpty(), "touch: a point waits for the finger to lift");
        touch(QEvent::MouseButtonRelease, {50, 50});
        check(pdoc.elements().size() == 1
                  && near(pdoc.elements().first().painterPath.boundingRect().center(), {50, 50}, 0.5),
              "touch: lifted, the point is placed where the finger went down");
        QTest::mousePress(pc.viewport(), Qt::LeftButton, Qt::NoModifier, vp(&pc, {80, 80}));
        check(pdoc.elements().size() == 2, "mouse: a press places the point at once");
        QTest::mouseRelease(pc.viewport(), Qt::LeftButton, Qt::NoModifier, vp(&pc, {80, 80}));
        check(pdoc.elements().size() == 2, "mouse: and the release adds nothing more");
        // A finger that starts a drag-shape (ellipse) still draws as it moves.
        pc.setTool(c2d::Canvas::DrawEllipse);
        touch(QEvent::MouseButtonPress, {20, 20});
        touch(QEvent::MouseMove, {40, 30});
        touch(QEvent::MouseButtonRelease, {40, 30});
        check(pdoc.elements().size() == 3, "touch: a dragged ellipse is drawn as before");
    }

    // --- The first finger of a pinch adds, trims and selects nothing ------------------
    {
        QPointingDevice *finger = QTest::createTouchDevice();
        c2d::Document gdoc;
        c2d::Canvas gc;
        gc.resize(900, 700);
        gc.show();
        gc.setDocument(&gdoc);
        gc.resetTransform();
        gc.scale(3, -3);
        gc.centerOn(QPointF(50, 50));
        auto touch = [&](QEvent::Type t, QPointF mm) {
            const QPoint p = vp(&gc, mm);
            QMouseEvent e(t, QPointF(p), QPointF(gc.viewport()->mapToGlobal(p)), Qt::LeftButton,
                          t == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier, finger);
            QApplication::sendEvent(gc.viewport(), &e);
        };
        auto tap = [&](QPointF mm) {
            touch(QEvent::MouseButtonPress, mm);
            touch(QEvent::MouseButtonRelease, mm);
        };

        // A point waiting for its finger to lift: the pinch drops it.
        gc.setTool(c2d::Canvas::DrawPoint);
        touch(QEvent::MouseButtonPress, {30, 30});
        gc.pinchStarted();
        touch(QEvent::MouseButtonRelease, {30, 30});
        check(gdoc.elements().isEmpty() && !gc.m_pinching, "pinch: the first finger's point is not placed");

        // Trim on the first finger: nothing trimmed. A plain tap trims.
        const c2d::Element h = c2d::Element::makePath({{0, 50}, {100, 50}}, false, gdoc.defaultLayer());
        const c2d::Element v = c2d::Element::makePath({{50, 0}, {50, 100}}, false, gdoc.defaultLayer());
        gdoc.addElement(h);
        gdoc.addElement(v);
        gc.rebuild();
        gc.setTool(c2d::Canvas::Trim);
        const QJsonObject h0 = gdoc.elementById(h.id)->raw;
        const int steps = gc.undoStack()->count();
        touch(QEvent::MouseButtonPress, {20, 50});
        gc.pinchStarted();
        touch(QEvent::MouseButtonRelease, {20, 50});
        check(gc.undoStack()->count() == steps && gdoc.elements().size() == 2
                  && gdoc.elementById(h.id) && gdoc.elementById(h.id)->raw == h0,
              "pinch: the first finger trims nothing");
        tap({20, 50});
        check(gc.undoStack()->count() == steps + 1, "touch: a tap with Trim trims");
        gc.undoStack()->undo();

        // The selection the finger's press changed comes back; the board
        // tap's "let go of the toolpath" is not said.
        gc.setTool(c2d::Canvas::Select);
        gc.selectElements({v.id});
        int empties = 0;
        auto conn = QObject::connect(&gc, &c2d::Canvas::emptyPressed, [&] { ++empties; });
        touch(QEvent::MouseButtonPress, {20, 80});   // empty board
        gc.pinchStarted();
        touch(QEvent::MouseButtonRelease, {20, 80});
        check(gc.selectedElementIds() == QStringList{v.id} && empties == 0,
              "pinch: the selection is as before the first finger, the toolpath kept");
        touch(QEvent::MouseButtonPress, {20, 80});
        check(empties == 0, "touch: an empty-board tap waits for the finger to lift");
        touch(QEvent::MouseButtonRelease, {20, 80});
        check(empties == 1 && gc.selectedElementIds().isEmpty(), "touch: and then lets go of everything");
        QObject::disconnect(conn);

        // A double tap near a path's start closes it once and starts nothing.
        gc.deleteElements({h.id, v.id});
        const int before = gdoc.elements().size();
        gc.setTool(c2d::Canvas::DrawPath);
        click(&gc, {10, 10});
        click(&gc, {60, 10});
        click(&gc, {60, 60});
        check(gc.m_penNodes.size() == 3, "double tap: three nodes placed");
        touch(QEvent::MouseButtonPress, {10, 10});
        touch(QEvent::MouseButtonRelease, {10, 10});
        touch(QEvent::MouseButtonPress, {10, 10});
        touch(QEvent::MouseButtonDblClick, {10, 10});
        touch(QEvent::MouseButtonRelease, {10, 10});
        bool closed = false;
        if (gdoc.elements().size() == before + 1)
            for (const auto &pt : gdoc.elements().last().raw.value("point_type").toArray())
                closed = closed || pt.toInt() == 4;
        check(gdoc.elements().size() == before + 1 && closed && gc.m_penNodes.isEmpty(),
              "double tap: the path is closed, and no new one is started");

        // A finger that went down on the start and slid a little off it
        // before lifting still closes the path (it went down to close it).
        click(&gc, {10, 10});
        click(&gc, {60, 10});
        click(&gc, {60, 60});
        touch(QEvent::MouseButtonPress, {10, 10});
        touch(QEvent::MouseMove, {13.5, 10});
        touch(QEvent::MouseButtonRelease, {13.5, 10});
        bool closed2 = false;
        if (gdoc.elements().size() == before + 2)
            for (const auto &pt : gdoc.elements().last().raw.value("point_type").toArray())
                closed2 = closed2 || pt.toInt() == 4;
        check(closed2 && gc.m_penNodes.isEmpty(), "slide: a close tap that slid off the start still closes");
        gc.setTool(c2d::Canvas::Select);
    }

    // --- A delete that leaves a toolpath without vectors marks it ------------------
    // (Export skips such a toolpath: a Facing left with nothing faced the
    // whole stock.)
    {
        c2d::Document vdoc;
        c2d::Canvas vc;
        vc.setDocument(&vdoc);
        const c2d::Element a = c2d::Element::makeCircle({10, 10}, 3, vdoc.defaultLayer());
        const c2d::Element b = c2d::Element::makeCircle({30, 10}, 3, vdoc.defaultLayer());
        vdoc.addElement(a);
        vdoc.addElement(b);
        c2d::Toolpath one = c2d::makeToolpath(vdoc, "contour", {a.id});
        c2d::Toolpath two = c2d::makeToolpath(vdoc, "contour", {a.id, b.id});
        vdoc.insertToolpath(0, one);
        vdoc.insertToolpath(1, two);
        vc.rebuild();
        vc.deleteElements({a.id});
        check(vdoc.toolpathByUuid(one.uuid)->json.value("vectors_removed").toBool(),
              "delete: a toolpath left with no vectors is marked");
        check(!vdoc.toolpathByUuid(two.uuid)->json.contains("vectors_removed"),
              "delete: one that keeps a vector is not");
        vc.undoStack()->undo();
        check(!vdoc.toolpathByUuid(one.uuid)->json.contains("vectors_removed")
                  && vdoc.toolpathByUuid(one.uuid)->json.value("elements").toArray().size() == 1,
              "delete undone: the toolpath is as it was");

        // A sketch point left behind is not cut by a contour (export drops
        // it), so the contour still counts as emptied; a drilling keeps it.
        const c2d::Element c = c2d::Element::makeCircle({50, 10}, 3, vdoc.defaultLayer());
        const c2d::Element pt = c2d::Element::makePoint({50, 30}, vdoc.defaultLayer());
        vdoc.addElement(c);
        vdoc.addElement(pt);
        c2d::Toolpath three = c2d::makeToolpath(vdoc, "contour", {c.id, pt.id});
        c2d::Toolpath drill = c2d::makeToolpath(vdoc, "drilling_toolpath", {c.id, pt.id});
        vdoc.insertToolpath(2, three);
        vdoc.insertToolpath(3, drill);
        vc.rebuild();
        vc.deleteElements({c.id});
        check(vdoc.toolpathByUuid(three.uuid)->json.value("vectors_removed").toBool(),
              "delete: a contour left with only a sketch point is marked");
        check(!vdoc.toolpathByUuid(drill.uuid)->json.contains("vectors_removed"),
              "delete: a drilling left with a sketch point is not");
    }

    // --- A dimension label dragged when Design is left goes back -------------------
    // Leaving the Design page ended the drag with the label moved in the
    // design and no undo step for it.
    {
        QTemporaryDir dd;
        QString err;
        const QString blank = dd.filePath("dim.c2d");
        check(c2d::Document::createBlank(blank, 200, 200, 19, &err), "dimension drag: a blank design");
        c2d::Document ddoc;
        check(ddoc.load(blank, &err), "dimension drag: it loads");
        const c2d::Element l = c2d::Element::makePath({{20, 50}, {120, 50}}, false, ddoc.defaultLayer());
        ddoc.addElement(l);
        c2d::Constraint c;
        c.type = c2d::Constraint::Distance;
        c2d::GeoRef r;
        r.kind = c2d::GeoRef::Line;
        r.id = l.id;
        r.index = 0;
        c.refs = {r};
        c.uid = ddoc.parametric().nextUid();
        c.name = ddoc.parametric().nextDimensionName();
        c.expr = QStringLiteral("100");
        c2d::param::prime(ddoc, c);
        c.label = QPointF(0, -15);
        ddoc.parametricRef().constraints.append(c);
        c2d::param::solve(ddoc);
        c2d::Canvas dc;
        dc.resize(900, 700);
        dc.show();
        dc.setTool(c2d::Canvas::Select);
        dc.setDocument(&ddoc);
        dc.resetTransform();
        dc.scale(3, -3);
        dc.centerOn(QPointF(70, 50));
        check(QTest::qWaitForWindowExposed(&dc), "dimension drag: the canvas is shown");
        dc.viewport()->repaint();
        const QPointF label0 = ddoc.parametric().constraints.first().label;
        QTest::mousePress(dc.viewport(), Qt::LeftButton, Qt::NoModifier, vp(&dc, {70, 35}));
        const QPointF at(70, 35);
        QTest::mouseMove(dc.viewport(), vp(&dc, at + QPointF(10, -10)));
        check(ddoc.parametric().constraints.first().label != label0, "dimension drag: the label follows the drag");
        dc.setParametricVisible(false);
        check(ddoc.parametric().constraints.first().label == label0,
              "dimension drag: leaving Design puts the label back");
        QTest::mouseRelease(dc.viewport(), Qt::LeftButton, Qt::NoModifier, vp(&dc, {80, 25}));
        check(ddoc.parametric().constraints.first().label == label0 && dc.undoStack()->count() == 0,
              "dimension drag: and no step moves it later");
        // A drag under way when an undo replaces the design: leaving Design
        // must not put back the label as it was before the undo.
        dc.setParametricVisible(true);
        dc.viewport()->repaint();
        QTest::mousePress(dc.viewport(), Qt::LeftButton, Qt::NoModifier, vp(&dc, {70, 35}));
        QTest::mouseMove(dc.viewport(), vp(&dc, {80, 25}));
        QTest::mouseRelease(dc.viewport(), Qt::LeftButton, Qt::NoModifier, vp(&dc, {80, 25}));
        const QPointF labelB = ddoc.parametric().constraints.first().label;
        check(labelB != label0 && dc.undoStack()->count() == 1, "dimension drag: a move is one step");
        dc.viewport()->repaint();
        QTest::mousePress(dc.viewport(), Qt::LeftButton, Qt::NoModifier, vp(&dc, {80, 25}));
        QTest::mouseMove(dc.viewport(), vp(&dc, {90, 15}));
        check(ddoc.parametric().constraints.first().label != labelB, "dimension drag: a second drag moves it");
        dc.undoStack()->undo();
        check(ddoc.parametric().constraints.first().label == label0, "dimension drag: undo puts the first move back");
        dc.setParametricVisible(false);
        check(ddoc.parametric().constraints.first().label == label0,
              "dimension drag: leaving Design keeps what the undo put back");
        QTest::mouseRelease(dc.viewport(), Qt::LeftButton, Qt::NoModifier, vp(&dc, {90, 15}));
        check(ddoc.parametric().constraints.first().label == label0 && dc.undoStack()->index() == 0,
              "dimension drag: and the release adds no step");
        // Undo of a step that is not about the label, during a drag: the
        // label goes back from where the drag left it (no step records it).
        dc.setParametricVisible(true);
        dc.duplicateElements({l.id});
        const int stepAt = dc.undoStack()->index();
        dc.viewport()->repaint();
        QTest::mousePress(dc.viewport(), Qt::LeftButton, Qt::NoModifier, vp(&dc, {70, 35}));
        QTest::mouseMove(dc.viewport(), vp(&dc, {80, 25}));
        check(ddoc.parametric().constraints.first().label != label0, "dimension drag: under way again");
        dc.undoStack()->undo();
        check(ddoc.parametric().constraints.first().label == label0,
              "dimension drag: an undo of another step puts the dragged label back");
        QTest::mouseRelease(dc.viewport(), Qt::LeftButton, Qt::NoModifier, vp(&dc, {80, 25}));
        check(ddoc.parametric().constraints.first().label == label0 && dc.undoStack()->index() == stepAt - 1,
              "dimension drag: and the release records nothing");

        // A double tap on a dimension with the Dimension tool edits it, as a
        // double-click does (the tap's press waits for its release).
        {
            QPointingDevice *finger = QTest::createTouchDevice();
            auto touchDim = [&](QEvent::Type t, QPointF mm) {
                const QPoint p = vp(&dc, mm);
                QMouseEvent e(t, QPointF(p), QPointF(dc.viewport()->mapToGlobal(p)), Qt::LeftButton,
                              t == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier,
                              finger);
                QApplication::sendEvent(dc.viewport(), &e);
            };
            dc.setTool(c2d::Canvas::Dimension);
            dc.viewport()->repaint();
            touchDim(QEvent::MouseButtonPress, {70, 35});
            touchDim(QEvent::MouseButtonRelease, {70, 35});
            touchDim(QEvent::MouseButtonPress, {70, 35});
            touchDim(QEvent::MouseButtonDblClick, {70, 35});
            check(!(dc.m_dimEdit && dc.m_dimEdit->isVisible()), "double tap: nothing before the finger lifts");
            touchDim(QEvent::MouseButtonRelease, {70, 35});
            check(dc.m_dimEdit && dc.m_dimEdit->isVisible(), "double tap: the dimension's value editor opens");
            dc.setTool(c2d::Canvas::Select);
        }
    }

    // --- The element menu offers Edit nodes on the Design page only ------------------
    // (Toolpaths hides the drawing palette; the Nodes tool there edited
    // instead of picking.)
    {
        c2d::Document mdoc;
        c2d::Canvas mc;
        mc.resize(900, 700);
        mc.show();
        mc.setDocument(&mdoc);
        mc.resetTransform();
        mc.scale(3, -3);
        mc.centerOn(QPointF(100, 100));
        const c2d::Element sq = c2d::Element::makeRectangle({100, 100}, 40, 40, mdoc.defaultLayer());
        mdoc.addElement(sq);
        mc.rebuild();
        mc.setTool(c2d::Canvas::Select);
        auto menuTexts = [&] {
            auto texts = std::make_shared<QStringList>();
            QTimer::singleShot(50, [texts] {
                if (auto *m = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
                    for (QAction *a : m->actions())
                        *texts << a->text();
                    m->close();
                }
            });
            mc.selectElements({sq.id});
            const QPoint at = vp(&mc, {80, 100});   // on the square's left edge
            QContextMenuEvent ev(QContextMenuEvent::Mouse, at, mc.viewport()->mapToGlobal(at));
            QApplication::sendEvent(mc.viewport(), &ev);
            return *texts;
        };
        auto hasEditNodes = [](const QStringList &t) {
            for (const QString &s : t)
                if (s.startsWith(QLatin1String("Edit nodes")))
                    return true;
            return false;
        };
        const QStringList design = menuTexts();
        check(!design.isEmpty() && hasEditNodes(design), "element menu: Edit nodes on the Design page");
        mc.setParametricVisible(false);
        const QStringList tp = menuTexts();
        check(!tp.isEmpty() && !hasEditNodes(tp), "element menu: no Edit nodes off the Design page");
        check(mc.tool() == c2d::Canvas::Select, "element menu: the tool is untouched");
    }

    // --- Shape strip: a shape past the strip's tiles stays selected ----------------
    // A traced image can hold more shapes than the strip shows; picking a
    // tile let go of every selected shape that had none.
    {
        c2d::Document tdoc;
        c2d::Canvas tc;
        tc.resize(900, 700);
        tc.setDocument(&tdoc);
        QStringList ids;
        for (int i = 0; i < 405; ++i) {
            const c2d::Element e = c2d::Element::makeCircle({double(i % 20) * 10, double(i / 20) * 10}, 2,
                                                            tdoc.defaultLayer());
            tdoc.addElement(e);
            ids << e.id;
        }
        tc.rebuild();
        c2d::ShapeStrip strip(&tc);
        check(strip.count() < ids.size(), "shape strip: not every shape has a tile");
        tc.selectElements({ids.last()});
        strip.item(0)->setSelected(true);
        QStringList sel = tc.selectedElementIds();
        sel.sort();
        QStringList want{ids.first(), ids.last()};
        want.sort();
        check(sel == want, "shape strip: a tile picked keeps the selected shape that has no tile");
        strip.clearSelection();
        check(tc.selectedElementIds() == QStringList{ids.last()},
              "shape strip: and letting go of the tiles keeps it too");
    }

    std::printf("OK: %d checks passed\n", g_checks);
    return 0;
}
