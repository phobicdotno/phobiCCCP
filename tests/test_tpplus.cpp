// Selecting shapes for a toolpath (Phobic, 2026-10-26): a select band dragged
// on over the panels stops at the edge of the drawing instead of picking up
// shapes off screen; the dotted "+" adds exactly what is selected; deleting a
// shape takes it out of its toolpaths (undo puts it back), and a file that
// still names a deleted shape loads without it; a shape's tile on the card
// selects that shape and brings it to the middle at the same zoom.
#define private public
#include "../src/mainwindow.h"
#include "../src/canvas.h"
#include "../src/toolpathpanel.h"
#undef private
#include "../src/c2ddocument.h"
#include "../src/element.h"
#include "../src/toolpathfactory.h"
#include <QApplication>
#include <QJsonArray>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QUndoStack>
#include <algorithm>
#include <cstdio>

static int failures = 0;
static void check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++failures;
}

static QStringList sorted(QStringList l)
{
    std::sort(l.begin(), l.end());
    return l;
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("phobicccp-test"));
    QCoreApplication::setApplicationName(QStringLiteral("test_tpplus"));
    c2d::MainWindow w;
    w.resize(1400, 900);
    w.show();
    w.newUntitled(400, 200, 19);
    c2d::Document &d = w.m_doc;
    QStringList grid;   // one row of ten 20 mm squares, 36 mm apart
    for (int col = 0; col < 10; ++col) {
        const c2d::Element e = c2d::Element::makeRectangle({20.0 + col * 36, 40.0}, 20, 20, d.defaultLayer());
        d.addElement(e);
        grid << e.id;
    }
    const c2d::Toolpath p = c2d::makeToolpath(d, "pocket_toolpath", {grid.at(0)});
    d.insertToolpath(0, p);
    w.m_canvas->setDocument(&d);
    w.m_tp->setDocument(&d);
    w.setMode(c2d::MainWindow::Mode::Toolpaths, false);
    QTest::qWait(100);
    c2d::Canvas *cv = w.m_canvas;
    QTreeWidget *list = w.m_tp->m_list;
    auto ids = [&] {
        QStringList out;
        for (const QJsonValue &v : d.toolpathByUuid(p.uuid)->json.value("elements").toArray())
            out << v.toObject().value("uuid").toString();
        return out;
    };

    // Zoomed in on the left end: the squares from the sixth on are off
    // screen to the right.
    cv->resetTransform();
    cv->scale(5, -5);
    cv->centerOn(QPointF(90, 40));
    QTest::qWait(50);
    const QRect vp = cv->viewport()->rect();
    int visible = 0;
    for (const QString &id : grid) {
        const QRect r = cv->mapFromScene(d.elementById(id)->painterPath.boundingRect()).boundingRect();
        if (vp.contains(r))
            ++visible;
    }
    check(visible >= 2 && visible < 8, "some squares are off screen to the right");

    // Select the toolpath card and open its shapes.
    QRect row = list->visualItemRect(list->topLevelItem(0));
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, row.center() - QPoint(0, 10));
    QTest::qWait(50);
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, QPoint(row.center().x(), row.bottom() - 10));
    QTest::qWait(50);

    // A band from just left of the second square, dragged on past the right
    // edge of the drawing (over the panels), left to right.
    const QPoint a = cv->mapFromScene(QPointF(40, 25));
    const QPoint b(vp.right() + 400, cv->mapFromScene(QPointF(0, 55)).y());
    QTest::mousePress(cv->viewport(), Qt::LeftButton, {}, a);
    for (int i = 1; i <= 10; ++i)
        QTest::mouseMove(cv->viewport(), a + (b - a) * i / 10);
    QTest::mouseRelease(cv->viewport(), Qt::LeftButton, {}, b);
    QTest::qWait(50);
    QStringList want;   // what lies wholly inside the band as far as it shows
    for (const QString &id : grid.mid(1)) {
        const QRect r = cv->mapFromScene(d.elementById(id)->painterPath.boundingRect()).boundingRect();
        if (vp.contains(r))
            want << id;
    }
    const QStringList sel = cv->selectedElementIds();
    check(!want.isEmpty() && sorted(sel) == sorted(want),
          "a band dragged over the panels picks only the shapes on screen");

    // "+" adds exactly those.
    {
        const QRect c = list->visualItemRect(list->topLevelItem(0)).adjusted(2, 3, -2, -3);
        const int headH = QFontMetrics(list->font()).height() * 2 + 22;
        QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, QPoint(c.left() + 8 + 70 + 32, c.top() + headH + 11 + 32));
        QTest::qWait(100);
    }
    check(sorted(ids()) == sorted(QStringList{grid.at(0)} + want), "+ adds exactly the selected shapes");

    // A shape's tile selects it and brings it to the middle; the zoom stays.
    {
        cv->centerOn(QPointF(300, 150));
        QTest::qWait(50);
        const double zoom = cv->transform().m11();
        const QRect c = list->visualItemRect(list->topLevelItem(0)).adjusted(2, 3, -2, -3);
        const int headH = QFontMetrics(list->font()).height() * 2 + 22;
        // The first tile, away from its bin in the top right corner.
        QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, QPoint(c.left() + 8 + 16, c.top() + headH + 11 + 48));
        QTest::qWait(100);
        check(cv->selectedElementIds() == QStringList{grid.at(0)}, "a tile selects its shape");
        const QPointF mid = cv->mapToScene(cv->viewport()->rect().center());
        check(QLineF(mid, QPointF(20, 40)).length() < 2.0, "and brings it to the middle");
        check(cv->transform().m11() == zoom, "at the same zoom");
    }

    // Deleting a shape takes it out of the toolpath; undo puts it back.
    const QStringList before = ids();
    cv->deleteElements({grid.at(1)});
    QTest::qWait(50);
    check(!ids().contains(grid.at(1)) && ids().size() == before.size() - 1,
          "a deleted shape leaves the toolpath");
    cv->undoStack()->undo();
    QTest::qWait(50);
    check(ids() == before, "undo puts it back in the toolpath");
    cv->undoStack()->redo();
    QTest::qWait(50);
    check(!ids().contains(grid.at(1)), "redo takes it out again");

    // A file that still names a deleted shape loads without it.
    {
        QTemporaryDir tmp;
        QJsonObject j = d.toolpathByUuid(p.uuid)->json;
        QJsonArray refs = j.value("elements").toArray();
        refs.append(QJsonObject{{QStringLiteral("uuid"), QStringLiteral("{00000000-0000-0000-0000-00000000dead}")}});
        j.insert("elements", refs);
        c2d::Toolpath t = *d.toolpathByUuid(p.uuid);
        t.json = j;
        d.replaceToolpath(t);
        const QString path = tmp.filePath("dangling.c2d");
        QString err;
        check(d.save(path, &err), "saved");
        c2d::Document again;
        check(again.load(path, &err), "loaded");
        const c2d::Toolpath *t2 = again.toolpathByUuid(p.uuid);
        check(t2 && t2->json.value("elements").toArray().size() == refs.size() - 1,
              "a vector whose shape is gone is dropped on load");
    }
    return failures ? 1 : 0;
}
