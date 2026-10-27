// Touch selection on the canvas (Phobic, 2026-10-27, CNC PC touch screen):
// a 2x2 picked with the select band, then a tap on Contour, and the toolpath
// got nearly every shape: the tap also reached the canvas as a press above
// its top edge and started a new band there. And a tap on empty board did
// not let go of the selected toolpath.
#include "../src/c2ddocument.h"
#include "../src/canvas.h"
#include "../src/element.h"
#include "../src/toolpathfactory.h"
#include "../src/toolpathpanel.h"
#include <QApplication>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <cstdio>

static int failures = 0;
static void check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++failures;
}

static QPointingDevice *g_touch = nullptr;

// What Qt hands the canvas for a finger: a mouse event naming the touch screen.
static void touchMouse(QWidget *vp, QEvent::Type t, QPoint at, Qt::MouseButtons held)
{
    const Qt::MouseButton b = t == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent ev(t, QPointF(at), QPointF(at), QPointF(vp->mapToGlobal(at)), b, held,
                   Qt::NoModifier, g_touch);
    QApplication::sendEvent(vp, &ev);
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    g_touch = QTest::createTouchDevice();
    QTemporaryDir tmp;
    QString err;
    const QString blank = tmp.filePath("a.c2d");
    c2d::Document::createBlank(blank, 300, 300, 19, &err);
    c2d::Document d;
    d.load(blank, &err);
    // A 4x4 grid of 20 mm squares at 30 mm pitch; the 2x2 bottom left is the pick.
    QStringList pick, all;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            const c2d::Element e = c2d::Element::makeRectangle({20.0 + 30 * c, 20.0 + 30 * r}, 20, 20,
                                                               d.defaultLayer());
            d.addElement(e);
            all << e.id;
            if (r < 2 && c < 2)
                pick << e.id;
        }
    d.insertToolpath(0, c2d::makeToolpath(d, "profile_toolpath", {all.first()}));

    // A window like the app's: a toolbar button above the drawing, the
    // toolpath list beside it.
    QWidget win;
    auto *h = new QHBoxLayout(&win);
    auto *left = new QVBoxLayout;
    auto *contour = new QToolButton;
    contour->setText(QStringLiteral("Contour"));
    contour->setFixedSize(120, 60);
    left->addWidget(contour);
    auto *canvas = new c2d::Canvas;
    left->addWidget(canvas, 1);
    h->addLayout(left, 1);
    auto *panel = new c2d::ToolpathPanel(canvas);
    h->addWidget(panel);
    canvas->setDocument(&d);
    panel->setDocument(&d);
    win.resize(1200, 800);
    win.show();
    QApplication::setActiveWindow(&win);
    QTest::qWaitForWindowActive(&win);
    canvas->setTool(c2d::Canvas::Select);
    canvas->resetTransform();
    canvas->scale(4, -4);
    canvas->centerOn(QPointF(70, 70));
    QWidget *vp = canvas->viewport();
    auto at = [&](double x, double y) { return canvas->mapFromScene(QPointF(x, y)); };
    auto sorted = [](QStringList l) { l.sort(); return l; };
    auto picked = [&] { return sorted(canvas->selectedElementIds()) == sorted(pick); };
    int clicks = 0;
    QObject::connect(contour, &QToolButton::clicked, [&] { ++clicks; });

    // 1. Band the 2x2 with a finger, left to right (window).
    touchMouse(vp, QEvent::MouseButtonPress, at(3, 67), Qt::LeftButton);
    touchMouse(vp, QEvent::MouseMove, at(40, 30), Qt::LeftButton);
    touchMouse(vp, QEvent::MouseMove, at(67, 3), Qt::LeftButton);
    touchMouse(vp, QEvent::MouseButtonRelease, at(67, 3), Qt::NoButton);
    check(picked(), "a finger band picks the 2x2");

    // 2. The tap on Contour reaches the canvas as a press above its top edge
    // and a drag down into the drawing: the selection must not change.
    const QPoint above(vp->mapFrom(&win, contour->geometry().center()));
    check(above.y() < 0, "the button lies above the drawing");
    touchMouse(vp, QEvent::MouseButtonPress, above, Qt::LeftButton);
    touchMouse(vp, QEvent::MouseMove, QPoint(above.x(), vp->height() - 5), Qt::LeftButton);
    touchMouse(vp, QEvent::MouseButtonRelease, QPoint(above.x(), vp->height() - 5), Qt::NoButton);
    check(picked(), "a press from outside the drawing leaves the selection alone");

    // 3. A band whose release got lost: the next move without a button, or
    // a finger off the drawing, must not stretch it over everything.
    QTest::mousePress(vp, Qt::LeftButton, {}, at(3, 67));
    QTest::mouseMove(vp, at(67, 3));
    QTest::mouseRelease(vp, Qt::LeftButton, {}, at(67, 3));
    check(picked(), "a mouse band picks the 2x2 too");
    touchMouse(vp, QEvent::MouseButtonPress, at(3, 67), Qt::LeftButton);
    touchMouse(vp, QEvent::MouseMove, at(67, 3), Qt::LeftButton);
    // the finger runs off the drawing onto the button, where it lifts
    touchMouse(vp, QEvent::MouseMove, above, Qt::LeftButton);
    touchMouse(vp, QEvent::MouseButtonRelease, above, Qt::NoButton);
    check(picked(), "a finger leaving the drawing leaves the band where it was");
    touchMouse(vp, QEvent::MouseButtonPress, at(3, 67), Qt::LeftButton);
    touchMouse(vp, QEvent::MouseMove, at(67, 3), Qt::LeftButton);
    // no release; the next thing is a move with nothing held, far away
    touchMouse(vp, QEvent::MouseMove, at(140, 140), Qt::NoButton);
    check(picked(), "a move with nothing held ends a band whose release was lost");
    touchMouse(vp, QEvent::MouseMove, at(140, 3), Qt::LeftButton);
    check(picked(), "and the band stays ended");
    touchMouse(vp, QEvent::MouseButtonRelease, at(140, 3), Qt::NoButton);

    // 4. Real touch events: a tap on the button clicks it and the canvas
    // keeps the 2x2.
    QTest::touchEvent(&win, g_touch).press(0, contour->geometry().center(), &win);
    QTest::touchEvent(&win, g_touch).release(0, contour->geometry().center(), &win);
    QTest::qWait(20);
    check(clicks == 1, "a touch tap clicks the button");
    check(picked(), "a touch tap on the button leaves the canvas selection alone");

    // 5. A selected toolpath lets go when empty board is tapped or clicked.
    QTreeWidget *list = panel->findChild<QTreeWidget *>();
    auto selectRow = [&] {
        list->clearSelection();
        list->topLevelItem(0)->setSelected(true);
        QTest::qWait(10);
    };
    selectRow();
    check(!canvas->vectorHighlight().isEmpty(), "the toolpath's shapes are lit");
    touchMouse(vp, QEvent::MouseButtonPress, at(140, 140), Qt::LeftButton);
    touchMouse(vp, QEvent::MouseButtonRelease, at(140, 140), Qt::NoButton);
    check(list->selectedItems().isEmpty() && canvas->vectorHighlight().isEmpty(),
          "a tap on empty board unselects the toolpath");
    check(canvas->selectedElementIds().isEmpty(), "and the shapes");
    selectRow();
    QTest::mouseClick(vp, Qt::LeftButton, {}, at(140, 140));
    check(list->selectedItems().isEmpty() && canvas->vectorHighlight().isEmpty(),
          "a click on empty board unselects the toolpath");
    selectRow();
    const QPoint tapAt = vp->mapTo(&win, at(140, 140));
    QTest::touchEvent(&win, g_touch).press(0, tapAt, &win);
    QTest::touchEvent(&win, g_touch).release(0, tapAt, &win);
    QTest::qWait(20);
    check(list->selectedItems().isEmpty(), "a real touch tap on empty board unselects the toolpath");
    selectRow();
    touchMouse(vp, QEvent::MouseButtonPress, above, Qt::LeftButton);
    touchMouse(vp, QEvent::MouseButtonRelease, above, Qt::NoButton);
    check(!list->selectedItems().isEmpty(), "a press from outside the drawing keeps the toolpath");

    std::printf(failures ? "%d FAILED\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
