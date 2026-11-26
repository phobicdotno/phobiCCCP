// Surfacing from the Machine page (surfacing.cpp): zigzag lines at the
// stepover from one radius before the edge to one radius past it, depth
// passes, M3 before cutting and M5 at the end, and an area that leaves the
// travel is refused.
//
// Plain asserts, no framework; exits 0 on success.

#include "../src/ncprogram.h"
#include "../src/post_grbl.h"
#include "../src/surfacing.h"

#include <QSet>
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

static bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

int main()
{
    SurfacingParams p;
    p.width = 100;
    p.height = 50;
    p.toolDia = 20;
    p.stepoverPct = 40;
    p.stepdown = 0.5;
    p.depth = 0.5;
    const SurfacingPlan plan = planSurfacing(p);
    check(plan.ok(), "plan made");
    check(plan.passes == 1, "one pass");
    // -10, -2, 6, ..., 54, then the last pulled to 60 (= 50 + r).
    check(plan.lines == 10, "ten lines");
    QVector<double> ys;
    double minZ = 0;
    for (const Op &o : plan.ops)
        if (o.kind == Op::Feed) {
            if (ys.isEmpty() || !near(ys.last(), o.y))
                ys << o.y;
            minZ = std::min(minZ, o.z);
            check(o.x >= -10 - 1e-9 && o.x <= 110 + 1e-9, "x inside -r..W+r");
            if (o.z < 0)
                check(near(o.z, -0.5), "cuts at Z-0.5");
        }
    check(ys.size() == 10, "ten distinct line ys");
    for (int k = 0; k < 9; ++k)
        check(near(ys.at(k), -10 + 8 * k), "line at -10 + 8k");
    check(near(ys.last(), 60), "last line at 50 + r");
    check(near(minZ, -0.5), "deepest is -0.5");
    check(near(plan.minX, -10) && near(plan.maxX, 110), "x extent -r..W+r");
    bool sawLeft = false, sawRight = false;
    for (const Op &o : plan.ops)
        if (o.kind == Op::Feed && o.z < 0) {
            sawLeft = sawLeft || near(o.x, -10);
            sawRight = sawRight || near(o.x, 110);
        }
    check(sawLeft && sawRight, "lines reach both ends");

    // Through the post and the G-code reader the Machine page runs.
    const QString text = GrblPost().generate(plan.ops);
    const NcProgram nc = readNcProgram(text);
    check(nc.ok(), "program reads back");
    int m3 = -1, firstCut = -1, m5 = -1;
    for (int i = 0; i < nc.lines.size(); ++i) {
        const QString &l = nc.lines.at(i);
        if (m3 < 0 && l.contains(QLatin1String("M03")))
            m3 = i;
        if (firstCut < 0 && l.startsWith(QLatin1String("G1")) && l.contains(QLatin1String("Z-0.5")))
            firstCut = i;
        if (l.contains(QLatin1String("M05")))
            m5 = i;
    }
    check(m3 >= 0 && firstCut > m3, "M3 before cutting");
    check(m5 > firstCut, "M5 after cutting");
    for (int i = m5 + 1; i < nc.lines.size(); ++i)
        check(!nc.lines.at(i).startsWith(QLatin1String("G1")), "nothing cut after M5");

    // A full circle named by its centre alone is a move for the reader too.
    {
        const NcProgram c = readNcProgram(QStringLiteral("G90 G21\nG0 X0 Y0 Z1\nG1 Z-1 F300\nG2 I5 J0 F800\n"));
        check(c.ok() && !c.ops.isEmpty() && c.ops.last().kind == Op::Arc && near(c.ops.last().x, 0)
                  && near(c.ops.last().ci, 5) && c.ops.last().cw,
              "nc: G2 with I/J only is a full-circle arc op");
    }

    // Two passes of 0.5 for 1 mm, lines along Y, climb finish round.
    SurfacingParams q = p;
    q.depth = 1.0;
    q.alongX = false;
    q.finishClimb = true;
    const SurfacingPlan two = planSurfacing(q);
    check(two.ok() && two.passes == 2, "two passes");
    // Across X: -10 .. 110 at 8 = 15 steps + the end.
    check(two.lines == 16, "sixteen lines along Y");
    QSet<QString> zs;
    for (const Op &o : two.ops)
        if (o.kind == Op::Feed && o.z < 0)
            zs.insert(QString::number(o.z, 'f', 3));
    check(zs == QSet<QString>({QStringLiteral("-0.500"), QStringLiteral("-1.000")}), "Z -0.5 then -1");
    // Finish: the last four feeds go anticlockwise round the rectangle.
    QVector<Op> feeds;
    for (const Op &o : two.ops)
        if (o.kind == Op::Feed)
            feeds << o;
    const int n = feeds.size();
    check(near(feeds[n - 4].x, 110) && near(feeds[n - 4].y, -10), "finish: along the front to the right");
    check(near(feeds[n - 3].x, 110) && near(feeds[n - 3].y, 60), "finish: up the right side");
    check(near(feeds[n - 2].x, -10) && near(feeds[n - 2].y, 60), "finish: across the back");
    check(near(feeds[n - 1].x, -10) && near(feeds[n - 1].y, -10), "finish: down the left");

    // Travel check: machine X -800..-2, Y -800..-2; work zero at machine
    // (-700, -700): the area spans -710..-590 in X, -710..-640 in Y: fits.
    check(surfacingFitProblem(plan, -700, -700, -800, -2, -800, -2).isEmpty(), "fits");
    // Zero at (-50, -700): x runs to -50 + 110 = +60, past the right end.
    const QString why = surfacingFitProblem(plan, -50, -700, -800, -2, -800, -2);
    check(why.contains(QLatin1String("right end")), "refused past the right end");
    // Zero right at the front-left limit: the -r lead-in leaves the travel.
    check(!surfacingFitProblem(plan, -800, -800, -800, -2, -800, -2).isEmpty(), "refused past front-left");

    // Nonsense is refused.
    SurfacingParams bad = p;
    bad.toolDia = 0;
    check(!planSurfacing(bad).ok(), "no bit refused");

    // A plan of hundreds of millions of moves is refused before it is built.
    SurfacingParams huge = p;
    huge.width = 5000;
    huge.height = 5000;
    huge.toolDia = 1;
    huge.stepoverPct = 5;
    huge.depth = 50;
    huge.stepdown = 0.05;
    const SurfacingPlan hp = planSurfacing(huge);
    check(!hp.ok() && hp.ops.isEmpty(), "an oversized plan is refused");

    std::printf("surfacing: %d checks passed\n", g_checks);
    return 0;
}
