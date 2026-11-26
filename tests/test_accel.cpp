// Time estimates planned like GRBL: acceleration on every move, corners at
// the junction-deviation speed, rapids at the axes' max rates. Checked on a
// single move (trapezoid math), a zig-zag of tiny segments, rapids, the job
// plan read from g-code, and a million moves in well under a second.
#include "../src/jobview.h"
#include "../src/post_grbl.h"
#include <QApplication>
#include <QElapsedTimer>
#include <cmath>
#include <cstdio>
using namespace c2d;
static int fails = 0;
static void check(bool ok, const char *what) { std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++fails; }

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    MotionLimits lim;
    for (int i = 0; i < 3; ++i)
        lim.rate[i] = 10000;
    setMotionLimits(lim);

    // One 100 mm move at 6000 mm/min (100 mm/s) with 400 mm/s²: 0.25 s up,
    // 0.25 s down (12.5 mm each), 75 mm cruising (0.75 s) = 1.25 s.
    {
        const QVector<Op> ops = {Op::rapid(0, 0, 0), Op::feedTo(100, 0, 0, 6000)};
        const double t = computeStats(ops).timeSec;
        const double want = 100.0 / 100.0 + 100.0 / 400.0;
        std::printf("      single move %.4f s (want %.4f)\n", t, want);
        check(std::abs(t - want) < want * 0.02, "a 100 mm move at 6000 mm/min with 400 mm/s² takes 100/100 + 100/400 s");
    }
    // Too short to reach the feed: a triangle, 2*sqrt(L/a).
    {
        const QVector<Op> ops = {Op::rapid(0, 0, 0), Op::feedTo(4, 0, 0, 6000)};
        check(std::abs(computeStats(ops).timeSec - 2 * std::sqrt(4.0 / 400)) < 1e-3, "a short move never reaches its feed");
    }
    // Straight on through many pieces: no slowing down between them.
    {
        QVector<Op> ops = {Op::rapid(0, 0, 0)};
        for (int i = 1; i <= 100; ++i)
            ops.append(Op::feedTo(i, 0, 0, 6000));
        check(std::abs(computeStats(ops).timeSec - 1.25) < 0.01, "collinear pieces run as one move");
    }
    // 1000 tiny zig-zag segments: every corner nearly stops.
    {
        QVector<Op> ops = {Op::rapid(0, 0, 0)};
        double len = 0;
        for (int i = 1; i <= 1000; ++i) {
            ops.append(Op::feedTo(i * 0.5, (i % 2) * 0.5, 0, 3000));
            len += std::sqrt(0.5);
        }
        const double t = computeStats(ops).timeSec, naive = len / 3000 * 60;
        std::printf("      zig-zag %.2f s vs length/feed %.2f s\n", t, naive);
        check(t > naive * 3, "a zig-zag of tiny segments takes much longer than length/feed");
    }
    // Rapids: still about length / max rate.
    {
        MotionLimits m;
        setMotionLimits(m);
        const QVector<Op> ops = {Op::rapid(0, 0, 0), Op::rapid(500, 0, 0)};
        const double t = computeStats(ops).timeSec, naive = 500.0 / 5000 * 60;
        check(t >= naive && t < naive * 1.5, "a long rapid stays near length / max rate");
        // Diagonal: each axis at its own limit, so the move runs faster than one axis.
        const QVector<Op> diag = {Op::rapid(0, 0, 0), Op::rapid(500, 500, 0)};
        check(computeStats(diag).timeSec < std::hypot(500, 500) / 5000 * 60, "a diagonal rapid uses both axes' rates");
    }
    // Z is slower to accelerate: the same move along Z takes longer.
    {
        const QVector<Op> x = {Op::rapid(0, 0, 0), Op::feedTo(20, 0, 0, 3000)};
        const QVector<Op> z = {Op::rapid(0, 0, 0), Op::feedTo(0, 0, -20, 3000)};
        check(computeStats(z).timeSec > computeStats(x).timeSec, "Z's lower acceleration shows");
    }
    // A small circle is turned slower than its feed: v = sqrt(a r).
    {
        const QVector<Op> ops = {Op::rapid(1, 0, 0), Op::arcTo(1, 0, 0, -1, 0, false, 6000)};
        const double t = computeStats(ops).timeSec;
        check(t > 2 * M_PI * 1 / std::sqrt(400.0 * 1), "a small circle runs at most at sqrt(a r)");
    }
    // A dwell stops the machine: the moves around it are planned apart.
    {
        const QVector<Op> a = {Op::rapid(0, 0, 0), Op::feedTo(50, 0, 0, 6000), Op::feedTo(100, 0, 0, 6000)};
        const QVector<Op> b = {Op::rapid(0, 0, 0), Op::feedTo(50, 0, 0, 6000), Op::dwell(0), Op::feedTo(100, 0, 0, 6000)};
        check(computeStats(b).timeSec > computeStats(a).timeSec + 0.2, "a dwell brings the machine to rest");
    }
    // The job plan from g-code uses the same planner.
    {
        const QStringList lines = {QStringLiteral("G21"), QStringLiteral("G90"), QStringLiteral("G0X0Y0Z0"),
                                   QStringLiteral("G1X100.000F6000"), QStringLiteral("M05")};
        const JobPlan plan = planJob(lines, {}, nullptr, 5000);
        // X's max rate (5000 mm/min) caps the feed: 100/83.3 + 83.3/400 s.
        const double v = 5000.0 / 60;
        check(std::abs(plan.cumSec.last() - (100 / v + v / 400)) < 0.03, "the job plan's times come from the planner");
        const QVector<Op> ops = {Op::rapid(0, 0, 0), Op::feedTo(100, 0, 0, 6000)};
        check(std::abs(plan.cumSec.last() - computeStats(ops).timeSec) < 1e-6, "job plan and estimate agree");
    }
    // A million moves: planned in well under a second.
    {
        QVector<Op> ops;
        ops.reserve(1000001);
        ops.append(Op::rapid(0, 0, 0));
        for (int i = 1; i <= 1000000; ++i)
            ops.append(Op::feedTo(std::fmod(i * 0.37, 300.0), std::fmod(i * 0.11, 200.0), -(i % 7) * 0.1, 2000));
        QElapsedTimer t;
        t.start();
        const double sec = computeStats(ops).timeSec;
        const qint64 ms = t.elapsed();
        std::printf("      1M moves: %lld ms, %.0f s estimated\n", (long long)ms, sec);
        check(ms < 1000 && sec > 0, "a million moves are estimated in under a second");
    }
    std::printf("%d failures\n", fails);
    return fails ? 1 : 0;
}
