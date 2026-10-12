// The sketch Create shapes (sketch.h): ellipse, slot, 3-point arc and the
// 3-point circle — exact end points, round arcs, and the edge cases.
//
// Plain asserts, no test framework; exits 0 on success.

#include "../src/element.h"
#include "../src/sketch.h"
#include "../src/vectorops.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <QLineF>
#include <QTransform>

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

static bool approx(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }
static bool nearPt(QPointF a, QPointF b, double eps = 1e-6) { return QLineF(a, b).length() < eps; }
static bool nearRect(const QRectF &a, const QRectF &b, double eps = 1e-6)
{
    return approx(a.left(), b.left(), eps) && approx(a.top(), b.top(), eps) &&
           approx(a.width(), b.width(), eps) && approx(a.height(), b.height(), eps);
}

// Qt flattens curves to about half a unit; measure at 100x.
static double area(const QPainterPath &p)
{
    return std::fabs(vec::ringArea(p.toFillPolygon(QTransform::fromScale(100, 100)))) / 1e4;
}

// Largest deviation from radius r about c over the path's length.
static double roundness(const QPainterPath &p, QPointF c, double r)
{
    double worst = 0;
    for (int i = 0; i <= 2000; ++i)
        worst = qMax(worst, std::fabs(QLineF(p.pointAtPercent(i / 2000.0), c).length() - r));
    return worst;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QJsonObject layer;
    layer.insert("name", QStringLiteral("DEFAULT"));

    // ---- arc ------------------------------------------------------------------
    {
        const SubPath q = sketch::arc({0, 0}, 10, 0, M_PI / 2);
        check(q.nodes.size() == 2 && nearPt(q.nodes[0].p, {10, 0}) && nearPt(q.nodes[1].p, {0, 10}),
              "quarter arc: one cubic, exact ends");
        PathModel m;
        m.subs.append(q);
        check(roundness(m.painterPath(), {0, 0}, 10) < 0.003, "quarter arc is round to 0.03%");
        const SubPath cw = sketch::arc({0, 0}, 10, 0, -3 * M_PI / 2);
        check(cw.nodes.size() == 4 && nearPt(cw.nodes.last().p, {0, 10}),
              "clockwise three-quarter arc: three cubics, ends at the top");
    }

    // ---- ellipse ----------------------------------------------------------------
    {
        const PathModel e = sketch::ellipse({50, 20}, 30, 10);
        check(e.subs.size() == 1 && e.subs[0].closed && e.subs[0].nodes.size() == 4,
              "ellipse: four nodes, closed");
        check(nearRect(e.painterPath().boundingRect(), QRectF(20, 10, 60, 20), 1e-6),
              "ellipse fills its bounding box");
        check(approx(area(e.painterPath()), M_PI * 30 * 10, 0.5), "ellipse area is pi a b");
        const PathModel t = sketch::ellipse({0, 0}, 10, 5, 90);
        check(nearRect(t.painterPath().boundingRect(), QRectF(-5, -10, 10, 20), 1e-6),
              "a turned ellipse turns its axes");
        check(sketch::ellipse({0, 0}, 0, 5).isEmpty(), "zero axis: nothing");
        const Element el = Element::makeBezierPath(e, layer);
        check(el.geometryType == "path" && vec::isClosed(el), "ellipse element is a closed path");
    }

    // ---- slot -------------------------------------------------------------------
    {
        const PathModel s = sketch::slot({0, 0}, {40, 0}, 10);
        check(s.subs.size() == 1 && s.subs[0].closed, "slot is one closed subpath");
        check(nearRect(s.painterPath().boundingRect(), QRectF(-5, -5, 50, 10), 1e-6),
              "slot spans center distance + width");
        check(approx(area(s.painterPath()), 40 * 10 + M_PI * 25, 0.3), "slot area: box + circle");
        // The far end is a half circle about the second center.
        double worst = 0;
        const QPainterPath p = s.painterPath();
        for (int i = 0; i <= 2000; ++i) {
            const QPointF q = p.pointAtPercent(i / 2000.0);
            if (q.x() > 40 + 1e-9)
                worst = qMax(worst, std::fabs(QLineF(q, {40, 0}).length() - 5));
        }
        check(worst < 0.002, "slot ends are round");
        const PathModel d = sketch::slot({0, 0}, {30, 30}, 4);
        check(nearRect(d.painterPath().boundingRect(), QRectF(-2, -2, 34, 34), 1e-6),
              "diagonal slot");
        const PathModel c = sketch::slot({5, 5}, {5, 5}, 6);
        check(approx(area(c.painterPath()), M_PI * 9, 0.05), "coincident centers: a circle");
        check(sketch::slot({0, 0}, {1, 0}, 0).isEmpty(), "zero width: nothing");
    }

    // ---- 3-point circle ---------------------------------------------------------
    {
        QPointF o;
        double r = 0;
        check(sketch::circleThrough({10, 0}, {0, 10}, {-10, 0}, &o, &r) && nearPt(o, {0, 0}) &&
              approx(r, 10), "circle through three points of r 10 about the origin");
        check(sketch::circleThrough({3, 7}, {9, 1}, {12, 12}, &o, &r) &&
              approx(QLineF(o, {3, 7}).length(), r) && approx(QLineF(o, {9, 1}).length(), r) &&
              approx(QLineF(o, {12, 12}).length(), r), "all three points on the circle");
        check(!sketch::circleThrough({0, 0}, {1, 1}, {2, 2}, &o, &r), "collinear: no circle");
    }

    // ---- 3-point arc ------------------------------------------------------------
    {
        // Upper half circle, start right, end left, through the top: CCW.
        PathModel a = sketch::arc3({10, 0}, {-10, 0}, {0, 10});
        QPainterPath p = a.painterPath();
        check(nearPt(p.pointAtPercent(0), {10, 0}) && nearPt(p.pointAtPercent(1), {-10, 0}),
              "arc runs start to end");
        check(nearPt(p.pointAtPercent(0.5), {0, 10}, 1e-3), "arc passes the middle point");
        check(roundness(p, {0, 0}, 10) < 0.003, "arc is round");
        // Same ends, through the bottom: clockwise.
        a = sketch::arc3({10, 0}, {-10, 0}, {0, -10});
        check(nearPt(a.painterPath().pointAtPercent(0.5), {0, -10}, 1e-3), "arc through the bottom");
        // A large arc (more than half a turn).
        a = sketch::arc3({10, 0}, {0, 10}, {-10, 0});
        p = a.painterPath();
        check(a.subs[0].nodes.size() == 4 &&
              nearRect(p.boundingRect(), QRectF(-10, -10, 20, 20), 1e-3),
              "three-quarter arc goes the long way round");
        // Collinear: a straight line.
        a = sketch::arc3({0, 0}, {10, 0}, {5, 0});
        check(a.subs[0].nodes.size() == 2 && !a.subs[0].nodes[0].hasOut(), "collinear: a line");
        check(sketch::arc3({1, 1}, {1, 1}, {2, 2}).isEmpty(), "zero chord: nothing");
        const Element el = Element::makeBezierPath(sketch::arc3({10, 0}, {-10, 0}, {0, 10}), layer);
        check(el.geometryType == "path" && !vec::isClosed(el), "arc element is an open path");
    }

    // ---- trim / break / extend --------------------------------------------------
    auto line = [&](QPointF a, QPointF b) {
        return Element::pathModel(Element::makePath({a, b}, false, layer));
    };
    {
        // A horizontal line crossed by two verticals at x = 30 and x = 70.
        const PathModel h = line({0, 50}, {100, 50});
        const QVector<PathModel> others = {line({30, 0}, {30, 100}), line({70, 0}, {70, 100})};
        const QVector<double> cuts = sketch::crossings(h, 0, others);
        check(cuts.size() == 2 && approx(cuts[0], 0.3) && approx(cuts[1], 0.7),
              "line crossings at 30% and 70%");

        bool hit = false;
        PathModel t = sketch::trim(h, {50, 50.5}, 1, others, &hit);
        check(hit && t.subs.size() == 2 && nearPt(t.subs[0].nodes.last().p, {30, 50}) &&
              nearPt(t.subs[1].nodes.first().p, {70, 50}) &&
              nearPt(t.subs[1].nodes.last().p, {100, 50}),
              "trim the middle: two pieces left, ending on the crossings");
        t = sketch::trim(h, {10, 50}, 1, others, &hit);
        check(t.subs.size() == 1 && nearPt(t.subs[0].nodes.first().p, {30, 50}),
              "trim an end piece back to the crossing");
        t = sketch::trim(h, {50, 80}, 1, others, &hit);
        check(!hit && t.subs.size() == 1, "a click away from the line trims nothing");
        t = sketch::trim(line({0, 0}, {10, 0}), {5, 0}, 1, {}, &hit);
        check(hit && t.isEmpty(), "a line nothing crosses is removed whole");

        const QVector<PathModel> parts = sketch::breakAt(h, {50, 50}, 1, others);
        check(parts.size() == 3 && parts[0].subs.size() == 1 &&
              nearPt(parts[0].subs[0].nodes.first().p, {30, 50}) &&
              nearPt(parts[0].subs[0].nodes.last().p, {70, 50}),
              "break: the clicked piece plus the two others");
        check(sketch::breakAt(line({0, 0}, {10, 0}), {5, 0}, 1, {}).isEmpty(),
              "nothing crosses: nothing to break");
    }
    {
        // A circle crossed by a line through its center: trim takes one half.
        const PathModel circ = Element::pathModel(Element::makeCircle({0, 0}, 10, layer));
        const QVector<PathModel> others = {line({-20, 0}, {20, 0})};
        check(sketch::crossings(circ, 0, others).size() == 2, "line crosses the circle twice");
        bool hit = false;
        const PathModel t = sketch::trim(circ, {0, 10}, 1, others, &hit);
        check(hit && t.subs.size() == 1 && !t.subs[0].closed, "trimmed circle is an open arc");
        const QRectF b = t.painterPath().boundingRect();
        check(nearRect(b, QRectF(-10, -10, 20, 10), 1e-6), "the lower half remains");
        check(roundness(t.painterPath(), {0, 0}, 10) < 0.003, "and it is still round");
        const PathModel whole = sketch::trim(circ, {0, 10}, 1, {}, &hit);
        check(whole.isEmpty(), "a loop nothing crosses goes whole");
        const QVector<PathModel> halves = sketch::breakAt(circ, {0, 10}, 1, others);
        check(halves.size() == 2 && nearRect(halves[0].painterPath().boundingRect(),
                                             QRectF(-10, 0, 20, 10), 1e-6),
              "break a circle: the clicked half and the other");
    }
    {
        // Two curves: crossing points sit on both (Newton-polished).
        const PathModel a = sketch::arc3({-10, 0}, {10, 0}, {0, 10});
        const PathModel c = Element::pathModel(Element::makeCircle({0, 10}, 8, layer));
        const QVector<double> cuts = sketch::crossings(a, 0, {c});
        check(cuts.size() == 2, "arc meets circle twice");
        for (double g : cuts) {
            const int seg = qMin(int(g), a.subs[0].segmentCount() - 1);
            const QPointF p = a.subs[0].pointAt(seg, g - seg);
            check(std::fabs(QLineF(p, {0, 10}).length() - 8) < 0.01 &&
                  std::fabs(QLineF(p, {0, 0}).length() - 10) < 0.01,
                  "crossing lies on both curves");
        }
    }
    {
        // Self-crossing: a bow tie's own crossing cuts it, its joints do not.
        const PathModel bow = Element::pathModel(
            Element::makePath({{0, 0}, {10, 10}, {10, 0}, {0, 10}}, false, layer));
        const QVector<double> cuts = sketch::crossings(bow, 0, {});
        check(cuts.size() == 2, "bow tie crosses itself once (seen from both strokes)");
    }
    {
        // Extend: a line stopping short of a wall reaches it; a curve gets a
        // straight continuation; nothing ahead, nothing happens.
        PathModel l = line({0, 0}, {10, 0});
        const QVector<PathModel> wall = {line({25, -5}, {25, 5})};
        check(sketch::extend(l, {9, 0}, 2, wall) && l.subs[0].nodes.size() == 2 &&
              nearPt(l.subs[0].nodes.last().p, {25, 0}), "line end grows to the wall");
        PathModel l2 = line({0, 0}, {10, 0});
        check(!sketch::extend(l2, {1, 0}, 2, wall), "the far end runs into nothing");
        PathModel l3 = line({30, 0}, {40, 0});
        check(sketch::extend(l3, {31, 0}, 2, wall) && nearPt(l3.subs[0].nodes.first().p, {25, 0}),
              "start end grows backwards to the wall");
        PathModel a = sketch::arc3({0, -10}, {10, 0}, {std::sqrt(50.0), -std::sqrt(50.0)});
        const QVector<PathModel> top = {line({-20, 20}, {20, 20})};
        const int nodes = a.subs[0].nodes.size();
        check(sketch::extend(a, {10, 0}, 2, top) && a.subs[0].nodes.size() == nodes + 1 &&
              nearPt(a.subs[0].nodes.last().p, {10, 20}, 1e-6),
              "a curved end continues straight along its tangent");
        PathModel closed = Element::pathModel(Element::makeCircle({0, 0}, 5, layer));
        check(!sketch::extend(closed, {5, 0}, 1, top), "closed paths have no ends");
    }

    // --- center-point arc: radius from the start, direction chosen ---------------
    {
        const PathModel ccw = sketch::arcCenter({0, 0}, {10, 0}, {0, 5}, true);
        check(ccw.subs.size() == 1, "center arc: one subpath");
        const SubPath &a = ccw.subs.first();
        check(nearPt(a.nodes.first().p, {10, 0}, 1e-9) && nearPt(a.nodes.last().p, {0, 10}, 1e-9),
              "center arc runs from the start to the end ray at the start's radius");
        check(roundness(ccw.painterPath(), {0, 0}, 10) < 0.003, "center arc is round");
        check(ccw.painterPath().boundingRect().width() < 10.01, "counter-clockwise: the quarter turn");
        const PathModel cw = sketch::arcCenter({0, 0}, {10, 0}, {0, 5}, false);
        check(cw.painterPath().boundingRect().width() > 19.9, "clockwise: the three-quarter turn");
        check(sketch::arcCenter({0, 0}, {0, 0}, {1, 1}, true).isEmpty(), "zero radius: nothing");
    }

    // --- tangent arc: leaves the end along its direction ---------------------------
    {
        const PathModel m = sketch::arcTangent({0, 0}, {1, 0}, {10, 10});
        const SubPath &a = m.subs.first();
        check(nearPt(a.nodes.first().p, {0, 0}, 1e-9) && nearPt(a.nodes.last().p, {10, 10}, 1e-9),
              "tangent arc ends where clicked");
        const QPointF h = a.nodes.first().out - a.nodes.first().p;
        check(h.x() > 0 && approx(h.y(), 0, 1e-9), "tangent arc leaves along the curve's direction");
        check(roundness(m.painterPath(), {0, 10}, 10) < 0.003, "quarter circle about (0, 10)");
        const PathModel ahead = sketch::arcTangent({0, 0}, {1, 0}, {7, 0});
        check(ahead.subs.first().nodes.size() == 2 && !ahead.subs.first().nodes.first().hasOut(),
              "straight ahead: a line");
        check(sketch::arcTangent({0, 0}, {1, 0}, {-5, 0}).isEmpty(), "straight back: nothing");
    }

    // --- slot modes ----------------------------------------------------------------
    {
        QRectF b = sketch::slotOverall({0, 0}, {50, 0}, 10).painterPath().boundingRect();
        check(nearRect(b, QRectF(0, -5, 50, 10), 1e-3), "overall slot: the clicks are its ends");
        b = sketch::slotCenterPoint({25, 0}, {45, 0}, 10).painterPath().boundingRect();
        check(nearRect(b, QRectF(0, -5, 50, 10), 1e-3), "center point slot: mirrored about the middle");
        b = sketch::slotOverall({0, 0}, {6, 0}, 10).painterPath().boundingRect();
        check(nearRect(b, QRectF(-2, -5, 10, 10), 1e-3), "overall slot shorter than wide: a circle");
        // Arc slot round a half circle of radius 20, 4 mm wide: half an annulus
        // plus two end caps (a whole 2 mm circle).
        const PathModel as = sketch::arcSlot({20, 0}, {-20, 0}, {0, 20}, 4);
        check(as.subs.size() == 1 && as.subs.first().closed, "arc slot is one closed outline");
        const double want = M_PI * (22 * 22 - 18 * 18) / 2 + M_PI * 2 * 2;
        check(approx(area(as.painterPath()), want, want * 0.002), "arc slot area");
        b = as.painterPath().boundingRect();
        check(nearRect(b, QRectF(-22, -2, 44, 24), 2e-3), "arc slot bounds");
        check(sketch::arcSlot({20, 0}, {-20, 0}, {0, 20}, 41).isEmpty(), "arc slot wider than its radius: nothing");
    }

    // --- edge polygon --------------------------------------------------------------
    {
        QPointF c;
        double r = 0, rot = 0;
        const QVector<QPointF> v = sketch::polygonEdge({0, 0}, {10, 0}, {5, 3}, 6, &c, &r, &rot);
        check(v.size() == 6 && approx(r, 10, 1e-9), "hexagon on a 10 mm edge has radius 10");
        check(nearPt(c, {5, 10 * std::sqrt(3.0) / 2}, 1e-9), "edge polygon sits on the clicked side");
        bool hasP2 = false;
        for (const QPointF &p : v)
            hasP2 = hasP2 || nearPt(p, {10, 0}, 1e-9);
        check(nearPt(v.first(), {0, 0}, 1e-9) && hasP2, "the edge's ends are corners");
        const Element e = Element::makePolygon(c, r, 6, layer, rot);
        check(nearRect(e.painterPath.boundingRect(), QRectF(-5, 0, 20, 10 * std::sqrt(3.0)), 1e-6),
              "makePolygon reproduces it");
        sketch::polygonEdge({0, 0}, {10, 0}, {5, -3}, 4, &c, &r, &rot);
        check(nearPt(c, {5, -5}, 1e-9), "the other side");
    }

    // --- fit-point spline ----------------------------------------------------------
    {
        const QVector<QPointF> pts{{0, 0}, {10, 10}, {20, 0}, {30, 10}};
        const PathModel open = sketch::fitSpline(pts, false);
        check(open.subs.size() == 1 && !open.subs.first().closed && open.subs.first().nodes.size() == 4,
              "open spline: a node per point");
        for (int i = 0; i < pts.size(); ++i)
            check(nearPt(open.subs.first().nodes.at(i).p, pts.at(i), 1e-12), "spline passes through each point");
        const PathNode &mid = open.subs.first().nodes.at(1);
        const QPointF di = mid.p - mid.in, dout = mid.out - mid.p;
        check(std::fabs(di.x() * dout.y() - di.y() * dout.x()) < 1e-9 && di.x() * dout.x() + di.y() * dout.y() > 0,
              "smooth through the inner points");
        const PathModel closed = sketch::fitSpline({{0, 0}, {10, 0}, {10, 10}, {0, 10}}, true);
        check(closed.subs.first().closed && closed.subs.first().nodes.size() == 4, "closed spline wraps");
        check(area(closed.painterPath()) > 100, "closed spline bulges out round its points");
        check(sketch::fitSpline({{1, 1}}, false).isEmpty(), "one point: nothing");
        check(sketch::fitSpline({{0, 0}, {5, 0}}, false).subs.first().nodes.size() == 2, "two points: a line");
    }

    // --- conic ------------------------------------------------------------------------
    {
        const PathModel par = sketch::conic({0, 0}, {40, 0}, {20, 30}, 0.5);
        check(par.subs.size() == 1 && !par.subs.first().closed, "conic: one open subpath");
        const SubPath &s = par.subs.first();
        check(nearPt(s.nodes.first().p, {0, 0}, 1e-12) && nearPt(s.nodes.last().p, {40, 0}, 1e-12),
              "conic: runs start to end");
        const QPointF t0 = s.nodes.at(0).out - s.nodes.at(0).p;
        const QPointF t1 = s.nodes.last().p - s.nodes.last().in;
        check(std::fabs(t0.x() * 30 - t0.y() * 20) < 1e-9 && t0.x() > 0
                  && std::fabs(t1.x() * -30 - t1.y() * 20) < 1e-9 && t1.x() > 0,
              "conic: tangent to the apex lines at both ends");
        check(nearPt(s.nodes.at(s.nodes.size() / 2).p, {20, 15}, 1e-9),
              "conic: rho 0.5 is a parabola, its shoulder halfway to the apex");
        const double fuller = sketch::conic({0, 0}, {40, 0}, {20, 30}, 0.8).painterPath().boundingRect().height();
        const double flatter = sketch::conic({0, 0}, {40, 0}, {20, 30}, 0.2).painterPath().boundingRect().height();
        check(fuller > 20 && flatter < 10, "conic: rho sets the fullness");
        // A quarter circle is the conic with rho = w / (1 + w), w = cos 45.
        const double w = std::sqrt(0.5);
        const PathModel q = sketch::conic({10, 0}, {0, 10}, {10, 10}, w / (1 + w));
        double worst = 0;
        for (int seg = 0; seg < q.subs.first().segmentCount(); ++seg)
            for (double t : {0.25, 0.5, 0.75}) {
                const QPointF p = q.subs.first().pointAt(seg, t);
                worst = std::max(worst, std::fabs(std::hypot(p.x(), p.y()) - 10));
            }
        check(worst < 1e-3, "conic: the cubic pieces follow the true curve");
        const PathModel line = sketch::conic({0, 0}, {10, 0}, {5, 0}, 0.5);
        check(line.subs.size() == 1 && line.subs.first().nodes.size() == 2, "conic: collinear apex gives a line");
    }

    std::printf("OK: %d checks passed\n", g_checks);
    return 0;
}
