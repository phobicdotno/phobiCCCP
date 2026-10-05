#include "sketch.h"

#include <QLineF>
#include <QRectF>
#include <QTransform>
#include <algorithm>
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

// ---- trim / break / extend ---------------------------------------------------

namespace {

// A segment as a cubic. Straight segments get their control points at the
// thirds, so the parameter runs evenly and Newton steps have a derivative
// at the ends.
struct Cubic {
    QPointF p[4];
    bool line = false;
};

Cubic cubicOf(const SubPath &s, int seg)
{
    const PathNode *a, *b;
    s.segment(seg, &a, &b);
    Cubic c;
    c.line = !a->hasOut() && !b->hasIn();
    if (c.line) {
        const QPointF d = b->p - a->p;
        c.p[0] = a->p; c.p[1] = a->p + d / 3; c.p[2] = a->p + 2 * d / 3; c.p[3] = b->p;
    } else {
        c.p[0] = a->p; c.p[1] = a->out; c.p[2] = b->in; c.p[3] = b->p;
    }
    return c;
}

QPointF at(const Cubic &c, double t)
{
    const double u = 1 - t;
    return c.p[0] * (u * u * u) + c.p[1] * (3 * u * u * t) + c.p[2] * (3 * u * t * t)
         + c.p[3] * (t * t * t);
}

QPointF deriv(const Cubic &c, double t)
{
    const double u = 1 - t;
    return (c.p[1] - c.p[0]) * (3 * u * u) + (c.p[2] - c.p[1]) * (6 * u * t)
         + (c.p[3] - c.p[2]) * (3 * t * t);
}

void split(const Cubic &c, double t, Cubic *left, Cubic *right)
{
    auto L = [t](QPointF a, QPointF b) { return a + (b - a) * t; };
    const QPointF a = L(c.p[0], c.p[1]), b = L(c.p[1], c.p[2]), d = L(c.p[2], c.p[3]);
    const QPointF ab = L(a, b), bd = L(b, d), m = L(ab, bd);
    if (left) { left->p[0] = c.p[0]; left->p[1] = a; left->p[2] = ab; left->p[3] = m; left->line = c.line; }
    if (right) { right->p[0] = m; right->p[1] = bd; right->p[2] = d; right->p[3] = c.p[3]; right->line = c.line; }
}

Cubic portion(const Cubic &c, double t0, double t1)
{
    Cubic left = c, out;
    if (t1 < 1 - 1e-15)
        split(c, t1, &left, nullptr);
    if (t0 <= 1e-15 || t1 <= 1e-15)
        return left;
    split(left, t0 / t1, nullptr, &out);
    return out;
}

// A subpath flattened for crossing tests: per segment its sample points
// (with their global parameters) and bounding box.
struct Flat {
    const SubPath *sub = nullptr;
    QVector<QVector<QPointF>> pts;
    QVector<QVector<double>> g;
    QVector<QRectF> box;
};

Flat flatten(const SubPath &s)
{
    Flat f;
    f.sub = &s;
    for (int i = 0; i < s.segmentCount(); ++i) {
        const Cubic c = cubicOf(s, i);
        const int n = c.line ? 1 : 32;
        QVector<QPointF> pts;
        QVector<double> gs;
        for (int k = 0; k <= n; ++k) {
            pts.append(at(c, double(k) / n));
            gs.append(i + double(k) / n);
        }
        // By hand: QRectF::united skips zero-size rectangles, which is
        // every straight horizontal or vertical segment's box.
        double x0 = pts.first().x(), x1 = x0, y0 = pts.first().y(), y1 = y0;
        for (const QPointF &p : pts) {
            x0 = qMin(x0, p.x()); x1 = qMax(x1, p.x());
            y0 = qMin(y0, p.y()); y1 = qMax(y1, p.y());
        }
        f.box.append(QRectF(QPointF(x0 - 1e-6, y0 - 1e-6), QPointF(x1 + 1e-6, y1 + 1e-6)));
        f.pts.append(pts);
        f.g.append(gs);
    }
    return f;
}

bool boxesMeet(const QRectF &a, const QRectF &b)
{
    return a.left() <= b.right() && b.left() <= a.right() && a.top() <= b.bottom()
        && b.top() <= a.bottom();
}

// Segment a0-a1 against b0-b1: fractions along each, when they cross.
bool edgeCross(QPointF a0, QPointF a1, QPointF b0, QPointF b1, double *s, double *u)
{
    const QPointF r = a1 - a0, q = b1 - b0, w = b0 - a0;
    const double den = r.x() * q.y() - r.y() * q.x();
    const double scale = std::hypot(r.x(), r.y()) * std::hypot(q.x(), q.y());
    if (std::fabs(den) <= 1e-12 * qMax(scale, 1e-300))
        return false;   // parallel (overlaps are not crossings)
    *s = (w.x() * q.y() - w.y() * q.x()) / den;
    *u = (w.x() * r.y() - w.y() * r.x()) / den;
    const double e = 1e-9;
    return *s >= -e && *s <= 1 + e && *u >= -e && *u <= 1 + e;
}

// Polish a crossing of cubics A and B near (t, u) by Newton steps on
// A(t) - B(u) = 0; keeps the estimate when the steps wander off.
void polish(const Cubic &A, const Cubic &B, double *t, double *u)
{
    if (A.line && B.line)
        return;   // the polyline crossing is already exact
    double tt = *t, uu = *u;
    for (int it = 0; it < 12; ++it) {
        const QPointF f = at(A, tt) - at(B, uu);
        const QPointF da = deriv(A, tt), db = deriv(B, uu);
        // [da, -db] [dt du]^T = -f
        const double det = da.x() * -db.y() - da.y() * -db.x();
        if (std::fabs(det) < 1e-18)
            return;
        const double dt = (-f.x() * -db.y() - -f.y() * -db.x()) / det;
        const double du = (da.x() * -f.y() - da.y() * -f.x()) / det;
        tt += dt;
        uu += du;
        if (tt < -0.05 || tt > 1.05 || uu < -0.05 || uu > 1.05)
            return;
        if (std::fabs(dt) < 1e-13 && std::fabs(du) < 1e-13)
            break;
    }
    if (QLineF(at(A, tt), at(B, uu)).length() < QLineF(at(A, *t), at(B, *u)).length()) {
        *t = qBound(0.0, tt, 1.0);
        *u = qBound(0.0, uu, 1.0);
    }
}

// Crossings of flattened subpath A with B, as A's global parameters (and
// B's, for the self-crossing filter).
void crossFlats(const Flat &A, const Flat &B, QVector<QPair<double, double>> *out)
{
    for (int i = 0; i < A.pts.size(); ++i) {
        for (int j = 0; j < B.pts.size(); ++j) {
            if (A.sub == B.sub && i == j)
                continue;   // a cubic's crossing with itself: not a cut
            if (!boxesMeet(A.box.at(i), B.box.at(j)))
                continue;
            const QVector<QPointF> &pa = A.pts.at(i), &pb = B.pts.at(j);
            for (int k = 0; k + 1 < pa.size(); ++k) {
                for (int l = 0; l + 1 < pb.size(); ++l) {
                    double s, u;
                    if (!edgeCross(pa.at(k), pa.at(k + 1), pb.at(l), pb.at(l + 1), &s, &u))
                        continue;
                    const double ga = A.g.at(i).at(k) + s * (A.g.at(i).at(k + 1) - A.g.at(i).at(k));
                    const double gb = B.g.at(j).at(l) + u * (B.g.at(j).at(l + 1) - B.g.at(j).at(l));
                    double t = ga - i, v = gb - j;
                    polish(cubicOf(*A.sub, i), cubicOf(*B.sub, j), &t, &v);
                    out->append({i + t, j + v});
                }
            }
        }
    }
}

double wrapDist(double a, double b, double n, bool closed)
{
    const double d = std::fabs(a - b);
    return closed ? qMin(d, std::fabs(n - d)) : d;
}

} // namespace

bool pick(const PathModel &m, QPointF q, double tol, int *sub, double *g)
{
    double best = tol;
    bool found = false;
    for (int i = 0; i < m.subs.size(); ++i) {
        int seg;
        double t;
        if (m.subs.at(i).segmentCount() < 1)
            continue;
        const double d = m.subs.at(i).closest(q, &seg, &t);
        if (seg >= 0 && d <= best) {
            best = d;
            *sub = i;
            *g = seg + t;
            found = true;
        }
    }
    return found;
}

QVector<double> crossings(const PathModel &m, int sub, const QVector<PathModel> &others)
{
    QVector<double> out;
    if (sub < 0 || sub >= m.subs.size())
        return out;
    const SubPath &s = m.subs.at(sub);
    const int n = s.segmentCount();
    const Flat A = flatten(s);
    QVector<QPair<double, double>> hits;
    // Own other subpaths and the rest of the drawing.
    for (int i = 0; i < m.subs.size(); ++i)
        if (i != sub)
            crossFlats(A, flatten(m.subs.at(i)), &hits);
    for (const PathModel &o : others)
        for (const SubPath &os : o.subs)
            crossFlats(A, flatten(os), &hits);
    for (const auto &h : hits)
        out.append(h.first);
    // Self-crossings, minus the joints between consecutive segments.
    QVector<QPair<double, double>> self;
    crossFlats(A, A, &self);
    for (const auto &h : self)
        if (wrapDist(h.first, h.second, n, s.closed) > 1e-6)
            out.append(h.first);

    std::sort(out.begin(), out.end());
    QVector<double> clean;
    for (double g : out) {
        if (s.closed && g >= n - 1e-9)
            g = 0;   // the start node, seen from the end
        if (!s.closed && (g < 1e-7 || g > n - 1e-7))
            continue;   // an open subpath's own ends bound it anyway
        bool dup = false;
        for (double c : clean)
            dup = dup || wrapDist(c, g, n, s.closed) < 1e-7;
        if (!dup)
            clean.append(g);
    }
    std::sort(clean.begin(), clean.end());
    return clean;
}

void pieceAround(const PathModel &m, int sub, double g, const QVector<double> &cuts,
                 double *from, double *to)
{
    const SubPath &s = m.subs.at(sub);
    const double n = s.segmentCount();
    if (!s.closed) {
        *from = 0;
        *to = n;
        for (double c : cuts) {
            if (c < g) *from = c;
            else if (c > g) { *to = c; break; }
        }
        return;
    }
    if (cuts.size() < 2) {
        *from = cuts.isEmpty() ? 0 : cuts.first();
        *to = *from + n;
        return;
    }
    double lo = cuts.last() - n, hi = cuts.first() + n;
    for (double c : cuts) {
        if (c <= g) lo = c;
        else { hi = c; break; }
    }
    if (lo < 0) { lo += n; hi += n; }
    *from = lo;
    *to = hi;
}

SubPath subRange(const SubPath &s, double from, double to)
{
    SubPath r;
    const int n = s.segmentCount();
    if (n < 1 || to - from < 1e-12)
        return r;
    double x = from;
    while (x < to - 1e-12) {
        int seg = int(std::floor(x + 1e-12));
        if (!s.closed)
            seg = qMin(seg, n - 1);
        const double t0 = qMax(0.0, x - seg);
        const double end = qMin(to, double(seg + 1));
        const double t1 = qMin(1.0, end - seg);
        const int idx = ((seg % n) + n) % n;
        const Cubic part = portion(cubicOf(s, idx), t0, t1);
        if (r.nodes.isEmpty()) {
            PathNode a;
            a.p = a.in = a.out = part.p[0];
            r.nodes.append(a);
        }
        r.nodes.last().out = part.line ? r.nodes.last().p : part.p[1];
        PathNode b;
        b.p = b.out = part.p[3];
        b.in = part.line ? part.p[3] : part.p[2];
        if (t1 >= 1 - 1e-12 && end < to - 1e-12) {
            // An original node inside the range keeps its kind.
            const PathNode &orig = s.nodes.at((idx + 1) % s.nodes.size());
            b.p = orig.p;
            if (part.line) b.in = b.p;
            b.kind = orig.kind;
        }
        r.nodes.append(b);
        x = end;
    }
    return r;
}

static PathModel replaced(const PathModel &m, int sub, const QVector<SubPath> &with)
{
    PathModel out;
    for (int i = 0; i < m.subs.size(); ++i) {
        if (i == sub) {
            for (const SubPath &w : with)
                if (w.nodes.size() >= 2)
                    out.subs.append(w);
        } else {
            out.subs.append(m.subs.at(i));
        }
    }
    return out;
}

PathModel trim(const PathModel &m, QPointF q, double tol, const QVector<PathModel> &others,
               bool *hit)
{
    int sub = -1;
    double g = 0;
    if (!pick(m, q, tol, &sub, &g)) {
        if (hit) *hit = false;
        return m;
    }
    if (hit) *hit = true;
    const SubPath &s = m.subs.at(sub);
    const double n = s.segmentCount();
    double from, to;
    pieceAround(m, sub, g, crossings(m, sub, others), &from, &to);
    QVector<SubPath> keep;
    if (!s.closed) {
        if (from > 1e-9) keep.append(subRange(s, 0, from));
        if (to < n - 1e-9) keep.append(subRange(s, to, n));
    } else if (to - from < n - 1e-9) {
        double a = to, b = from + n;
        if (a >= n) { a -= n; b -= n; }
        keep.append(subRange(s, a, b));
    }
    return replaced(m, sub, keep);
}

QVector<PathModel> breakAt(const PathModel &m, QPointF q, double tol,
                           const QVector<PathModel> &others)
{
    QVector<PathModel> out;
    int sub = -1;
    double g = 0;
    if (!pick(m, q, tol, &sub, &g))
        return out;
    const SubPath &s = m.subs.at(sub);
    const double n = s.segmentCount();
    const QVector<double> cuts = crossings(m, sub, others);
    double from, to;
    pieceAround(m, sub, g, cuts, &from, &to);
    SubPath picked = subRange(s, from, to);
    QVector<SubPath> rest;
    if (!s.closed) {
        if (from > 1e-9) rest.append(subRange(s, 0, from));
        if (to < n - 1e-9) rest.append(subRange(s, to, n));
        if (rest.isEmpty())
            return out;   // no crossing: nothing to break
    } else if (to - from < n - 1e-9) {
        double a = to, b = from + n;
        if (a >= n) { a -= n; b -= n; }
        rest.append(subRange(s, a, b));
    } else if (cuts.isEmpty()) {
        return out;       // a loop nothing crosses
    }   // one crossing: the loop opens there, as one piece
    out.append(replaced(m, sub, {picked}));
    for (const SubPath &r : rest) {
        PathModel pm;
        pm.subs.append(r);
        out.append(pm);
    }
    return out;
}

bool extend(PathModel &m, QPointF q, double tol, const QVector<PathModel> &others)
{
    int sub = -1;
    double g = 0;
    if (!pick(m, q, tol, &sub, &g))
        return false;
    SubPath &s = m.subs[sub];
    const int n = s.segmentCount();
    if (s.closed || n < 1)
        return false;
    const bool atEnd = QLineF(q, s.nodes.last().p).length() < QLineF(q, s.nodes.first().p).length();
    const Cubic endSeg = cubicOf(s, atEnd ? n - 1 : 0);
    const QPointF P = atEnd ? endSeg.p[3] : endSeg.p[0];
    QPointF dir = atEnd ? endSeg.p[3] - endSeg.p[2] : endSeg.p[0] - endSeg.p[1];
    if (std::hypot(dir.x(), dir.y()) < 1e-12)
        dir = atEnd ? endSeg.p[3] - endSeg.p[0] : endSeg.p[0] - endSeg.p[3];
    const double dl = std::hypot(dir.x(), dir.y());
    if (dl < 1e-12)
        return false;
    dir /= dl;
    const double reach = 1e6;
    Cubic ray;
    ray.line = true;
    for (int k = 0; k < 4; ++k)
        ray.p[k] = P + dir * (reach * k / 3);

    // Everything the end could run into: the drawing, the element's other
    // subpaths, and this subpath itself except the segment being extended.
    double best = reach;
    auto test = [&](const SubPath &o, int skipSeg) {
        const Flat f = flatten(o);
        const QRectF rb = QRectF(P, P + dir * reach).normalized().adjusted(-1e-6, -1e-6, 1e-6, 1e-6);
        for (int j = 0; j < f.pts.size(); ++j) {
            if (j == skipSeg || !boxesMeet(rb, f.box.at(j)))
                continue;
            for (int l = 0; l + 1 < f.pts.at(j).size(); ++l) {
                double sr, u;
                if (!edgeCross(P, P + dir * reach, f.pts.at(j).at(l), f.pts.at(j).at(l + 1), &sr, &u))
                    continue;
                double t = sr, v = (f.g.at(j).at(l) + u * (f.g.at(j).at(l + 1) - f.g.at(j).at(l))) - j;
                polish(ray, cubicOf(o, j), &t, &v);
                const double d = t * reach;
                if (d > 1e-6 && d < best)
                    best = d;
            }
        }
    };
    for (int i = 0; i < m.subs.size(); ++i)
        test(m.subs.at(i), i == sub ? (atEnd ? n - 1 : 0) : -1);
    for (const PathModel &o : others)
        for (const SubPath &os : o.subs)
            test(os, -1);
    if (best >= reach)
        return false;
    const QPointF hitPt = P + dir * best;
    if (endSeg.line) {
        PathNode &e = atEnd ? s.nodes.last() : s.nodes.first();
        e.p = e.in = e.out = hitPt;
    } else {
        PathNode e;
        e.p = e.in = e.out = hitPt;
        if (atEnd) s.nodes.append(e);
        else s.nodes.prepend(e);
    }
    return true;
}

} // namespace sketch
} // namespace c2d
