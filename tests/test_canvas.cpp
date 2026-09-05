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

#include <QApplication>
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
    canvas.convertToPaths({text.id});
    check(doc.elementById(text.id)->geometryType == QLatin1String("path")
          && doc.elements().size() == 4 + glyphs.size(), "text converts to one path per contour");
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

    std::printf("OK: %d checks passed\n", g_checks);
    return 0;
}
