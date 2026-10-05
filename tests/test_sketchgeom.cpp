// Headless checks for the Fusion-style circle and rectangle construction math.
#include "../src/sketchgeom.h"

#include <QCoreApplication>
#include <cmath>
#include <cstdio>

using namespace c2d::sketch;

static int g_fail = 0;
static void check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}
static bool near(double a, double b, double tol = 1e-6) { return std::abs(a - b) < tol; }
static bool near(const QPointF &a, const QPointF &b, double tol = 1e-6)
{
    return near(a.x(), b.x(), tol) && near(a.y(), b.y(), tol);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // 3-point: three points on the circle centred (10,20) radius 5.
    Circle c = circleThrough3({15, 20}, {10, 25}, {5, 20});
    check(c.valid && near(c.center, {10, 20}) && near(c.radius, 5), "3-point circumcircle");
    check(!circleThrough3({0, 0}, {1, 1}, {2, 2}).valid, "3-point collinear rejected");

    // 2-tangent: the x and y axes, cursor in the first quadrant on y = x.
    c = circleTangent2(QLineF(0, 0, 10, 0), QLineF(0, 0, 0, 10), {4, 4});
    check(c.valid && near(c.center, {4, 4}) && near(c.radius, 4), "2-tangent in a corner");
    c = circleTangent2(QLineF(0, 0, 10, 0), QLineF(0, 0, 0, 10), {-3, 3});
    check(c.valid && near(c.center, {-3, 3}) && near(c.radius, 3), "2-tangent other quadrant");
    c = circleTangent2(QLineF(0, 0, 10, 0), QLineF(0, 6, 10, 6), {5, 1});
    check(c.valid && near(c.center, {5, 3}) && near(c.radius, 3), "2-tangent parallel lines");

    // 3-tangent: right triangle (0,0) (12,0) (0,9) — incircle r = (a+b-c)/2 = 3.
    const QLineF a(0, 0, 12, 0), b(0, 0, 0, 9), h(12, 0, 0, 9);
    c = circleTangent3(a, b, h, {3, 3});
    check(c.valid && near(c.center, {3, 3}) && near(c.radius, 3), "3-tangent incircle");
    // Excircle beyond the hypotenuse: r = area / (s - 15) = 54 / 3 = 18.
    c = circleTangent3(a, b, h, {16, 16});
    check(c.valid && near(c.center, {18, 18}) && near(c.radius, 18),
          "3-tangent picks the excircle near the cursor");

    // 3-point rectangle: tilted edge, height from the third point.
    QVector<QPointF> r = rect3Point({0, 0}, {3, 4}, {-4, 3});
    check(r.size() == 4 && near(r[2], {-1, 7}) && near(r[3], {-4, 3}), "3-point tilted rectangle");
    check(!isAxisAligned(r), "tilted rectangle is not axis-aligned");
    r = rect3Point({0, 0}, {10, 0}, {2, 5});
    check(r.size() == 4 && near(r[2], {10, 5}) && isAxisAligned(r), "3-point axis-aligned rectangle");
    return g_fail ? 1 : 0;
}
