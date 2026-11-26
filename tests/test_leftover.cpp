// "Show what's left" in the simulation: the intended floors of the 2D
// toolpaths compared with the simulated stock. A 20 x 20 pocket cut with a
// 6 mm bit leaves its four corners; a 1 mm bit leaves far less.
// Plain asserts, no test framework; exits 0 on success.

#include "../src/c2ddocument.h"
#include "../src/element.h"
#include "../src/gcodeexport.h"
#include "../src/simpanel.h"
#include "../src/simulation.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>

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

// 40 x 40 x 10 stock, a 20 x 20 square at (10, 10), pocketed 3 mm deep.
static void makeDoc(Document &doc, double toolD, const QString &type = QStringLiteral("pocket_toolpath"))
{
    doc.setParam(QStringLiteral("width"), QStringLiteral("40"));
    doc.setParam(QStringLiteral("height"), QStringLiteral("40"));
    doc.setParam(QStringLiteral("thickness"), QStringLiteral("10"));
    const Element e = Element::makeRectangle({20, 20}, 20, 20, doc.defaultLayer());
    doc.addElement(e);
    QJsonObject j{{"type", type}, {"name", QStringLiteral("Pocket")}, {"enabled", true},
                  {"uuid", QStringLiteral("{tp-1}")}, {"start_depth", QStringLiteral("0.000")},
                  {"end_depth", QStringLiteral("3.000")}, {"stepdown", 3.0},
                  {"stepover", toolD * 0.4}, {"stock_to_leave", 0.0},
                  {"speeds", QJsonObject{{"feedrate", 1000}, {"plungerate", 300}, {"rpm", 10000}}},
                  {"tool", QJsonObject{{"diameter", toolD}, {"number", 201}, {"type", 0}}},
                  {"elements", QJsonArray{QJsonObject{{"uuid", e.id}}}}};
    Toolpath tp;
    tp.uuid = j.value("uuid").toString();
    tp.type = type;
    tp.json = j;
    doc.addToolpath(tp);
}

static LeftoverResult leftFor(double toolD)
{
    Document doc;
    makeDoc(doc, toolD);
    const GcodeResult g = exportGcode(doc);
    check(g.done.size() == 1, "the pocket exports");
    SimSettings s;
    s.stockW = 40; s.stockH = 40; s.stockT = 10;
    s.minCell = 0.1;
    const SimResult r = simulate(g.ops, toolGeometry(doc), s);
    const QVector<IntendedFloor> floors = intendedFloors(doc);
    check(floors.size() == 1, "one intended floor");
    check(std::abs(floors.first().z + 3.0) < 1e-9, "the pocket's floor is its end depth");
    return findLeftovers(r.map, floors);
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    // Contours are not checked: only the band along the line is cut.
    {
        Document doc;
        makeDoc(doc, 6.0, QStringLiteral("contour"));
        check(intendedFloors(doc).isEmpty(), "a contour has no intended floor");
    }

    const LeftoverResult big = leftFor(6.0);
    std::printf("6 mm: %d areas, %.3f mm2\n", int(big.areas.size()), big.totalArea);
    check(big.areas.size() == 4, "a 6 mm bit leaves the four corners");
    for (const LeftoverArea &a : big.areas) {
        check(a.box.width() < 3.2 && a.box.height() < 3.2, "each corner leftover is small");
        check(a.height > 2.5, "the corner stands nearly the full depth");
        const QPointF c = a.box.center();
        check((std::abs(c.x() - 10) < 2.5 || std::abs(c.x() - 30) < 2.5)
                  && (std::abs(c.y() - 10) < 2.5 || std::abs(c.y() - 30) < 2.5),
              "the leftovers sit in the corners");
    }
    check(big.mask.size() > 0, "the mask is filled");
    const QString line = leftoverSummary(big);
    check(line.startsWith(QStringLiteral("Left uncut: 4 areas, largest")), "summary counts 4 areas");
    check(line.contains(QStringLiteral("smaller bit")), "summary suggests a smaller bit");

    const LeftoverResult small = leftFor(1.0);
    std::printf("1 mm: %d areas, %.3f mm2\n", int(small.areas.size()), small.totalArea);
    check(small.totalArea < big.totalArea / 4, "a 1 mm bit leaves far less");

    // The panel: off computes nothing, on tints and shows the line.
    {
        Document doc;
        makeDoc(doc, 6.0);
        const GcodeResult g = exportGcode(doc);
        SimPanel panel;
        panel.setJob(g.ops, toolGeometry(doc), 40, 40, 10);
        panel.setIntendedFloors(intendedFloors(doc));
        panel.simulateBlocking();
        check(panel.leftovers().areas.isEmpty(), "nothing computed while off");
        const QImage plain = panel.image();
        panel.setShowLeftovers(true);
        check(panel.leftovers().areas.size() == 4, "toggled on: 4 areas");
        check(panel.image() != plain, "the leftovers are tinted");
        panel.setShowLeftovers(false);
        check(panel.leftovers().areas.isEmpty() && panel.image() == plain, "off again: plain view");
    }

    std::printf("leftover: %d checks passed\n", g_checks);
    return 0;
}
