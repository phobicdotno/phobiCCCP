#include "cam3d.h"

#include <QJsonArray>
#include <QLineF>
#include <QPainterPathStroker>
#include <QPolygonF>
#include <QRectF>
#include <QUuid>
#include <QtMath>

#include <algorithm>
#include <cmath>

#ifdef HAVE_CLIPPER2
#include <clipper2/clipper.h>
namespace C2 = Clipper2Lib;
#endif

namespace c2d {

// ---- parameters -----------------------------------------------------------

// Same sign-agnostic depth reading as the 2D exporter.
static double depthToZ(const QJsonValue &v)
{
    return -qAbs(v.isString() ? v.toString().toDouble() : v.toDouble());
}

Cam3dParams cam3dParams(const QJsonObject &j, double safeZ)
{
    Cam3dParams p;
    p.safeZ = safeZ;
    const QJsonObject speeds = j.value("speeds").toObject();
    p.feed = speeds.value("feedrate").toDouble(500);
    p.plunge = speeds.value("plungerate").toDouble(100);
    p.tool = toolGeomFromJson(j.value("tool").toObject());
    p.stockToLeave = qMax(0.0, j.value("stock_to_leave").toDouble(0));
    p.stepdown = qMax(0.05, j.value("stepdown").toDouble(1.0));

    const QJsonValue so = j.value("stepover");
    double stepover = 0;
    if (so.isString()) {
        QString s = so.toString().trimmed();
        if (s.endsWith(QChar('%'))) {
            s.chop(1);
            stepover = p.tool.diameter * s.toDouble() / 100.0;
        } else {
            stepover = s.toDouble();
        }
    } else {
        stepover = so.toDouble(0);
    }
    if (!(stepover > 0))
        stepover = p.tool.diameter * 0.4;
    p.stepover = qMax(0.05, stepover);

    // max_depth 0 / absent = down to the model floor.
    const double md = depthToZ(j.value("max_depth"));
    if (md < -1e-9)
        p.maxDepth = md;
    p.boundaryOffset = j.value("boundary_offset").toDouble(0);
    p.rasterAngle = j.value("raster_angle").toDouble(0);
    const QString dir = j.value("direction").toString().toLower();
    p.direction = dir == QLatin1String("climb") ? Cam3dParams::Climb
                : dir == QLatin1String("conventional") ? Cam3dParams::Conventional
                : Cam3dParams::Zigzag;
    p.bothDirections = j.value("both_directions").toBool(false);
    return p;
}

// ---- region helpers (Clipper2 exact, Qt path booleans as fallback) ---------

static QPainterPath regionFromPolys(const QList<QPolygonF> &polys)
{
    QPainterPath r;
    r.setFillRule(Qt::OddEvenFill);
    for (const QPolygonF &p : polys)
        if (p.size() > 2)
            r.addPolygon(p);
    return r;
}

#ifdef HAVE_CLIPPER2
static const double kScale = 1000.0;   // integer µm

static C2::Path64 toPath64(const QPolygonF &poly, bool closed)
{
    C2::Path64 p;
    const int n = (closed && poly.isClosed()) ? poly.size() - 1 : poly.size();
    p.reserve(size_t(qMax(0, n)));
    for (int i = 0; i < n; ++i)
        p.push_back(C2::Point64(qRound64(poly.at(i).x() * kScale),
                                qRound64(poly.at(i).y() * kScale)));
    return p;
}

static QPolygonF fromPath64(const C2::Path64 &p, bool closed)
{
    QPolygonF out;
    out.reserve(int(p.size()) + 1);
    for (const C2::Point64 &pt : p)
        out.append(QPointF(pt.x / kScale, pt.y / kScale));
    if (closed && out.size() > 2)
        out.append(out.first());
    return out;
}

static C2::Paths64 pathsOf(const QPainterPath &region)
{
    C2::Paths64 ps;
    for (const QPolygonF &poly : region.toSubpathPolygons())
        if (poly.size() > 2)
            ps.push_back(toPath64(poly, true));
    return ps;
}

static QPainterPath regionFrom(const C2::Paths64 &paths)
{
    QList<QPolygonF> polys;
    for (const C2::Path64 &p : paths)
        polys.append(fromPath64(p, true));
    return regionFromPolys(polys);
}

static QPainterPath intersectRegion(const QPainterPath &a, const QPainterPath &b)
{
    return regionFrom(C2::Intersect(pathsOf(a), pathsOf(b), C2::FillRule::EvenOdd));
}

// Union first so outer/hole orientation is canonical before offsetting.
static QPainterPath offsetRegion(const QPainterPath &region, double delta)
{
    const C2::Paths64 u = C2::Union(pathsOf(region), C2::FillRule::EvenOdd);
    return regionFrom(C2::InflatePaths(u, delta * kScale, C2::JoinType::Round,
                                       C2::EndType::Polygon, 2.0, 10.0));
}

// Pieces of the open segment a→b inside the region, as open polylines.
struct LineClipper {
    C2::Paths64 clip;
    explicit LineClipper(const QPainterPath &region) : clip(pathsOf(region)) {}
    QList<QPolygonF> operator()(const QPointF &a, const QPointF &b) const
    {
        C2::Clipper64 c;
        c.AddOpenSubject(C2::Paths64{toPath64(QPolygonF() << a << b, false)});
        c.AddClip(clip);
        C2::Paths64 closedOut, openOut;
        c.Execute(C2::ClipType::Intersection, C2::FillRule::EvenOdd, closedOut, openOut);
        QList<QPolygonF> out;
        for (const C2::Path64 &p : openOut)
            if (p.size() >= 2)
                out.append(fromPath64(p, false));
        return out;
    }
};
#else
static QPainterPath intersectRegion(const QPainterPath &a, const QPainterPath &b)
{
    QPainterPath r = a.intersected(b).simplified();
    r.setFillRule(Qt::OddEvenFill);
    return r;
}

static QPainterPath offsetRegion(const QPainterPath &region, double delta)
{
    QPainterPathStroker st;
    st.setWidth(2 * qAbs(delta));
    st.setJoinStyle(Qt::RoundJoin);
    st.setCapStyle(Qt::RoundCap);
    const QPainterPath band = st.createStroke(region);
    QPainterPath r = (delta > 0 ? region.united(band) : region.subtracted(band)).simplified();
    r.setFillRule(Qt::OddEvenFill);
    return r;
}

// Sampled containment runs (endpoints within 0.05 mm of the boundary).
struct LineClipper {
    QPainterPath region;
    explicit LineClipper(const QPainterPath &r) : region(r) {}
    QList<QPolygonF> operator()(const QPointF &a, const QPointF &b) const
    {
        QList<QPolygonF> out;
        const double len = QLineF(a, b).length();
        const int n = qMax(1, int(std::ceil(len / 0.05)));
        int runStart = -1;
        for (int k = 0; k <= n; ++k) {
            const QPointF q = a + (b - a) * (double(k) / n);
            const bool in = k < n && region.contains(q);
            if (in && runStart < 0)
                runStart = k;
            if (!in && runStart >= 0) {
                if (k - 1 > runStart)
                    out.append(QPolygonF() << a + (b - a) * (double(runStart) / n)
                                           << a + (b - a) * (double(k - 1) / n));
                runStart = -1;
            }
        }
        return out;
    }
};
#endif

QPainterPath cam3dBoundary(const HeightModel &model, const QPainterPath &vectors, double offset)
{
    QPainterPath b = vectors;
    if (b.isEmpty())
        b.addRect(model.bounds());
    b.setFillRule(Qt::OddEvenFill);
    if (qAbs(offset) > 1e-6)
        b = offsetRegion(b, offset);
    return b;
}

// ---- marching squares over the relief -------------------------------------
// Nodes are cell centres, padded by one ring of floor (baseZ) cells so the
// region runs out to the model edge, then a ring that is never inside so every
// contour closes. Node (i, j), i in [-2, cols+1], j in [-2, rows+1].
namespace {

struct Field {
    int cols = 0, rows = 0;
    int W = 0, H = 0;              // padded node counts
    double originX = 0, originY = 0, cell = 1;
    QVector<double> f;             // surface + stock to leave per node

    Field(const HeightModel &m, double leave)
        : cols(m.cols), rows(m.rows), W(m.cols + 4), H(m.rows + 4),
          originX(m.originX), originY(m.originY), cell(m.cell), f(W * H)
    {
        for (int j = -2; j < rows + 2; ++j)
            for (int i = -2; i < cols + 2; ++i) {
                double v;
                if (i < -1 || j < -1 || i > cols || j > rows)
                    v = 1e6;                       // outer ring: never inside
                else if (i < 0 || j < 0 || i >= cols || j >= rows)
                    v = m.baseZ + leave;           // floor ring
                else {
                    const float h = m.at(i, j);
                    v = (h == HeightModel::NoModel ? m.baseZ : double(h)) + leave;
                }
                f[(j + 2) * W + (i + 2)] = v;
            }
    }
    int node(int i, int j) const { return (j + 2) * W + (i + 2); }
    QPointF pos(int n) const
    {
        const int i = n % W - 2, j = n / W - 2;
        return QPointF(originX + (i + 0.5) * cell, originY + (j + 0.5) * cell);
    }
};

// Closed contour polygons of {f <= level}.
QList<QPolygonF> levelContours(const Field &F, double level, QVector<int> &edgeSeg)
{
    const int W = F.W, H = F.H;
    auto in = [&](int n) { return F.f[n] <= level + 1e-9; };
    // Edge ids: 2n = horizontal edge n→n+1, 2n+1 = vertical edge n→n+W.
    QVector<int> segA, segB;
    segA.reserve(W * 2);
    segB.reserve(W * 2);
    for (int j = 0; j < H - 1; ++j) {
        for (int i = 0; i < W - 1; ++i) {
            const int n = j * W + i;
            const int code = (in(n) ? 1 : 0) | (in(n + 1) ? 2 : 0)
                             | (in(n + 1 + W) ? 4 : 0) | (in(n + W) ? 8 : 0);
            if (code == 0 || code == 15)
                continue;
            const int bottom = 2 * n, right = 2 * (n + 1) + 1, top = 2 * (n + W), left = 2 * n + 1;
            auto seg = [&](int a, int b) { segA.append(a); segB.append(b); };
            switch (code) {
            case 1:  seg(left, bottom); break;
            case 2:  seg(bottom, right); break;
            case 3:  seg(left, right); break;
            case 4:  seg(right, top); break;
            case 6:  seg(bottom, top); break;
            case 7:  seg(left, top); break;
            case 8:  seg(top, left); break;
            case 9:  seg(bottom, top); break;
            case 11: seg(right, top); break;
            case 12: seg(right, left); break;
            case 13: seg(bottom, right); break;
            case 14: seg(left, bottom); break;
            case 5:
            case 10: {
                const double centre = (F.f[n] + F.f[n + 1] + F.f[n + 1 + W] + F.f[n + W]) / 4;
                const bool cin = centre <= level + 1e-9;
                if ((code == 5) == cin) { seg(left, top); seg(bottom, right); }
                else                    { seg(left, bottom); seg(right, top); }
                break;
            }
            default: break;
            }
        }
    }
    const int nSeg = segA.size();
    QList<QPolygonF> out;
    if (nSeg == 0)
        return out;

    // Two segment slots per edge crossing.
    std::fill(edgeSeg.begin(), edgeSeg.end(), -1);
    auto attach = [&](int e, int s) { edgeSeg[2 * e + (edgeSeg[2 * e] < 0 ? 0 : 1)] = s; };
    for (int s = 0; s < nSeg; ++s) {
        attach(segA.at(s), s);
        attach(segB.at(s), s);
    }
    auto crossing = [&](int e) {
        const int n0 = e / 2, n1 = (e & 1) ? n0 + W : n0 + 1;
        const double f0 = F.f[n0], f1 = F.f[n1];
        double t = f1 != f0 ? (level - f0) / (f1 - f0) : 0.5;
        t = qBound(0.0, t, 1.0);
        const QPointF p0 = F.pos(n0), p1 = F.pos(n1);
        return p0 + (p1 - p0) * t;
    };

    QVector<bool> used(nSeg, false);
    for (int s0 = 0; s0 < nSeg; ++s0) {
        if (used.at(s0))
            continue;
        used[s0] = true;
        QPolygonF poly;
        const int startEdge = segA.at(s0);
        int cur = segB.at(s0);
        poly.append(crossing(startEdge));
        for (int guard = 0; guard < nSeg + 2; ++guard) {
            poly.append(crossing(cur));
            if (cur == startEdge)
                break;
            int s1 = edgeSeg.at(2 * cur);
            if (s1 < 0 || used.at(s1))
                s1 = edgeSeg.at(2 * cur + 1);
            if (s1 < 0 || used.at(s1))
                break;   // dangling: cannot happen with the outer ring
            used[s1] = true;
            cur = segA.at(s1) == cur ? segB.at(s1) : segA.at(s1);
        }
        if (poly.size() >= 4 && poly.first() == poly.last())
            out.append(poly);
    }
    return out;
}

} // namespace

// ---- rough ----------------------------------------------------------------

QList<RoughLevel> roughLevels(const HeightModel &model, const Cam3dParams &p,
                              const QPainterPath &boundary)
{
    QList<RoughLevel> out;
    if (!model.valid() || boundary.isEmpty())
        return out;
    // The deepest level is the model floor plus the stock to leave (so the
    // floor is actually cleared), or max_depth if that is shallower.
    double floorZ = model.baseZ + p.stockToLeave;
    if (p.maxDepth > floorZ)
        floorZ = p.maxDepth;
    floorZ = qMin(floorZ, 0.0);
    if (floorZ > -1e-6)
        return out;

    const Field F(model, p.stockToLeave);
    QVector<int> edgeSeg(2 * 2 * F.W * F.H);
    for (int k = 1; k < 100000; ++k) {
        double L = -k * p.stepdown;
        bool last = false;
        if (L <= floorZ + 1e-9) {
            L = floorZ;
            last = true;
        }
        const QList<QPolygonF> contours = levelContours(F, L, edgeSeg);
        if (!contours.isEmpty()) {
            const QPainterPath region = intersectRegion(regionFromPolys(contours), boundary);
            if (!region.isEmpty())
                out.append({L, region});
        }
        if (last)
            break;
    }
    return out;
}

// ---- tool compensation ------------------------------------------------------

// Height of the cutting surface above the tip at radius d (d <= tool radius).
static double profileAt(const ToolGeom &t, double d)
{
    const double r = t.radius();
    switch (t.kind) {
    case ToolGeom::Ball:
        return r - std::sqrt(qMax(0.0, r * r - d * d));
    case ToolGeom::VBit:
        return d / std::tan(qDegreesToRadians(qBound(5.0, t.angle, 179.0) / 2));
    case ToolGeom::Flat:
    default: {
        const double c = qMin(t.cornerRadius, r);
        if (c <= 1e-9 || d <= r - c)
            return 0;
        const double u = d - (r - c);
        return c - std::sqrt(qMax(0.0, c * c - u * u));
    }
    }
}

static double nodeValue(const HeightModel &m, int c, int r)
{
    const float h = m.at(c, r);
    return h == HeightModel::NoModel ? m.baseZ : double(h);
}

double compensatedZ(const HeightModel &m, const ToolGeom &tool, double x, double y)
{
    if (!m.valid())
        return 0;
    const double r = tool.radius();
    const double cell = m.cell;
    double best = -1e30;

    // Flat part of the footprint (end mill face, bull-nose centre): the model
    // is bilinear between cell centres, so its maximum over the disc is
    // attained at a node of some cell the disc meets — exact, whatever the
    // wall steepness.
    const double rFlat = tool.kind == ToolGeom::Flat ? r - qMin(tool.cornerRadius, r) : 0.0;
    if (rFlat > 1e-9) {
        const double reach = rFlat + cell;
        const int c0 = int(std::floor((x - reach - m.originX) / cell - 0.5));
        const int c1 = int(std::ceil((x + reach - m.originX) / cell - 0.5));
        const int r0 = int(std::floor((y - reach - m.originY) / cell - 0.5));
        const int r1 = int(std::ceil((y + reach - m.originY) / cell - 0.5));
        for (int rr = r0; rr <= r1; ++rr) {
            const double gy = m.originY + (rr + 0.5) * cell;
            const double dy = qMax(0.0, qAbs(gy - y) - cell);
            for (int cc = c0; cc <= c1; ++cc) {
                const double gx = m.originX + (cc + 0.5) * cell;
                const double dx = qMax(0.0, qAbs(gx - x) - cell);
                if (dx * dx + dy * dy <= rFlat * rFlat)
                    best = qMax(best, nodeValue(m, cc, rr));
            }
        }
    }

    // Curved part (ball, V, bull-nose fillet): supersample the bilinear
    // surface under the inverted tool profile. The sub-step shrinks to a
    // quarter cell while the footprint stays within ~4k samples.
    if (rFlat < r - 1e-9) {
        const int cellsAcross = qMax(1, int(std::ceil(2 * r / cell)));
        const int n = qBound(1, 64 / cellsAcross, 4);
        const double step = cell / n;
        const int k = int(std::ceil(r / step));
        for (int j = -k; j <= k; ++j) {
            for (int i = -k; i <= k; ++i) {
                const double d2 = (i * i + j * j) * step * step;
                if (d2 > r * r)
                    continue;
                const double d = std::sqrt(d2);
                if (d < rFlat)
                    continue;
                const double v = m.sample(x + i * step, y + j * step) - profileAt(tool, d);
                if (v > best)
                    best = v;
            }
        }
    }
    return qMin(0.0, best);
}

bool roughLinkClear(const HeightModel &model, const Cam3dParams &p,
                    const QPointF &a, const QPointF &b, double z)
{
    ToolGeom flat = p.tool;
    flat.kind = ToolGeom::Flat;
    flat.cornerRadius = 0;
    const double len = QLineF(a, b).length();
    const int n = qMax(1, int(std::ceil(len / (model.cell * 0.5))));
    for (int k = 0; k <= n; ++k) {
        const QPointF q = a + (b - a) * (double(k) / n);
        if (compensatedZ(model, flat, q.x(), q.y()) + p.stockToLeave > z + 1e-6)
            return false;
    }
    return true;
}

// ---- finish -----------------------------------------------------------------

namespace {

struct P3 {
    double x, y, z;
};

// Douglas–Peucker on the (distance, Z) profile with a one-sided band: the
// simplified chord may sit above the samples by `up` (material left) but
// below them by at most `down` (a gouge).
void simplifyProfile(const QVector<P3> &pts, int i, int j, double up, double down,
                     QVector<int> &keep)
{
    if (j - i < 2)
        return;
    const double sx = pts.at(j).x - pts.at(i).x, sy = pts.at(j).y - pts.at(i).y;
    const double len2 = sx * sx + sy * sy;
    int worst = -1;
    double worstScore = 1.0;
    for (int k = i + 1; k < j; ++k) {
        const double t = len2 > 1e-18
            ? ((pts.at(k).x - pts.at(i).x) * sx + (pts.at(k).y - pts.at(i).y) * sy) / len2
            : double(k - i) / (j - i);
        const double zc = pts.at(i).z + (pts.at(j).z - pts.at(i).z) * t;
        const double dev = zc - pts.at(k).z;
        const double score = dev >= 0 ? dev / up : -dev / down;
        if (score > worstScore) {
            worstScore = score;
            worst = k;
        }
    }
    if (worst < 0)
        return;
    simplifyProfile(pts, i, worst, up, down, keep);
    keep.append(worst);
    simplifyProfile(pts, worst, j, up, down, keep);
}

} // namespace

QVector<Op> finishOps(const HeightModel &model, const Cam3dParams &p,
                      const QPainterPath &boundary, int *passes)
{
    QVector<Op> ops;
    int nPasses = 0;
    if (passes)
        *passes = 0;
    if (!model.valid() || boundary.isEmpty())
        return ops;
    // A model with no defined cell has no surface to follow: finishing it
    // would raster the whole boundary at the stock top and cut nothing.
    bool anyRelief = false;
    for (float h : model.z)
        if (h != HeightModel::NoModel) { anyRelief = true; break; }
    if (!anyRelief)
        return ops;

    const double cell = model.cell;
    const double leave = p.stockToLeave;
    auto zAt = [&](const QPointF &q) {
        double z = compensatedZ(model, p.tool, q.x(), q.y()) + leave;
        z = qMax(z, p.maxDepth);
        return qMin(0.0, z);
    };
    // Straight tool-centre run a→b sampled at <= cell spacing, refined where
    // the surface bulges above a chord (a ball rolling over a rim makes the
    // compensated surface far steeper than the cell spacing resolves), then
    // simplified.
    const double kUp = 0.02, kDown = 0.005;
    auto sampleRun = [&](const QPointF &a, const QPointF &b) {
        const double len = QLineF(a, b).length();
        const int n = qMax(1, int(std::ceil(len / cell)));
        QVector<P3> pts;
        pts.reserve(n + 1);
        std::function<void(const P3 &, const P3 &, int)> refine =
            [&](const P3 &s, const P3 &e, int depth) {
                const QPointF q((s.x + e.x) / 2, (s.y + e.y) / 2);
                const P3 m{q.x(), q.y(), zAt(q)};
                if (depth >= 6 || m.z - (s.z + e.z) / 2 <= kDown)
                    return;
                refine(s, m, depth + 1);
                pts.append(m);
                refine(m, e, depth + 1);
            };
        P3 prev{a.x(), a.y(), zAt(a)};
        pts.append(prev);
        for (int k = 1; k <= n; ++k) {
            const QPointF q = a + (b - a) * (double(k) / n);
            const P3 next{q.x(), q.y(), zAt(q)};
            refine(prev, next, 0);
            pts.append(next);
            prev = next;
        }
        QVector<int> keep;
        keep.append(0);
        simplifyProfile(pts, 0, pts.size() - 1, kUp, kDown, keep);
        keep.append(pts.size() - 1);
        std::sort(keep.begin(), keep.end());
        QVector<P3> out;
        for (int k : keep)
            out.append(pts.at(k));
        return out;
    };

    const LineClipper clipper(boundary);
    const QPainterPath linkRegion = offsetRegion(boundary, 0.05);
    const double linkDist = 10.0 * qMax(p.stepover, p.tool.radius());
    auto linkInside = [&](const QPointF &a, const QPointF &b) {
        const double len = QLineF(a, b).length();
        const int n = qMax(1, int(std::ceil(len / cell)));
        for (int k = 0; k <= n; ++k)
            if (!linkRegion.contains(a + (b - a) * (double(k) / n)))
                return false;
        return true;
    };

    bool haveCur = false;
    P3 cur{0, 0, 0};
    auto emitRun = [&](const QVector<P3> &pts, bool linked) {
        if (!linked) {
            if (haveCur)
                ops.append(Op::rapid(cur.x, cur.y, p.safeZ));
            ops.append(Op::rapid(pts.first().x, pts.first().y, p.safeZ));
            ops.append(Op::feedTo(pts.first().x, pts.first().y, pts.first().z, p.plunge));
        }
        for (int k = 1; k < pts.size(); ++k)
            ops.append(Op::feedTo(pts.at(k).x, pts.at(k).y, pts.at(k).z, p.feed));
        cur = pts.last();
        haveCur = true;
    };

    const QRectF bb = boundary.boundingRect();
    auto rasterSet = [&](double angleDeg) {
        const double th = qDegreesToRadians(angleDeg);
        const QPointF d(std::cos(th), std::sin(th));
        const QPointF nrm(-d.y(), d.x());   // passes advance along +nrm
        double oMin = 1e30, oMax = -1e30, sMin = 1e30, sMax = -1e30;
        for (const QPointF &c : {bb.topLeft(), bb.topRight(), bb.bottomLeft(), bb.bottomRight()}) {
            const double o = c.x() * nrm.x() + c.y() * nrm.y();
            const double s = c.x() * d.x() + c.y() * d.y();
            oMin = qMin(oMin, o); oMax = qMax(oMax, o);
            sMin = qMin(sMin, s); sMax = qMax(sMax, s);
        }
        QVector<double> offsets;
        for (double o = oMin; o <= oMax + 1e-6; o += p.stepover)
            offsets.append(o);
        if (offsets.isEmpty() || oMax - offsets.last() > 0.05 * p.stepover)
            offsets.append(oMax);

        // Climb: uncut material (the next pass, +nrm) on the left of travel
        // → travel along +d for a clockwise spindle. Conventional: −d.
        int dir = p.direction == Cam3dParams::Conventional ? -1 : 1;
        for (double o : offsets) {
            const QPointF a = d * (sMin - 1.0) + nrm * o;
            const QPointF b = d * (sMax + 1.0) + nrm * o;
            QList<QPolygonF> pieces = clipper(a, b);
            if (pieces.isEmpty())
                continue;
            for (QPolygonF &pc : pieces) {
                QPointF s = pc.first(), e = pc.last();
                if ((e.x() - s.x()) * d.x() + (e.y() - s.y()) * d.y() < 0)
                    std::swap(s, e);
                pc = QPolygonF() << s << e;
            }
            std::sort(pieces.begin(), pieces.end(), [&](const QPolygonF &u, const QPolygonF &v) {
                return u.first().x() * d.x() + u.first().y() * d.y()
                     < v.first().x() * d.x() + v.first().y() * d.y();
            });
            if (dir < 0)
                std::reverse(pieces.begin(), pieces.end());
            for (const QPolygonF &pc : pieces) {
                const QPointF s = dir > 0 ? pc.first() : pc.last();
                const QPointF e = dir > 0 ? pc.last() : pc.first();
                if (QLineF(s, e).length() < 1e-6)
                    continue;
                bool linked = false;
                if (p.direction == Cam3dParams::Zigzag && haveCur) {
                    const QPointF from(cur.x, cur.y);
                    const double dl = QLineF(from, s).length();
                    if (dl < 1e-6) {
                        linked = true;
                    } else if (dl <= linkDist && linkInside(from, s)) {
                        emitRun(sampleRun(from, s), true);
                        linked = true;
                    }
                }
                emitRun(sampleRun(s, e), linked);
                ++nPasses;
            }
            if (p.direction == Cam3dParams::Zigzag)
                dir = -dir;
        }
    };
    rasterSet(p.rasterAngle);
    if (p.bothDirections)
        rasterSet(p.rasterAngle + 90.0);
    if (haveCur)
        ops.append(Op::rapid(cur.x, cur.y, p.safeZ));
    if (passes)
        *passes = nPasses;
    return ops;
}

// ---- defaults for the panel ----------------------------------------------

static QJsonObject speedsJson(double feed, double plunge, int rpm)
{
    return QJsonObject{{"feedrate", feed}, {"plungerate", plunge}, {"rpm", rpm}};
}

static QJsonObject baseJson(const char *type, const char *name)
{
    QJsonObject j;
    j.insert("type", QLatin1String(type));
    j.insert("name", QLatin1String(name));
    j.insert("enabled", true);
    j.insert("uuid", QUuid::createUuid().toString());
    j.insert("elements", QJsonArray());
    j.insert("start_depth", QStringLiteral("0.000"));
    j.insert("max_depth", QStringLiteral("0.000"));   // 0 = to the model floor
    j.insert("boundary_offset", 0.0);
    return j;
}

QJsonObject defaultRoughToolpathJson()
{
    QJsonObject j = baseJson("3d_rough_toolpath", "3D Rough");
    j.insert("tool", QJsonObject{{"number", 201}, {"diameter", 6.35}, {"type", 0},
                                 {"angle", 0}, {"corner_radius", 0}, {"flutes", 3},
                                 {"name", QStringLiteral("#201 1/4\" End Mill")}});
    j.insert("speeds", speedsJson(1500, 500, 18000));
    j.insert("stock_to_leave", 0.5);
    j.insert("stepdown", 2.0);
    j.insert("stepover", 2.54);
    return j;
}

QJsonObject defaultFinishToolpathJson()
{
    QJsonObject j = baseJson("3d_finish_toolpath", "3D Finish");
    j.insert("tool", QJsonObject{{"number", 202}, {"diameter", 3.175}, {"type", 1},
                                 {"angle", 0}, {"corner_radius", 1.5875}, {"flutes", 2},
                                 {"name", QStringLiteral("#202 1/8\" Ball")}});
    j.insert("speeds", speedsJson(1500, 500, 18000));
    j.insert("stock_to_leave", 0.0);
    j.insert("stepdown", 1.0);
    j.insert("stepover", 0.5);
    j.insert("raster_angle", 0.0);
    j.insert("direction", QStringLiteral("zigzag"));
    j.insert("both_directions", false);
    return j;
}

} // namespace c2d
