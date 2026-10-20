// Surface map (surfacemap.h): the interpolation, its storage, the probe
// order and macros, and what applying it does to a program. The probing run
// itself is driven against the simulator by tools/flowtest.sh through
// `phobicccp --grbl-map`.

#include "surfacemap.h"

#include <QRegularExpression>

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

static bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

// A tilted plane sampled on a grid: bilinear must reproduce it exactly.
static double plane(double x, double y) { return -40.0 + 0.01 * x - 0.004 * y; }

static SurfaceMap planeMap(double x0, double y0, double x1, double y1, int cols, int rows)
{
    SurfaceMap m;
    m.x0 = x0; m.y0 = y0; m.x1 = x1; m.y1 = y1; m.cols = cols; m.rows = rows;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c)
            m.z.append(plane(m.xAt(c), m.yAt(r)));
    return m;
}

int main()
{
    // ---- interpolation ----------------------------------------------------
    const SurfaceMap m = planeMap(0, 0, 200, 100, 4, 3);
    check(m.isValid(), "a 4 x 3 grid is valid");
    for (double x : {0.0, 13.0, 66.6, 150.0, 200.0})
        for (double y : {0.0, 27.0, 50.0, 99.0})
            check(near(m.heightAt(x, y), plane(x, y)), "bilinear reproduces a plane");
    check(near(m.offsetAt(0, 0), 0), "offset is zero at X0 Y0");
    check(near(m.offsetAt(200, 100), plane(200, 100) - plane(0, 0)), "offset counts from X0 Y0");
    check(near(m.heightAt(-50, 50), plane(0, 50)), "left of the grid the edge is held");
    check(near(m.heightAt(260, 140), plane(200, 100)), "past the far corner the corner is held");

    // A bump in the middle of a 3 x 3: bilinear in each cell, not a plane.
    SurfaceMap bump = planeMap(0, 0, 100, 100, 3, 3);
    for (double &v : bump.z) v = 0;
    bump.z[4] = 1.0;
    check(near(bump.heightAt(50, 50), 1.0), "bump peak");
    check(near(bump.heightAt(25, 50), 0.5), "halfway to the peak along X");
    check(near(bump.heightAt(25, 25), 0.25), "bilinear inside a cell");

    // The grid kept clear of the stock edges: X0 Y0 lies outside it, and the
    // reference is then the nearest edge point.
    const SurfaceMap inset = planeMap(10, 10, 90, 90, 3, 3);
    check(near(inset.offsetAt(10, 10), 0), "inset grid: reference at its corner nearest X0 Y0");
    check(near(inset.offsetAt(90, 90), plane(90, 90) - plane(10, 10)), "inset grid: far corner");
    double lo, hi;
    inset.offsetRange(&lo, &hi);
    check(near(lo, plane(10, 90) - plane(10, 10)) && near(hi, plane(90, 10) - plane(10, 10)),
          "offset range over the grid");

    // One row of points (a long thin board) interpolates along X only.
    const SurfaceMap strip = planeMap(0, 20, 300, 20, 4, 1);
    check(strip.isValid(), "a single row is a valid map");
    check(near(strip.heightAt(120, 80), plane(120, 20)), "single row: Y is ignored");

    // ---- validity and storage --------------------------------------------
    SurfaceMap bad = m;
    bad.z.removeLast();
    check(!bad.isValid(), "wrong number of heights is invalid");
    bad = m;
    bad.z[3] = std::nan("");
    check(!bad.isValid(), "a NaN height is invalid");
    SurfaceMap one;
    one.cols = one.rows = 1;
    one.z = {1.0};
    check(!one.isValid(), "one point is not a map");
    check(near(SurfaceMap().offsetAt(10, 10), 0), "an empty map offsets nothing");

    const SurfaceMap back = SurfaceMap::fromJson(m.toJson());
    check(back.isValid() && back.cols == 4 && back.rows == 3 && back.z == m.z
              && back.x1 == 200 && back.y1 == 100, "JSON round trip");
    check(!SurfaceMap::fromJson(QStringLiteral("{nonsense")).isValid(), "broken JSON gives no map");
    check(!SurfaceMap::fromJson(QString()).isValid(), "empty setting gives no map");
    check(!SurfaceMap::fromJson(QStringLiteral(
              R"({"x0":0,"y0":0,"x1":10,"y1":10,"cols":2,"rows":2,"z":[0,1,"2",3]})")).isValid(),
          "a string height is rejected");

    // ---- probe order --------------------------------------------------------
    const QVector<GridPoint> order = surfaceMapProbeOrder(10, 10, 90, 90, 3, 3);
    check(order.size() == 9, "nine points");
    QVector<int> seen(9, 0);
    for (const GridPoint &p : order)
        ++seen[p.index];
    check(!seen.contains(0) && !seen.contains(2), "every grid point probed once");
    check(near(order[2].x, 90) && near(order[3].x, 90) && near(order[3].y, 50),
          "rows alternate direction (no run back across the board)");
    for (int i = 1; i < order.size(); ++i)
        check(std::hypot(order[i].x - order[i - 1].x, order[i].y - order[i - 1].y) <= 40.0 + 1e-9,
              "each step goes to a neighbouring point");

    // ---- macros -----------------------------------------------------------------
    SurfaceMapPlan plan;
    plan.plate = 13;
    plan.clearance = 5;
    plan.maxDeviation = 4;
    const QStringList move = surfaceMapMoveLines(plan, 50, 25);
    check(move.size() == 3 && move[0] == QLatin1String("G90")
              && move[1] == QLatin1String("G0 Z18.000")
              && move[2] == QLatin1String("G0 X50.000 Y25.000"),
          "move: up to travel height first, then across");
    const QStringList probe = surfaceMapProbeLines(plan);
    check(probe.filter(QRegularExpression(QStringLiteral("^G38\\.2"))).size() == 2,
          "probe: fast and slow touch");
    check(probe[1].startsWith(QLatin1String("G38.2 Z-9.000")),
          "probe gives up max. dip below the expected plate top");
    check(probe.last() == QLatin1String("G0 Z18.000") && probe[probe.size() - 2] == QLatin1String("G90"),
          "probe ends back at travel height, absolute");
    const QRegularExpression xy(QStringLiteral("[XY]"));
    for (const QString &l : probe)
        check(!xy.match(l).hasMatch(), "probe macro never moves sideways");

    // ---- applying it to a program -------------------------------------------
    const SurfaceMap tilt = planeMap(0, 0, 100, 100, 2, 2);
    auto expectZ = [&](double x, double y, double z) { return z + plane(x, y) - plane(0, 0); };

    QVector<Op> prog = {
        Op::tool(1),
        Op::spindle(18000),
        Op::rapid(0, 0, 5),
        Op::rapid(100, 0, 5),
        Op::feedTo(100, 0, -1, 300),
        Op::feedTo(100, 100, -1, 1000),
        Op::feedTo(0, 100, -2, 1000),
        Op::rapid(0, 100, 5),
        Op::spindle(0),
    };
    const QVector<Op> out = applySurfaceMap(prog, tilt, 3.0);
    int rapids = 0;
    for (const Op &o : out)
        if (o.kind == Op::Rapid)
            ++rapids;
    // The first rapid has no known start: it clears the map's highest point
    // (+1.0 at X100 Y0), then drops to the offset under it.
    check(rapids == 4, "rapids stay single moves, plus one drop after the blind first one");
    check(out[2].kind == Op::Rapid && near(out[2].z, 5 + 1.0), "a rapid from nowhere clears the highest point");
    check(out[3].kind == Op::Rapid && near(out[3].x, 0) && near(out[3].z, expectZ(0, 0, 5)),
          "then drops to the offset at its end");
    check(out[4].kind == Op::Rapid && near(out[4].z, expectZ(100, 0, 5)), "rapid end gets the offset");
    double lx = 0, ly = 0;
    bool have = false;
    int feeds = 0;
    for (const Op &o : out) {
        if (o.kind == Op::Feed) {
            ++feeds;
            if (have)
                check(std::hypot(o.x - lx, o.y - ly) <= 3.0 + 1e-9, "feed pieces no longer than 3 mm");
        }
        if (o.kind == Op::Feed || o.kind == Op::Rapid) {
            lx = o.x; ly = o.y; have = true;
        }
    }
    check(feeds >= 1 + 34 + 34, "long feeds are cut up");
    // The ramp from Z-1 to Z-2 along Y = 100: each piece on the line and the
    // surface under it.
    for (const Op &o : out)
        if (o.kind == Op::Feed && near(o.y, 100) && o.x < 100) {
            const double zProg = -1.0 + (-1.0) * (100.0 - o.x) / 100.0;
            check(near(o.z, expectZ(o.x, o.y, zProg), 1e-6), "ramp follows program Z plus the surface");
        }
    check(out.first().kind == Op::Tool && out[1].kind == Op::Spindle && out.last().kind == Op::Spindle,
          "tool and spindle ops pass through");

    // An arc: a full circle of r 10 around (50, 50), clockwise.
    QVector<Op> circ = {Op::rapid(60, 50, 5), Op::feedTo(60, 50, -1, 300),
                        Op::arcTo(60, 50, -1, -10, 0, true, 800)};
    const QVector<Op> arcOut = applySurfaceMap(circ, tilt, 3.0);
    int chords = 0;
    double minAngle = 10, maxAngle = -10;
    for (int i = 3; i < arcOut.size(); ++i) {   // after the blind first rapid and its drop
        const Op &o = arcOut[i];
        check(o.kind == Op::Feed, "an arc becomes feeds");
        check(near(std::hypot(o.x - 50, o.y - 50), 10, 1e-6), "chord ends on the circle");
        check(near(o.z, expectZ(o.x, o.y, -1)), "arc points get the offset");
        ++chords;
        const double a = std::atan2(o.y - 50, o.x - 50);
        minAngle = std::min(minAngle, a);
        maxAngle = std::max(maxAngle, a);
    }
    // Sagitta of a chord of angle d on r 10 is 10 (1 - cos(d/2)) <= 0.005.
    check(chords >= int(std::ceil(2 * M_PI / (2 * std::acos(1 - 0.005 / 10)))), "chords within 0.005 mm");
    check(minAngle < -3.0 && maxAngle > 3.0, "the whole circle is swept");
    check(arcOut[3].y < 50, "clockwise from (60, 50) goes down first");

    // After a tool change the head is wherever the tool-change macro left it.
    QVector<Op> tc = {Op::rapid(0, 0, 5), Op::feedTo(0, 0, -1, 300), Op::tool(2),
                      Op::feedTo(100, 100, -1, 300)};
    const QVector<Op> tcOut = applySurfaceMap(tc, tilt, 3.0);
    check(tcOut.size() == 5 && tcOut[4].kind == Op::Feed && near(tcOut[4].x, 100),   // [0..1]: blind rapid + drop
          "first move after a tool change is taken whole");

    // A long rapid over a bump: lifted by the peak in between, not by its ends.
    {
        QVector<Op> hop = {Op::rapid(0, 50, 2), Op::feedTo(0, 50, -1, 300), Op::rapid(0, 50, 2),
                           Op::rapid(100, 50, 2), Op::feedTo(100, 50, -1, 300)};
        const QVector<Op> hopOut = applySurfaceMap(hop, bump, 3.0);
        int i = 0;
        while (i < hopOut.size() && !(hopOut[i].kind == Op::Rapid && near(hopOut[i].x, 100)))
            ++i;
        check(i + 1 < hopOut.size() && near(hopOut[i].z, 2 + 1.0),
              "a rapid across a high spot travels above it");
        check(hopOut[i + 1].kind == Op::Rapid && near(hopOut[i + 1].x, 100) && near(hopOut[i + 1].z, 2),
              "and comes down to the surface at its end");
        // Diagonal across a cell: the peak is passed between samples of the
        // ends only.
        const QVector<Op> diag = applySurfaceMap(
            {Op::rapid(0, 0, 2), Op::feedTo(0, 0, 0, 300), Op::rapid(0, 0, 2), Op::rapid(100, 100, 2)},
            bump, 3.0);
        double top = -1e9;
        for (const Op &o : diag)
            if (o.kind == Op::Rapid && near(o.x, 100))
                top = std::max(top, o.z);
        check(near(top, 3.0, 0.01), "a diagonal rapid over the peak is lifted by it");
        // Z-only rapids are exact already and stay one move.
        int zr = 0;
        for (const Op &o : hopOut)
            if (o.kind == Op::Rapid && near(o.x, 0))
                ++zr;
        check(zr == 3, "rapids that only move Z are not doubled (beyond the blind first one)");
    }

    // A flat map changes no Z.
    SurfaceMap flat = tilt;
    for (double &v : flat.z) v = -40;
    for (const Op &o : applySurfaceMap(prog, flat, 3.0))
        if (o.kind == Op::Feed || o.kind == Op::Rapid)
            check(o.z == 5 || o.z == -1 || (o.z <= -1 && o.z >= -2), "flat map leaves Z alone");
    check(applySurfaceMap(prog, SurfaceMap(), 3.0).size() == prog.size(), "no map, no change");

    std::printf("surfacemap: %d checks passed\n", g_checks);
    return 0;
}
