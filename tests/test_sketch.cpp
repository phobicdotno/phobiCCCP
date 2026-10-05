// The sketch Create shapes (sketch.h): ellipse, slot, 3-point arc and the
// 3-point circle — exact end points, round arcs, and the edge cases.
//
// Plain asserts, no test framework; exits 0 on success.

#include "../src/element.h"
#include "../src/sketch.h"
#include "../src/vectorops.h"

#include <QGuiApplication>
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
    QGuiApplication app(argc, argv);
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

    std::printf("OK: %d checks passed\n", g_checks);
    return 0;
}
