// The "v" under a toolpath card opens a thumbnail per shape, each with a bin
// that takes the shape out of the toolpath; a pocket under an as-deep pocket
// shows only what is left for it on the canvas.
#include "../src/c2ddocument.h"
#include "../src/canvas.h"
#include "../src/element.h"
#include "../src/toolpathfactory.h"
#include "../src/toolpathpanel.h"
#include <QApplication>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QUndoStack>
#include <cstdio>

static int failures = 0;
static void check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++failures;
}

static QStringList idsOf(c2d::Document &d, const QString &uuid)
{
    QStringList out;
    for (const QJsonValue &v : d.toolpathByUuid(uuid)->json.value("elements").toArray())
        out << v.toObject().value("uuid").toString();
    return out;
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QTemporaryDir tmp;
    QString err;
    const QString blank = tmp.filePath("a.c2d");
    c2d::Document::createBlank(blank, 300, 300, 19, &err);
    c2d::Document d;
    d.load(blank, &err);
    const c2d::Element small = c2d::Element::makeRectangle({50, 50}, 40, 40, d.defaultLayer());
    const c2d::Element big = c2d::Element::makeRectangle({100, 100}, 150, 150, d.defaultLayer());
    const c2d::Element far = c2d::Element::makeRectangle({250, 250}, 20, 20, d.defaultLayer());
    d.addElement(small);
    d.addElement(big);
    d.addElement(far);
    const c2d::Toolpath a = c2d::makeToolpath(d, "pocket_toolpath", {small.id});
    const c2d::Toolpath b = c2d::makeToolpath(d, "pocket_toolpath", {big.id, far.id});
    d.insertToolpath(0, a);
    d.insertToolpath(1, b);
    c2d::Canvas canvas;
    c2d::ToolpathPanel panel(&canvas);
    canvas.setDocument(&d);
    panel.setDocument(&d);
    panel.resize(500, 1200);
    panel.show();
    QApplication::setActiveWindow(&panel);
    QTest::qWaitForWindowActive(&panel);

    QTreeWidget *list = panel.findChild<QTreeWidget *>();
    const int headH = QFontMetrics(list->font()).height() * 2 + 22;
    auto rowOf = [&](int i) { return list->visualItemRect(list->topLevelItem(i)); };

    // Selecting the lower pocket: only what the one above left, in amber.
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, rowOf(1).center() - QPoint(0, 10));
    QTest::qWait(50);
    check(canvas.vectorHighlightPartial(), "the lower pocket's halo is what is left");
    const QPainterPath left = canvas.vectorHighlightLeft();
    check(!left.contains(QPointF(50, 50)) && left.contains(QPointF(150, 150)),
          "the part the pocket above cleared is left out");
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, rowOf(0).center() - QPoint(0, 10));
    QTest::qWait(50);
    check(!canvas.vectorHighlightPartial(), "the pocket on top shows all of its shapes");

    // "v" opens the card: it grows by a row of thumbnails.
    const QRect closed = rowOf(1);
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, QPoint(closed.center().x(), closed.bottom() - 10));
    QTest::qWait(50);
    const QRect opened = rowOf(1);
    check(opened.height() > closed.height() + 50, "v opens the card to show its shapes");

    // The bin on the first thumbnail takes that shape out, undoably.
    const QRect c = opened.adjusted(2, 3, -2, -3);
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, QPoint(c.left() + 61, c.top() + headH + 23));
    QTest::qWait(50);
    check(idsOf(d, b.uuid) == QStringList{far.id}, "the bin takes that one shape out of the toolpath");
    check(d.elementById(big.id) != nullptr, "the shape stays in the drawing");
    check(rowOf(1).height() > closed.height() + 50, "the card stays open");
    canvas.undoStack()->undo();
    QTest::qWait(50);
    check(idsOf(d, b.uuid) == (QStringList{big.id, far.id}), "undo puts the shape back");

    // "v" again closes it.
    const QRect again = rowOf(1);
    const int stripY = again.top() + 3 + headH + 1;
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, QPoint(again.center().x(), stripY));
    QTest::qWait(50);
    check(rowOf(1).height() == closed.height(), "v closes it again");
    return failures ? 1 : 0;
}
