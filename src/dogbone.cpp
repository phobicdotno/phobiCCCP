#include "dogbone.h"

#include <QPolygonF>
#include <QtMath>
#include <algorithm>
#include <cmath>

#ifdef HAVE_CLIPPER2
#include <clipper2/clipper.h>
namespace C2 = Clipper2Lib;
#endif

namespace c2d {

CornerStyle cornerStyleOf(const QJsonValue &v)
{
    const QString s = v.toString().trimmed().toLower();
    if (s == QLatin1String("dogbone"))
        return CornerStyle::Dogbone;
    if (s == QLatin1String("tbone") || s == QLatin1String("t-bone"))
        return CornerStyle::TBone;
    return CornerStyle::Sharp;
}

#ifdef HAVE_CLIPPER2
namespace {

const double kScale = 1000.0;   // integer µm, as gcodeexport
const double kSlack = 0.05;     // mm a bite is wider than the bit, each side

C2::Path64 toPath64(const QPolygonF &poly)
{
    C2::Path64 p;
    const int n = poly.size() > 1 && poly.first() == poly.last() ? poly.size() - 1 : poly.size();
    p.reserve(size_t(qMax(0, n)));
    for (int i = 0; i < n; ++i)
        p.push_back(C2::Point64(qRound64(poly.at(i).x() * kScale), qRound64(poly.at(i).y() * kScale)));
    return p;
}

// The region as Clipper rings with the region on the left of travel:
// outlines positive (counter-clockwise), holes negative.
C2::Paths64 regionRings(const QPainterPath &region)
{
    C2::Paths64 ps;
    for (const QPolygonF &poly : region.toSubpathPolygons())
        if (poly.size() > 2)
            ps.push_back(toPath64(poly));
    return C2::Union(ps, C2::FillRule::EvenOdd);
}

// A circle polygon with `through` as an exact vertex, so the bite really
// reaches the corner; chords stay within 0.01 mm of the arc.
C2::Path64 circleThrough(QPointF c, double r, QPointF through)
{
    const double step = 0.01;
    const double a0 = std::atan2(through.y() - c.y(), through.x() - c.x());
    const int n = qMax(24, int(std::ceil(M_PI / std::acos(qMax(-1.0, 1.0 - step / r)))));
    C2::Path64 p;
    p.reserve(size_t(n));
    p.push_back(C2::Point64(qRound64(through.x() * kScale), qRound64(through.y() * kScale)));
    for (int i = 1; i < n; ++i) {
        const double a = a0 + 2 * M_PI * i / n;
        p.push_back(C2::Point64(qRound64((c.x() + r * std::cos(a)) * kScale),
                                qRound64((c.y() + r * std::sin(a)) * kScale)));
    }
    return p;
}

QList<CornerBite> bitesOf(const C2::Paths64 &rings, bool materialInside, double r,
                          CornerStyle style, double minDeg, double maxDeg)
{
    QList<CornerBite> out;
    if (style == CornerStyle::Sharp || !(r > 0))
        return out;
    for (const C2::Path64 &ring : rings) {
        const int n = int(ring.size());
        if (n < 3)
            continue;
        for (int i = 0; i < n; ++i) {
            const C2::Point64 &P = ring[size_t((i + n - 1) % n)], &V = ring[size_t(i)],
                              &Q = ring[size_t((i + 1) % n)];
            const QPointF p(P.x / kScale, P.y / kScale), v(V.x / kScale, V.y / kScale),
                q(Q.x / kScale, Q.y / kScale);
            const QPointF e1 = v - p, e2 = q - v;
            const double l1 = std::hypot(e1.x(), e1.y()), l2 = std::hypot(e2.x(), e2.y());
            if (l1 < 1e-9 || l2 < 1e-9)
                continue;
            const QPointF d1 = e1 / l1, d2 = e2 / l2;
            const double cross = d1.x() * d2.y() - d1.y() * d2.x();
            // The region is on the left. An inside corner turns towards the
            // empty side: right when the region is the part, left when it
            // is the hole.
            if (materialInside ? cross > -1e-9 : cross < 1e-9)
                continue;
            const double dot = qBound(-1.0, d1.x() * d2.x() + d1.y() * d2.y(), 1.0);
            const double angle = 180.0 - qRadiansToDegrees(std::acos(dot));   // opening angle
            if (angle < minDeg - 1e-6 || angle > maxDeg + 1e-6)
                continue;
            QPointF c;
            if (style == CornerStyle::TBone) {
                c = l1 >= l2 ? v - r * d1 : v + r * d2;
            } else {
                const QPointF b = d2 - d1;   // bisector, into the empty side
                c = v + r * b / std::hypot(b.x(), b.y());
            }
            out.append({v, c});
        }
    }
    return out;
}

} // namespace

QList<CornerBite> cornerBites(const QPainterPath &region, bool materialInside, double toolD,
                              CornerStyle style, double minDeg, double maxDeg)
{
    if (style == CornerStyle::Sharp || !(toolD > 0))
        return {};
    return bitesOf(regionRings(region), materialInside, toolD / 2, style, minDeg, maxDeg);
}

QPainterPath withCornerRelief(const QPainterPath &region, bool materialInside, double toolD,
                              CornerStyle style, double minDeg, double maxDeg, int *count)
{
    if (count)
        *count = 0;
    if (style == CornerStyle::Sharp || !(toolD > 0) || !std::isfinite(toolD))
        return region;
    // The bites are a hair wider than the bit: at exactly its radius the
    // tool-centre offset meets the bite centre only in a cusp, which the
    // offset rounds off 0.15 mm short of the corner.
    const double r = toolD / 2 + kSlack;
    const C2::Paths64 rings = regionRings(region);
    const QList<CornerBite> bites = bitesOf(rings, materialInside, r, style, minDeg, maxDeg);
    if (bites.isEmpty())
        return region;
    if (count)
        *count = int(bites.size());
    C2::Paths64 circles;
    for (const CornerBite &b : bites)
        circles.push_back(circleThrough(b.centre, r, b.corner));
    const C2::Paths64 res = materialInside
        ? C2::Difference(rings, circles, C2::FillRule::NonZero)
        : C2::Union(rings, circles, C2::FillRule::NonZero);
    QPainterPath out;
    out.setFillRule(Qt::OddEvenFill);
    for (const C2::Path64 &p : res) {
        if (p.size() < 3)
            continue;
        QPolygonF poly;
        poly.reserve(int(p.size()) + 1);
        for (const C2::Point64 &pt : p)
            poly.append(QPointF(pt.x / kScale, pt.y / kScale));
        poly.append(poly.first());
        out.addPolygon(poly);
    }
    return out;
}

#else // !HAVE_CLIPPER2: no exact booleans; corners stay sharp.

QList<CornerBite> cornerBites(const QPainterPath &, bool, double, CornerStyle, double, double)
{
    return {};
}

QPainterPath withCornerRelief(const QPainterPath &region, bool, double, CornerStyle, double, double,
                              int *count)
{
    if (count)
        *count = 0;
    return region;
}

#endif

} // namespace c2d
