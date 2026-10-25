// Dragging on the Toolpaths side scrolls it (touch screens): the open
// background of a scroll area, and the toolpath cards themselves, where a
// drag scrolls instead of reordering and a scroll never picks the card it
// started on. Moving a card needs a 500 ms hold, which is not driven here
// (QDrag would block the offscreen test).
//
// Plain asserts, no framework; exits 0 on success.

#include "../src/c2ddocument.h"
#include "../src/canvas.h"
#include "../src/dragscroll.h"
#include "../src/padlink.h"
#include "../src/toolpathpanel.h"

#include <QMouseEvent>
#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QVBoxLayout>

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

static void drag(QWidget *w, QPoint from, QPoint to)
{
    QTest::mousePress(w, Qt::LeftButton, {}, from);
    const QPoint step = (to - from) / 5;
    for (int i = 1; i <= 5; ++i)
        QTest::mouseMove(w, from + step * i);
    QTest::mouseRelease(w, Qt::LeftButton, {}, to);
}

static void testOpenArea()
{
    QScrollArea area;
    area.resize(300, 300);
    area.setWidgetResizable(true);
    auto *body = new QWidget;
    auto *v = new QVBoxLayout(body);
    auto *label = new QLabel(QStringLiteral("Depth"));
    auto *button = new QPushButton(QStringLiteral("Change…"));
    v->addWidget(label);
    v->addWidget(button);
    v->addSpacing(2000);
    area.setWidget(body);
    c2d::DragScroll::install(&area);
    area.show();
    check(QTest::qWaitForWindowExposed(&area), "window shown");
    QScrollBar *bar = area.verticalScrollBar();
    check(bar->maximum() > 500, "the content is taller than the area");

    drag(label, label->rect().center(), label->rect().center() - QPoint(0, 150));
    check(bar->value() > 100, "dragging a label scrolls the panel");

    bar->setValue(0);
    QTest::qWait(10);
    drag(button, button->rect().center(), button->rect().center() - QPoint(0, 150));
    check(bar->value() == 0, "dragging on a button does not scroll");
}

static void testCards()
{
    QFile f(QStringLiteral(PHOBICCCP_SOURCE_DIR "/tests/data/pad-design.json"));
    check(f.open(QIODevice::ReadOnly), "pad fixture readable");
    QTemporaryDir tmp;
    const QString path = tmp.filePath(QStringLiteral("cards.c2d"));
    QString err;
    check(c2d::padlink::buildDocument(QJsonDocument::fromJson(f.readAll()).object(), path, nullptr, &err),
          "pad fixture builds");
    c2d::Document doc;
    check(doc.load(path, &err), "built document loads");
    check(doc.toolpaths().size() >= 4, "the document has toolpaths");

    c2d::Canvas canvas;
    c2d::ToolpathPanel panel(&canvas);
    canvas.setDocument(&doc);
    panel.setDocument(&doc);
    auto *tree = panel.findChild<QTreeWidget *>();
    check(tree != nullptr, "the panel has its list");
    tree->setFixedHeight(130);   // shorter than the cards, so it scrolls
    panel.resize(360, 600);
    panel.show();
    check(QTest::qWaitForWindowExposed(&panel), "panel shown");
    QWidget *vp = tree->viewport();

    const QString first = panel.currentUuid();
    const QRect r1 = tree->visualItemRect(tree->topLevelItem(1));
    check(r1.isValid(), "second card is on screen");

    // A drag that starts on a card scrolls; the card is not picked.
    drag(vp, r1.center(), r1.center() - QPoint(0, 60));
    check(tree->verticalScrollBar()->value() > 0, "dragging a card scrolls the list");
    check(panel.currentUuid() == first, "a scroll does not pick the card it started on");
    check(doc.toolpaths().at(0).uuid == first, "nor does it move anything");

    // A plain tap still picks the card.
    tree->verticalScrollBar()->setValue(0);
    QTest::qWait(10);
    const QRect r2 = tree->visualItemRect(tree->topLevelItem(1));
    QTest::mouseClick(vp, Qt::LeftButton, {}, r2.center());
    check(panel.currentUuid() == doc.toolpaths().at(1).uuid, "a tap picks the card");

    // A press whose release went elsewhere (a dialog popped up): moves with
    // no button held neither scroll nor pick up a card later.
    tree->verticalScrollBar()->setValue(0);
    QTest::qWait(10);
    const QRect r3 = tree->visualItemRect(tree->topLevelItem(1));
    QTest::mousePress(vp, Qt::LeftButton, {}, r3.center());
    for (int i = 1; i <= 5; ++i) {
        QMouseEvent mv(QEvent::MouseMove, QPointF(r3.center() - QPoint(0, 12 * i)),
                       QPointF(vp->mapToGlobal(r3.center() - QPoint(0, 12 * i))), Qt::NoButton, Qt::NoButton, {});
        QApplication::sendEvent(vp, &mv);
    }
    check(tree->verticalScrollBar()->value() == 0, "hovering after a lost release does not scroll");
    QTest::qWait(600);
    check(doc.toolpaths().at(0).uuid == first, "nor is a card picked up by the hold");
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    testOpenArea();
    testCards();
    std::printf("test_dragscroll: %d checks OK\n", g_checks);
    return 0;
}
