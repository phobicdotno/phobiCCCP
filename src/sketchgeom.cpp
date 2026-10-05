#include "sketchgeom.h"

#include <QtMath>
#include <cmath>
#include <limits>

namespace c2d {
namespace sketch {

namespace {

// Line in normal form n·p = d with |n| = 1.
struct NLine {
    QPointF n;
    double d = 0;
    bool ok = false;
};

NLine normalForm(const QLineF &l)
{
    NLine r;
    const double len = l.length();
    if (len < 1e-12)
        return r;
    r.n = QPointF(-l.dy() / len, l.dx() / len);
    r.d = r.n.x() * l.x1() + r.n.y() * l.y1();
    r.ok = true;
    return r;
}

double dot(const QPointF &a, const QPointF &b) { return a.x() * b.x() + a.y() * b.y(); }

} // namespace

Circle circleThrough3(const QPointF &a, const QPointF &b, const QPointF &c)
{
    Circle r;
    const double d = 2.0 * (a.x() * (b.y() - c.y()) + b.x() * (c.y() - a.y())
                            + c.x() * (a.y() - b.y()));
    if (std::abs(d) < 1e-9)
        return r;
    const double a2 = dot(a, a), b2 = dot(b, b), c2 = dot(c, c);
    r.center = QPointF((a2 * (b.y() - c.y()) + b2 * (c.y() - a.y()) + c2 * (a.y() - b.y())) / d,
                       (a2 * (c.x() - b.x()) + b2 * (a.x() - c.x()) + c2 * (b.x() - a.x())) / d);
    r.radius = QLineF(r.center, a).length();
    r.valid = r.radius > 1e-9;
    return r;
}

Circle circleTangent2(const QLineF &l1, const QLineF &l2, const QPointF &cursor)
{
    Circle r;
    const NLine a = normalForm(l1), b = normalForm(l2);
    if (!a.ok || !b.ok)
        return r;
    const double cross = a.n.x() * b.n.y() - a.n.y() * b.n.x();
    if (std::abs(cross) < 1e-9) {
        // Parallel: diameter is the gap, centre slides along the midline.
        const double gap = std::abs(a.d - dot(a.n, b.n) * b.d);
        const double mid = (a.d + dot(a.n, b.n) * b.d) / 2.0;
        r.center = cursor - a.n * (dot(a.n, cursor) - mid);
        r.radius = gap / 2.0;
        r.valid = r.radius > 1e-9;
        return r;
    }
    // Intersection of the two lines.
    const QPointF x((a.d * b.n.y() - b.d * a.n.y()) / cross,
                    (a.n.x() * b.d - b.n.x() * a.d) / cross);
    // The two bisector directions; pick the one the cursor lies along.
    QPointF best;
    double bestT = 0, bestDist = std::numeric_limits<double>::max();
    const QPointF dirA(-a.n.y(), a.n.x()), dirB(-b.n.y(), b.n.x());
    for (int s : {1, -1}) {
        QPointF bis = dirA + dirB * s;
        const double len = std::hypot(bis.x(), bis.y());
        if (len < 1e-12)
            continue;
        bis /= len;
        const double t = dot(cursor - x, bis);
        const QPointF foot = x + bis * t;
        const double dist = QLineF(foot, cursor).length();
        if (dist < bestDist) {
            bestDist = dist;
            best = foot;
            bestT = t;
        }
    }
    r.center = best;
    r.radius = std::abs(dot(a.n, best) - a.d);
    r.valid = r.radius > 1e-9 && std::abs(bestT) > 1e-9;
    return r;
}

Circle circleTangent3(const QLineF &l1, const QLineF &l2, const QLineF &l3,
                      const QPointF &cursor)
{
    Circle best;
    const NLine L[3] = {normalForm(l1), normalForm(l2), normalForm(l3)};
    if (!L[0].ok || !L[1].ok || !L[2].ok)
        return best;
    double bestDist = std::numeric_limits<double>::max();
    // Signed distance s_i * (n_i·c - d_i) = r for each line; every sign
    // combination is one linear system in (cx, cy, r).
    for (int mask = 0; mask < 8; ++mask) {
        double m[3][4];
        for (int i = 0; i < 3; ++i) {
            const double s = (mask >> i) & 1 ? -1.0 : 1.0;
            m[i][0] = s * L[i].n.x();
            m[i][1] = s * L[i].n.y();
            m[i][2] = -1.0;
            m[i][3] = s * L[i].d;
        }
        // Gaussian elimination with partial pivoting.
        bool singular = false;
        for (int col = 0; col < 3 && !singular; ++col) {
            int piv = col;
            for (int row = col + 1; row < 3; ++row)
                if (std::abs(m[row][col]) > std::abs(m[piv][col]))
                    piv = row;
            if (std::abs(m[piv][col]) < 1e-12) {
                singular = true;
                break;
            }
            for (int k = 0; k < 4; ++k)
                std::swap(m[col][k], m[piv][k]);
            for (int row = 0; row < 3; ++row) {
                if (row == col)
                    continue;
                const double f = m[row][col] / m[col][col];
                for (int k = col; k < 4; ++k)
                    m[row][k] -= f * m[col][k];
            }
        }
        if (singular)
            continue;
        const QPointF c(m[0][3] / m[0][0], m[1][3] / m[1][1]);
        const double rad = m[2][3] / m[2][2];
        if (rad <= 1e-9)
            continue;
        const double dist = QLineF(c, cursor).length();
        if (dist < bestDist) {
            bestDist = dist;
            best.center = c;
            best.radius = rad;
            best.valid = true;
        }
    }
    return best;
}

QVector<QPointF> rect3Point(const QPointF &p1, const QPointF &p2, const QPointF &p3)
{
    const QLineF edge(p1, p2);
    const double len = edge.length();
    if (len < 1e-12)
        return {};
    const QPointF n(-edge.dy() / len, edge.dx() / len);
    const double h = dot(p3 - p1, n);
    const QPointF off = n * h;
    return {p1, p2, p2 + off, p1 + off};
}

bool isAxisAligned(const QVector<QPointF> &c, double tol)
{
    if (c.size() != 4)
        return false;
    for (int i = 0; i < 4; ++i) {
        const QPointF a = c.at(i), b = c.at((i + 1) % 4);
        if (std::abs(a.x() - b.x()) > tol && std::abs(a.y() - b.y()) > tol)
            return false;
    }
    return true;
}

} // namespace sketch
} // namespace c2d
