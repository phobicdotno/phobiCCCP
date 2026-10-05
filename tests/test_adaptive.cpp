// Adaptive clearing (adaptive.h): checked independently of the planner's own
// bookkeeping. A separate, finer raster replays the plan in order and
// measures, at every point of every pass, how much of the cutter's front
// half is buried in material not yet cut; it also checks that the plan
// clears everything the tool can reach and never puts the tool where it
// may not go.
//
// Plain asserts, no test framework; exits 0 on success.

#include "../src/adaptive.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLineF>
#include <QPainterPath>
#include <QRectF>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <vector>

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

// Replay raster: cell cleared once a tool centre passed within r of it.
struct Replay {
    QRectF box;
    double res, r;
    int w, h;
    std::vector<unsigned char> cells;
    Replay(const QRectF &b, double toolR, double resolution)
        : box(b.adjusted(-toolR, -toolR, toolR, toolR)), res(resolution), r(toolR)
    {
        w = int(std::ceil(box.width() / res)) + 1;
        h = int(std::ceil(box.height() / res)) + 1;
        cells.assign(size_t(w) * size_t(h), 0);
    }
    bool cut(QPointF q) const
    {
        const int i = int((q.x() - box.left()) / res), j = int((q.y() - box.top()) / res);
        if (i < 0 || j < 0 || i >= w || j >= h) return true;
        return cells[size_t(j) * w + i];
    }
    void stamp(QPointF c)
    {
        const int i0 = std::max(0, int((c.x() - r - box.left()) / res));
        const int i1 = std::min(w - 1, int((c.x() + r - box.left()) / res) + 1);
        const int j0 = std::max(0, int((c.y() - r - box.top()) / res));
        const int j1 = std::min(h - 1, int((c.y() + r - box.top()) / res) + 1);
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) {
                const double x = box.left() + (i + 0.5) * res - c.x();
                const double y = box.top() + (j + 0.5) * res - c.y();
                if (x * x + y * y <= r * r) cells[size_t(j) * w + i] = 1;
            }
    }
    // Front-half engagement in degrees, 2-degree samples.
    double engage(QPointF c, QPointF dir) const
    {
        const double a0 = std::atan2(dir.y(), dir.x());
        int n = 0;
        for (int k = -45; k <= 45; ++k) {
            const double a = a0 + k * M_PI / 90;
            if (!cut(c + QPointF(std::cos(a), std::sin(a)) * (r * 0.95))) ++n;
        }
        return n * 2.0;
    }
};

struct Result {
    double worst = 0;       // worst engagement over the passes
    double worstEntry = 0;  // at the first point of a pass (where it starts in the clear)
    bool inBounds = true;
    double uncleared = 0;   // reachable area left uncut, mm^2
    int cuts = 0;
};

// `allowed(c)`: may the tool centre be at c; `reach(q)`: can the tool reach q.
template <typename Allowed, typename Reach>
static Result replay(const adaptive::Plan &plan, const QRectF &box, double r,
                     Allowed allowed, Reach reach)
{
    Result res;
    Replay rp(box, r, 0.1);
    for (const adaptive::Move &m : plan.moves) {
        if (m.kind == adaptive::Move::Helix) {
            for (int k = 0; k <= 360; ++k) {
                const double a = k * M_PI / 180;
                rp.stamp(m.center + QPointF(std::cos(a), std::sin(a)) * m.radius);
            }
            res.inBounds = res.inBounds && allowed(m.center);
            continue;
        }
        ++res.cuts;
        const QPolygonF &p = m.path;
        for (int k = 0; k + 1 < p.size(); ++k) {
            const QLineF seg(p.at(k), p.at(k + 1));
            const int n = std::max(1, int(std::ceil(seg.length() / 0.1)));
            const QPointF dir = (p.at(k + 1) - p.at(k)) / std::max(1e-12, seg.length());
            for (int s = 0; s < n; ++s) {
                const QPointF c = seg.pointAt(double(s) / n);
                res.inBounds = res.inBounds && allowed(c);
                res.worst = std::max(res.worst, rp.engage(c, dir));
                rp.stamp(c);
            }
        }
        rp.stamp(p.last());
    }
    for (double y = box.top() + 0.13; y < box.bottom(); y += 0.25)
        for (double x = box.left() + 0.13; x < box.right(); x += 0.25)
            if (reach(QPointF(x, y)) && !rp.cut(QPointF(x, y)))
                res.uncleared += 0.0625;
    return res;
}

static QPolygonF rect(double x0, double y0, double x1, double y1)
{
    return QPolygonF({QPointF(x0, y0), QPointF(x1, y0), QPointF(x1, y1), QPointF(x0, y1),
                      QPointF(x0, y0)});
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // ---- a 60 x 40 rectangular pocket, 6.35 mm cutter, 10% stepover -------
    {
        const double r = 3.175;
        adaptive::Params p;
        p.toolR = r;
        p.stepover = 0.635;
        QElapsedTimer t;
        t.start();
        const adaptive::Plan plan = adaptive::plan({rect(0, 0, 60, 40)}, p);
        std::fprintf(stderr, "rect: %d moves, %lld ms, straight %.1f deg, limit %.1f, planned max %.1f\n",
                     int(plan.moves.size()), t.elapsed(), plan.straightEngageDeg, plan.limitDeg,
                     plan.maxEngageDeg);
        check(plan.available && !plan.moves.isEmpty() && plan.moves.first().kind == adaptive::Move::Helix,
              "plan starts with a helix");
        check(std::fabs(plan.moves.first().center.x() - 30) < 1 && std::fabs(plan.moves.first().center.y() - 20) < 1,
              "helix sits at the widest point");
        const Result res = replay(plan, QRectF(0, 0, 60, 40), r,
            [&](QPointF c) { return c.x() >= r - 0.02 && c.x() <= 60 - r + 0.02
                                    && c.y() >= r - 0.02 && c.y() <= 40 - r + 0.02; },
            [&](QPointF q) {   // reachable: not in the corners a round tool cannot reach
                const double cx = std::clamp(q.x(), r, 60 - r), cy = std::clamp(q.y(), r, 40 - r);
                return std::hypot(q.x() - cx, q.y() - cy) < r - 0.1;
            });
        int lifts = 0;
        double len = 0;
        for (const adaptive::Move &m : plan.moves)
            if (m.kind == adaptive::Move::Cut) {
                lifts += m.link == adaptive::Move::Retract;
                for (int k = 0; k + 1 < m.path.size(); ++k)
                    len += QLineF(m.path.at(k), m.path.at(k + 1)).length();
            }
        std::fprintf(stderr, "rect replay: worst %.1f deg over %d passes (%d retracts, %.0f mm), uncleared %.2f mm^2\n",
                     res.worst, res.cuts, lifts, len, res.uncleared);
        check(res.inBounds, "tool centre never leaves the allowed area");
        check(res.uncleared < 0.5, "everything reachable is cleared");
        check(res.worst <= plan.limitDeg + 6, "no pass buries more of the cutter than the limit");
    }

    // Distance helpers for the shapes below.
    auto segDist = [](QPointF q, QPointF a, QPointF b) {
        const QPointF d = b - a;
        const double L2 = d.x() * d.x() + d.y() * d.y();
        double t = L2 > 0 ? ((q.x() - a.x()) * d.x() + (q.y() - a.y()) * d.y()) / L2 : 0;
        t = std::clamp(t, 0.0, 1.0);
        return QLineF(q, a + d * t).length();
    };
    auto polyDist = [&](QPointF q, const QPolygonF &poly) {
        double best = 1e30;
        for (int i = 0; i + 1 < poly.size(); ++i)
            best = std::min(best, segDist(q, poly.at(i), poly.at(i + 1)));
        return best;
    };
    // Region = even-odd of the rings; centre allowed when inside and >= r
    // from every edge; a point reachable when some allowed centre within r
    // exists (tested on a grid of candidate centres).
    auto runShape = [&](const char *name, const QVector<QPolygonF> &rings, double r, double so,
                        double leave = 0) {
        QPainterPath region;
        region.setFillRule(Qt::OddEvenFill);
        QRectF box;
        for (const QPolygonF &ring : rings) {
            region.addPolygon(ring);
            box = box.isNull() ? ring.boundingRect() : box.united(ring.boundingRect());
        }
        auto edgeDist = [&](QPointF q) {
            double d = 1e30;
            for (const QPolygonF &ring : rings) d = std::min(d, polyDist(q, ring));
            return d;
        };
        auto allowed = [&](QPointF c) {
            return region.contains(c) && edgeDist(c) >= r + leave - 0.03;
        };
        // Reachability grid of allowed centres.
        std::vector<QPointF> centres;
        for (double y = box.top(); y <= box.bottom(); y += 0.25)
            for (double x = box.left(); x <= box.right(); x += 0.25)
                if (region.contains(QPointF(x, y)) && edgeDist(QPointF(x, y)) >= r + leave + 0.05)
                    centres.push_back(QPointF(x, y));
        auto reach = [&](QPointF q) {
            if (!region.contains(q) || edgeDist(q) < leave + 0.3) return false;
            for (const QPointF &c : centres)
                if (QLineF(q, c).length() < r - 0.3) return true;
            return false;
        };
        adaptive::Params p;
        p.toolR = r;
        p.stepover = so;
        p.leave = leave;
        QElapsedTimer t;
        t.start();
        const adaptive::Plan plan = adaptive::plan(rings, p);
        const qint64 ms = t.elapsed();
        const Result res = replay(plan, box, r, allowed, reach);
        int helices = 0, retracts = 0;
        for (const adaptive::Move &m : plan.moves) {
            helices += m.kind == adaptive::Move::Helix;
            retracts += m.kind == adaptive::Move::Cut && m.link == adaptive::Move::Retract;
        }
        std::fprintf(stderr, "%s: %d moves, %d helix, %d retracts, %lld ms, limit %.1f, replay worst %.1f, uncleared %.2f mm^2, unreached %d\n",
                     name, int(plan.moves.size()), helices, retracts, ms, plan.limitDeg, res.worst,
                     res.uncleared, plan.unreached);
        check(res.inBounds, "tool centre stays where it may go");
        check(res.uncleared < 1.0, "everything reachable is cleared");
        check(res.worst <= plan.limitDeg + 6, "engagement stays under the limit");
        return plan;
    };

    // L-shaped pocket: a reflex corner the front has to wrap around.
    runShape("L", {QPolygonF({QPointF(0, 0), QPointF(60, 0), QPointF(60, 20), QPointF(20, 20),
                              QPointF(20, 60), QPointF(0, 60), QPointF(0, 0)})}, 3.175, 0.635);
    // Pocket with an island: the front has to split around it and meet again.
    {
        QPolygonF island;
        for (int k = 0; k <= 48; ++k) {
            const double a = k * 2 * M_PI / 48;
            island << QPointF(40 + 8 * std::cos(a), 25 + 8 * std::sin(a));
        }
        const adaptive::Plan pl = runShape("island", {rect(0, 0, 80, 50), island}, 3.175, 0.635);
        check(pl.moves.first().kind == adaptive::Move::Helix, "island pocket starts on a helix");
    }
    // Two separate pockets: one helix each.
    {
        const adaptive::Plan pl = runShape("two", {rect(0, 0, 20, 20), rect(30, 0, 50, 15)}, 3.175, 0.635);
        int helices = 0;
        for (const adaptive::Move &m : pl.moves) helices += m.kind == adaptive::Move::Helix;
        check(helices == 2, "each separate pocket gets its own entry");
    }
    // A round pocket, a small tool, a heavier stepover, stock to leave.
    {
        QPolygonF disc;
        for (int k = 0; k <= 96; ++k) {
            const double a = k * 2 * M_PI / 96;
            disc << QPointF(25 + 25 * std::cos(a), 25 + 25 * std::sin(a));
        }
        runShape("disc", {disc}, 1.5875, 0.5, 0.3);
    }
    // A slot narrower than the tool: nothing to do, and said so.
    {
        adaptive::Params p;
        p.toolR = 3.175;
        const adaptive::Plan pl = adaptive::plan({rect(0, 0, 40, 5)}, p);
        check(pl.moves.isEmpty(), "a slot narrower than the tool gets no moves");
    }
    // Conventional milling reverses every pass.
    {
        adaptive::Params p;
        p.toolR = 3.175;
        p.stepover = 0.635;
        const adaptive::Plan climb = adaptive::plan({rect(0, 0, 30, 30)}, p);
        p.climb = false;
        const adaptive::Plan conv = adaptive::plan({rect(0, 0, 30, 30)}, p);
        auto turn = [](const adaptive::Plan &pl) {   // signed area swept by the passes
            double a = 0;
            for (const adaptive::Move &m : pl.moves)
                for (int i = 0; m.kind == adaptive::Move::Cut && i + 1 < m.path.size(); ++i)
                    a += m.path.at(i).x() * m.path.at(i + 1).y() - m.path.at(i + 1).x() * m.path.at(i).y();
            return a;
        };
        check(turn(climb) > 0 && turn(conv) < 0, "climb runs counter-clockwise, conventional clockwise");
    }

    std::printf("OK: %d checks passed\n", g_checks);
    return 0;
}
