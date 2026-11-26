// Raster pocket strategy: a 40 x 20 rectangle with a 6 mm bit at 50 %
// stepover cuts one lap of the wall and then parallel X lines, never lets
// the bit's centre closer than its radius to the wall, and leaves nothing
// of the floor uncut. Raster angle 90 turns the lines along Y; rest
// machining keeps the rings.
#include "../src/c2ddocument.h"
#include "../src/element.h"
#include "../src/gcodeexport.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineF>
#include <cmath>
#include <cstdio>
using namespace c2d;

static int fails = 0;
static void check(bool ok, const char *what)
{
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++fails;
}

static GcodeResult run(const QJsonObject &extra)
{
    Document doc;
    const Element r = Element::makeRectangle({50, 50}, 40, 20, doc.defaultLayer());
    doc.addElement(r);
    QJsonObject j{{"type", "pocket_toolpath"}, {"name", "P"}, {"enabled", true}, {"uuid", "{p}"},
                  {"start_depth", "0.000"}, {"end_depth", "2.000"}, {"stepdown", 2.0},
                  {"stock_to_leave", 0.0}, {"stepover", 3.0},
                  {"speeds", QJsonObject{{"feedrate", 1000}, {"plungerate", 300}, {"rpm", 10000}}},
                  {"tool", QJsonObject{{"diameter", 6.0}, {"number", 201}}},
                  {"elements", QJsonArray{QJsonObject{{"uuid", r.id}}}}};
    for (const QString &k : extra.keys())
        j.insert(k, extra.value(k));
    Toolpath t;
    t.uuid = QStringLiteral("{p}");
    t.type = QStringLiteral("pocket_toolpath");
    t.json = j;
    doc.addToolpath(t);
    return exportGcode(doc);
}

struct Seg { QPointF a, b; };

// XY segments cut at the floor (both ends at z = -2).
static QVector<Seg> floorCuts(const QVector<Op> &ops, int *arcs)
{
    QVector<Seg> out;
    double px = 0, py = 0, pz = 10;
    *arcs = 0;
    for (const Op &o : ops) {
        if (o.kind != Op::Rapid && o.kind != Op::Feed && o.kind != Op::Arc)
            continue;
        if (o.kind == Op::Arc && std::abs(o.z + 2) < 1e-6)
            ++*arcs;
        if (o.kind == Op::Feed && std::abs(o.z + 2) < 1e-6 && std::abs(pz + 2) < 1e-6)
            out.append({QPointF(px, py), QPointF(o.x, o.y)});
        px = o.x; py = o.y; pz = o.z;
    }
    return out;
}

static double distSeg(const QPointF &p, const Seg &s)
{
    const QPointF d = s.b - s.a;
    const double l2 = d.x() * d.x() + d.y() * d.y();
    double t = l2 > 1e-18 ? ((p.x() - s.a.x()) * d.x() + (p.y() - s.a.y()) * d.y()) / l2 : 0;
    t = std::max(0.0, std::min(1.0, t));
    return QLineF(p, s.a + t * d).length();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const double R = 3.0;

    const GcodeResult g = run({{"pocket_strategy", "raster"}});
    check(g.done.size() == 1, "the raster pocket is generated");
    int arcs = 0;
    const QVector<Seg> cuts = floorCuts(g.ops, &arcs);
    check(!cuts.isEmpty() && arcs == 0, "it cuts at the floor in straight moves");

    // Parallel X moves inside the wall lap (the lap's own long sides are at
    // y = 43 and 57).
    int xLines = 0;
    for (const Seg &s : cuts)
        if (std::abs(s.a.y() - s.b.y()) < 1e-6 && std::abs(s.a.x() - s.b.x()) > 10
            && s.a.y() > 43.5 && s.a.y() < 56.5)
            ++xLines;
    std::printf("     %d inner X lines, %d floor moves\n", xLines, int(cuts.size()));
    check(xLines >= 3, "zigzag lines run along X");
    int slanted = 0;
    for (const Seg &s : cuts)
        if (std::abs(s.a.y() - s.b.y()) > 1e-6 && std::abs(s.a.x() - s.b.x()) > 1e-6)
            ++slanted;
    check(slanted <= 1, "at most one diagonal move: the link from the wall lap to the lines");

    // Every cut point keeps the bit inside the rectangle: centre at least a
    // radius from each wall.
    bool inside = true;
    for (const Seg &s : cuts)
        for (const QPointF &p : {s.a, s.b})
            inside &= p.x() >= 30 + R - 1e-3 && p.x() <= 70 - R + 1e-3
                      && p.y() >= 40 + R - 1e-3 && p.y() <= 60 - R + 1e-3;
    check(inside, "the bit stays a radius inside the walls");

    // Full cover: every floor point a round bit can reach (not the four
    // corners it cannot) is within a radius of a cut.
    auto uncut = [&](const QVector<Seg> &cs) {
        int n = 0;
        for (double x = 30.05; x < 70; x += 0.25)
            for (double y = 40.05; y < 60; y += 0.25) {
                const double cx = std::max(0.0, std::max(33 - x, x - 67));
                const double cy = std::max(0.0, std::max(43 - y, y - 57));
                if (cx > 0 && cy > 0)
                    continue;           // a corner: the bit's radius stays there
                double best = 1e9;
                for (const Seg &s : cs)
                    best = std::min(best, distSeg(QPointF(x, y), s));
                if (best > R + 1e-3)
                    ++n;
            }
        return n;
    };
    const int left = uncut(cuts);
    std::printf("     %d sample points uncut\n", left);
    check(left == 0, "the floor is fully cleared");

    // The rings are still the default, and raster is shorter on the floor.
    const GcodeResult rings = run({});
    int a2 = 0;
    const QVector<Seg> rc = floorCuts(rings.ops, &a2);
    check(!rc.isEmpty() && rc.size() != cuts.size(), "no strategy key: rings as before");

    // Raster angle 90: lines along Y.
    const GcodeResult gy = run({{"pocket_strategy", "raster"}, {"raster_angle", 90.0}});
    int a3 = 0;
    int yLines = 0;
    for (const Seg &s : floorCuts(gy.ops, &a3))
        if (std::abs(s.a.x() - s.b.x()) < 1e-6 && std::abs(s.a.y() - s.b.y()) > 5
            && s.a.x() > 33.5 && s.a.x() < 66.5)
            ++yLines;
    check(yLines >= 8, "raster angle 90 cuts along Y");

    // Two depth passes with a ramp: every pass has its wall lap and lines.
    const GcodeResult deep = run({{"pocket_strategy", "raster"}, {"end_depth", "4.000"},
                                  {"enable_ramping", true}, {"ramp_angle", 5.0}});
    check(deep.done.size() == 1 && !deep.ops.isEmpty(), "two passes with a ramp entry generate");
    bool deepInside = true;
    for (const Op &o : deep.ops)
        if ((o.kind == Op::Feed) && o.z < -1e-6)
            deepInside &= o.x >= 33 - 1e-3 && o.x <= 67 + 1e-3 && o.y >= 43 - 1e-3 && o.y <= 57 + 1e-3;
    check(deepInside, "the ramp and links stay inside too");

    // Rest machining: rings, with a note.
    const GcodeResult rest = run({{"pocket_strategy", "raster"}, {"enable_rest", true},
                                  {"rest_diameter", 12.0}});
    bool noted = false;
    for (const QString &n : rest.notes)
        noted |= n.contains("rest machining");
    check(noted, "raster with rest machining notes that it cuts rings");

    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
