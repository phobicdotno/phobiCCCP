// Cutting time on the toolpath cards: roughDuration()'s wording, and a
// simple contour's card and the ticked total showing its estimate once the
// program is built (timeByComment over the export's moves).
#include "../src/c2ddocument.h"
#include "../src/canvas.h"
#include "../src/element.h"
#include "../src/gcodeexport.h"
#include "../src/post_grbl.h"
#include "../src/toolpathfactory.h"
#include "../src/toolpathpanel.h"
#include <QApplication>
#include <QLabel>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <cstdio>
using namespace c2d;
static int fails = 0;
static void check(bool ok, const char *what) { std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++fails; }

static QString subtitle(const QTreeWidgetItem *it) { return it->data(0, Qt::UserRole + 1).toString(); }

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    check(roughDuration(0) == QStringLiteral("< 1 min"), "0 s is < 1 min");
    check(roughDuration(25) == QStringLiteral("< 1 min"), "25 s is < 1 min");
    check(roughDuration(4 * 60 + 10) == QString::fromUtf8("≈ 4 min"), "4 min 10 s is ~4 min");
    check(roughDuration(72 * 60) == QString::fromUtf8("≈ 1 h 12 min"), "72 min is ~1 h 12 min");
    check(roughDuration(120 * 60 + 5) == QString::fromUtf8("≈ 2 h"), "2 h on the dot drops the minutes");
    check(roughDuration(-5) == QStringLiteral("< 1 min"), "negative is < 1 min");

    QTemporaryDir tmp; QString err;
    const QString blank = tmp.filePath("a.c2d");
    Document::createBlank(blank, 200, 200, 19, &err);
    Document d; d.load(blank, &err);
    const Element r1 = Element::makeRectangle({50, 50}, 40, 40, d.defaultLayer());
    d.addElement(r1);
    Toolpath a = makeToolpath(d, "contour", {r1.id});
    d.insertToolpath(0, a);
    Canvas canvas;
    ToolpathPanel panel(&canvas);
    canvas.setDocument(&d);
    panel.setDocument(&d);
    panel.resize(500, 900);
    panel.show();

    auto *list = panel.findChild<QTreeWidget *>();
    auto *total = panel.findChild<QLabel *>(QStringLiteral("tpTotalTime"));
    if (!list || !total || list->topLevelItemCount() != 1) { std::printf("panel not as expected\n"); return 2; }
    check(!subtitle(list->topLevelItem(0)).contains(QStringLiteral("min")), "no time before the moves are built");
    check(total->isHidden(), "no total before the moves are built");

    Document copy = d;
    const GcodeResult r = exportGcode(copy);
    const QHash<QString, double> sec = timeByComment(r.ops);
    const QString name = a.json.value("name").toString();
    check(sec.value(name) > 0, "the contour's share of the program takes time");
    double sum = 0;
    for (double s : sec) sum += s;
    check(std::abs(sum - computeStats(r.ops).timeSec) < 1.0, "the shares add up to the program's estimate");

    panel.setTimeEstimates(sec);
    const QString sub = subtitle(list->topLevelItem(0));
    check(sub.endsWith(roughDuration(sec.value(name))), "the card shows the contour's time");
    check(!total->isHidden() && total->text().contains(roughDuration(sec.value(name))), "the total shows the ticked time");

    panel.clearTimeEstimates();
    check(!subtitle(list->topLevelItem(0)).contains(QStringLiteral("min")), "stale moves: the time goes");
    check(total->isHidden(), "stale moves: the total goes");

    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
