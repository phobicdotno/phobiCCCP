#include "sketch.h"

#include <QLineF>
#include <QTransform>
#include <cmath>

namespace c2d {
namespace sketch {

static QPointF polar(QPointF c, double r, double a)
{
    return c + QPointF(r * std::cos(a), r * std::sin(a));
}

SubPath arc(QPointF c, double r, double a0, double sweep)
{
    SubPath s;
    const int pieces = qMax(1, int(std::ceil(std::fabs(sweep) / (M_PI / 2) - 1e-9)));
    const double step = sweep / pieces;
    // Handle length of a cubic that best matches an arc of `step`.
    const double h = 4.0 / 3.0 * std::tan(std::fabs(step) / 4) * r;
    const double dir = sweep < 0 ? -1.0 : 1.0;
    for (int i = 0; i <= pieces; ++i) {
        const double a = a0 + i * step;
        const QPointF p = polar(c, r, a);
        const QPointF tangent(-std::sin(a) * dir, std::cos(a) * dir);
        PathNode n;
        n.p = p;
        n.in = i > 0 ? p - tangent * h : p;
        n.out = i < pieces ? p + tangent * h : p;
        n.kind = (i > 0 && i < pieces) ? PathNode::Symmetric : PathNode::Corner;
        s.nodes.append(n);
    }
    return s;
}

// A full turn as a closed subpath (the duplicate end node folded into the first).
static SubPath fullCircle(QPointF c, double r)
{
    SubPath s = arc(c, r, 0, 2 * M_PI);
    s.nodes.first().in = s.nodes.last().in;
    s.nodes.first().kind = PathNode::Symmetric;
    s.nodes.removeLast();
    s.closed = true;
    return s;
}

PathModel ellipse(QPointF c, double rx, double ry, double rotDeg)
{
    PathModel m;
    if (rx <= 0 || ry <= 0)
        return m;
    SubPath s = fullCircle(QPointF(0, 0), 1.0);
    QTransform t;
    t.translate(c.x(), c.y());
    t.rotate(rotDeg);
    t.scale(rx, ry);
    for (PathNode &n : s.nodes) {
        n.p = t.map(n.p);
        n.in = t.map(n.in);
        n.out = t.map(n.out);
    }
    m.subs.append(s);
    return m;
}

// Append `more` to `s`, fusing its first node with s's last when they meet.
static void join(SubPath &s, const SubPath &more)
{
    for (int i = 0; i < more.nodes.size(); ++i) {
        const PathNode &n = more.nodes.at(i);
        if (i == 0 && !s.nodes.isEmpty() && QLineF(s.nodes.last().p, n.p).length() < 1e-9) {
            s.nodes.last().out = n.out;
            continue;
        }
        s.nodes.append(n);
    }
}

PathModel slot(QPointF c1, QPointF c2, double width)
{
    PathModel m;
    if (width <= 0)
        return m;
    const double r = width / 2;
    const QPointF d = c2 - c1;
    const double len = std::hypot(d.x(), d.y());
    if (len < 1e-9) {
        m.subs.append(fullCircle(c1, r));
        return m;
    }
    const double an = std::atan2(d.y(), d.x()) + M_PI / 2;   // the +normal side
    SubPath s;
    // Round c2's end from the +normal side to the -normal side, straight
    // back to c1, round its end, and close straight back along the +normal
    // side.
    join(s, arc(c2, r, an, -M_PI));
    join(s, arc(c1, r, an + M_PI, -M_PI));
    s.closed = true;
    m.subs.append(s);
    return m;
}

bool circleThrough(QPointF a, QPointF b, QPointF c, QPointF *center, double *radius)
{
    const double bx = b.x() - a.x(), by = b.y() - a.y();
    const double cx = c.x() - a.x(), cy = c.y() - a.y();
    const double d = 2 * (bx * cy - by * cx);
    const double scale = qMax(1e-12, (bx * bx + by * by) + (cx * cx + cy * cy));
    if (std::fabs(d) < 1e-9 * scale)
        return false;
    const double b2 = bx * bx + by * by, c2 = cx * cx + cy * cy;
    const QPointF o(a.x() + (cy * b2 - by * c2) / d, a.y() + (bx * c2 - cx * b2) / d);
    if (center)
        *center = o;
    if (radius)
        *radius = QLineF(o, a).length();
    return true;
}

PathModel arc3(QPointF start, QPointF end, QPointF through)
{
    PathModel m;
    QPointF o;
    double r = 0;
    if (QLineF(start, end).length() < 1e-9)
        return m;
    if (!circleThrough(start, through, end, &o, &r)) {
        SubPath s;
        PathNode a, b;
        a.p = a.in = a.out = start;
        b.p = b.in = b.out = end;
        s.nodes << a << b;
        m.subs.append(s);
        return m;
    }
    auto ang = [&o](QPointF p) { return std::atan2(p.y() - o.y(), p.x() - o.x()); };
    auto ccw = [](double from, double to) {   // counter-clockwise turn in [0, 2pi)
        double t = std::fmod(to - from, 2 * M_PI);
        return t < 0 ? t + 2 * M_PI : t;
    };
    const double a0 = ang(start);
    const double toEnd = ccw(a0, ang(end)), toMid = ccw(a0, ang(through));
    const double sweep = toMid < toEnd ? toEnd : toEnd - 2 * M_PI;
    SubPath s = arc(o, r, a0, sweep);
    s.nodes.first().p = start;   // exact ends, whatever the trig rounding
    s.nodes.last().p = end;
    m.subs.append(s);
    return m;
}

} // namespace sketch
} // namespace c2d
