// Escape (and conflicting constraints) mid-gesture: Escape during a node
// drag crashed; during a resize drag the resize still landed on release;
// Tangent then Concentric on two circles shrank both to nothing.
#include "../src/canvas.h"
#include "../src/c2ddocument.h"
#include "../src/element.h"
#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>
#include <QUndoStack>
#include <cmath>
#include <cstdio>

static QPoint vp(c2d::Canvas *c, QPointF mm) { return c->mapFromScene(mm); }
static int g_fail = 0;
static void check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++g_fail;
}

static void run(const QString &mode)
{
    c2d::Document doc;
    c2d::Canvas canvas;
    canvas.resize(900, 700);
    canvas.show();
    canvas.setDocument(&doc);
    canvas.resetTransform();
    canvas.scale(3, -3);
    canvas.centerOn(QPointF(100, 100));
    if (mode == "node") {
        const c2d::Element p = c2d::Element::makePath({{100, 100}, {140, 100}, {140, 140}}, true, doc.defaultLayer());
        doc.addElement(p);
        canvas.rebuild();
        canvas.setTool(c2d::Canvas::NodeEdit);
        canvas.selectElements({p.id});
        QTest::mousePress(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, vp(&canvas, {140, 140}));
        QTest::mouseMove(canvas.viewport(), vp(&canvas, {145, 145}));
        std::printf("pressed Escape mid-drag\n"); std::fflush(stdout);
        QTest::keyClick(&canvas, Qt::Key_Escape);
        QTest::mouseMove(canvas.viewport(), vp(&canvas, {150, 150}));
        std::printf("moved after Escape\n"); std::fflush(stdout);
        QTest::mouseRelease(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, vp(&canvas, {150, 150}));
        const QPainterPath after = doc.elementById(p.id)->painterPath;
        check(after.boundingRect().right() < 140.01 && after.boundingRect().bottom() < 140.01,
              "Escape mid node drag: no crash, the node stays where it was");
    } else if (mode == "resize") {
        const c2d::Element c = c2d::Element::makeCircle({100, 100}, 20, doc.defaultLayer());
        doc.addElement(c);
        canvas.rebuild();
        canvas.setTool(c2d::Canvas::Select);
        canvas.selectElements({c.id});
        // find the handle: try the right-hand point
        QTest::mousePress(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, vp(&canvas, {120, 100}));
        QTest::mouseMove(canvas.viewport(), vp(&canvas, {130, 100}));
        QTest::keyClick(&canvas, Qt::Key_Escape);
        QTest::mouseMove(canvas.viewport(), vp(&canvas, {140, 100}));
        QTest::mouseRelease(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, vp(&canvas, {140, 100}));
        check(std::abs(doc.elementById(c.id)->raw.value("radius").toDouble() - 20) < 1e-6 && canvas.undoStack()->count() == 0,
              "Escape mid resize drag: the resize is cancelled");
    }
    else if (mode == "concentric") {
        c2d::Element a = c2d::Element::makeCircle({100, 100}, 5, doc.defaultLayer());
        c2d::Element b = c2d::Element::makeCircle({130, 100}, 8, doc.defaultLayer());
        doc.addElement(a); doc.addElement(b); canvas.rebuild();
        auto cref = [](const QString &id) { c2d::GeoRef r; r.kind = c2d::GeoRef::Circle; r.id = id; return r; };
        c2d::Parametric after = doc.parametric();
        c2d::Constraint t; t.type = c2d::Constraint::Tangent; t.refs = {cref(a.id), cref(b.id)}; t.uid = after.nextUid();
        c2d::param::prime(doc, t); after.constraints.append(t);
        bool ok1 = canvas.applyParametric(after, "tangent");
        after = doc.parametric();
        c2d::Constraint c; c.type = c2d::Constraint::Concentric; c.refs = {cref(a.id), cref(b.id)}; c.uid = after.nextUid();
        c2d::param::prime(doc, c); after.constraints.append(c);
        bool ok2 = canvas.applyParametric(after, "concentric");
        check(ok1, "tangent accepted");
        check(doc.elementById(a.id)->raw.value("radius").toDouble() > 1 && doc.elementById(b.id)->raw.value("radius").toDouble() > 1,
              "concentric after tangent does not shrink the circles to nothing");
        (void)ok2;
    }
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    for (const char *m : {"node", "resize", "concentric"})
        run(QString::fromLatin1(m));
    std::printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
