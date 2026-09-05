#include "gcodeexport.h"
#include "c2ddocument.h"
#include "cam3d.h"
#include "heightmodel.h"
#include "post_grbl.h"
#include "vcarve.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QLineF>
#include <QPainterPathStroker>
#include <QPolygonF>
#include <QtMath>
#include <algorithm>
#include <functional>

#ifdef HAVE_CLIPPER2
#include <clipper2/clipper.h>
namespace C2 = Clipper2Lib;
#endif

namespace c2d {

// Depth values are strings whose sign convention flipped between CC builds
// (843 stores "-19.126", 853 stores "19.126" for the same cut). Machine Z is
// negative below stock top either way.
static double depthToZ(const QJsonValue &v)
{
    return -qAbs(v.isString() ? v.toString().toDouble() : v.toDouble());
}

static QVector<const Element *> referenced(Document &doc, const QJsonObject &tp)
{
    QVector<const Element *> out;
    for (const QJsonValue &ev : tp.value("elements").toArray()) {
        const QString id = ev.toObject().value("uuid").toString();
        if (const Element *e = doc.elementById(id))
            out.append(e);
    }
    return out;
}

[[maybe_unused]] static void closeLoop(QPolygonF &poly)
{
    if (poly.size() > 2 && poly.first() != poly.last())
        poly.append(poly.first());
}

// Flatten curves to polygons at `tol` mm. Qt flattens beziers at a fixed
// 0.5 path-unit threshold, which in millimetres turns a Ø6.6 circle into a
// ~16-gon; offsetting those chords instead of the arc shrinks a ring by
// delta*(1/cos(pi/n)-1) — 0.06 mm at a 3 mm tool radius. Flatten in a scaled
// space instead so the chord error is negligible against the 0.01 mm arc-fit
// tolerance used downstream.
static QList<QPolygonF> finePolygons(const QPainterPath &path, double tol = 0.005)
{
    const double k = 0.5 / tol;
    QList<QPolygonF> out = path.toSubpathPolygons(QTransform::fromScale(k, k));
    for (QPolygonF &poly : out)
        for (QPointF &pt : poly)
            pt /= k;
    return out;
}

#ifdef HAVE_CLIPPER2
// ---- exact geometry backend (Clipper2) ----------------------------------
static const double kScale = 1000.0;   // integer µm

static C2::Path64 toPath64(const QPolygonF &poly)
{
    C2::Path64 p;
    const int n = poly.isClosed() ? poly.size() - 1 : poly.size();
    p.reserve(size_t(n));
    for (int i = 0; i < n; ++i)
        p.push_back(C2::Point64(qRound64(poly.at(i).x() * kScale),
                                qRound64(poly.at(i).y() * kScale)));
    return p;
}

static QPolygonF fromPath64(const C2::Path64 &p)
{
    QPolygonF out;
    out.reserve(int(p.size()) + 1);
    for (const C2::Point64 &pt : p)
        out.append(QPointF(pt.x / kScale, pt.y / kScale));
    if (out.size() > 2)
        out.append(out.first());
    return out;
}

static C2::Paths64 pathsOfRegion(const QPainterPath &region)
{
    C2::Paths64 ps;
    for (const QPolygonF &poly : region.toSubpathPolygons())
        if (poly.size() > 2)
            ps.push_back(toPath64(poly));
    return ps;
}

// Union of the referenced closed vectors as a filled region (even-odd, so
// nested vectors become holes/islands exactly as CC treats them).
static QPainterPath regionOf(const QVector<const Element *> &elems, double tol = 0.005)
{
    C2::Paths64 subj;
    for (const Element *e : elems)
        for (const QPolygonF &poly : finePolygons(e->painterPath, tol))
            if (poly.size() > 2)
                subj.push_back(toPath64(poly));
    const C2::Paths64 u = C2::Union(subj, C2::FillRule::EvenOdd);
    QPainterPath r;
    r.setFillRule(Qt::OddEvenFill);
    for (const C2::Path64 &p : u)
        r.addPolygon(fromPath64(p));
    return r;
}

static QList<QPolygonF> ringsFromPaths(const C2::Paths64 &paths)
{
    QList<QPolygonF> out;
    for (const C2::Path64 &p : paths)
        if (p.size() > 2)
            out.append(fromPath64(p));
    return out;
}

static QList<QPolygonF> insetRings(const QPainterPath &region, double delta)
{
    return ringsFromPaths(C2::InflatePaths(pathsOfRegion(region), -delta * kScale,
                                           C2::JoinType::Round, C2::EndType::Polygon,
                                           2.0, 10.0));
}

static QList<QPolygonF> outsetRings(const QPainterPath &region, double delta)
{
    return ringsFromPaths(C2::InflatePaths(pathsOfRegion(region), delta * kScale,
                                           C2::JoinType::Round, C2::EndType::Polygon,
                                           2.0, 10.0));
}

// Connected components via Clipper2's polytree: each top-level polygon plus
// its immediate holes is one machinable pocket; islands nested inside holes
// start new components.
static void collectComponents(const C2::PolyPath64 &node, QList<QPainterPath> &out)
{
    QPainterPath comp;
    comp.setFillRule(Qt::OddEvenFill);
    comp.addPolygon(fromPath64(node.Polygon()));
    for (const auto &hole : node) {
        comp.addPolygon(fromPath64(hole->Polygon()));
        for (const auto &nested : *hole)
            collectComponents(*nested, out);
    }
    out.append(comp);
}

static QList<QPainterPath> components(const QPainterPath &region)
{
    C2::Clipper64 c;
    c.AddSubject(pathsOfRegion(region));
    C2::PolyTree64 tree;
    c.Execute(C2::ClipType::Union, C2::FillRule::EvenOdd, tree);
    QList<QPainterPath> out;
    for (const auto &child : tree)
        collectComponents(*child, out);
    return out;
}

#else
// ---- approximate geometry backend (Qt path booleans) ---------------------
// Stroking the region boundary with width 2·delta gives a band from
// -delta..+delta around it; subtracting the band insets the region, uniting
// it outsets. Round joins approximate the true tool-radius offset.
static QPainterPath offsetBand(const QPainterPath &region, double delta)
{
    QPainterPathStroker st;
    st.setWidth(2 * delta);
    st.setJoinStyle(Qt::RoundJoin);
    st.setCapStyle(Qt::RoundCap);
    return st.createStroke(region);
}

static QList<QPolygonF> insetRings(const QPainterPath &region, double delta)
{
    QList<QPolygonF> out;
    const QPainterPath inner =
        region.subtracted(offsetBand(region, delta)).simplified();
    for (QPolygonF p : inner.toSubpathPolygons())
        if (p.size() > 2) {
            closeLoop(p);
            out.append(p);
        }
    return out;
}

static QList<QPolygonF> outsetRings(const QPainterPath &region, double delta)
{
    QList<QPolygonF> out;
    const QPainterPath outer =
        region.united(offsetBand(region, delta)).simplified();
    for (QPolygonF p : outer.toSubpathPolygons())
        if (p.size() > 2) {
            closeLoop(p);
            out.append(p);
        }
    return out;
}

// Union of the referenced closed vectors as a filled region.
static QPainterPath regionOf(const QVector<const Element *> &elems, double = 0.005)
{
    // Qt's path booleans flatten curves at a fixed 0.5 path-unit tolerance.
    // In millimetres that turns a Ø6.6 circle into a ~16-gon, and insetting
    // the chords instead of the arc shrinks every ring by delta*(1/cos(pi/n)-1)
    // — 0.06 mm at a 3 mm tool radius. Do the booleans in 0.01 mm units so
    // the flattening error is 0.005 mm, then map back.
    const double k = 100.0;
    const QTransform up = QTransform::fromScale(k, k);
    const QTransform down = QTransform::fromScale(1.0 / k, 1.0 / k);
    QPainterPath r;
    for (const Element *e : elems) {
        const QPainterPath p = up.map(e->painterPath);
        r = r.isEmpty() ? p : r.united(p);
    }
    return down.map(r.simplified());
}

// Split a (simplified) region into connected components: each outer boundary
// paired with its immediate holes, so pockets are machined one at a time.
static QList<QPainterPath> components(const QPainterPath &region)
{
    const QList<QPolygonF> polys = region.toSubpathPolygons();
    const int n = polys.size();
    QVector<int> depth(n, 0);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            if (i != j && !polys.at(i).isEmpty()
                && polys.at(j).containsPoint(polys.at(i).first(), Qt::OddEvenFill))
                depth[i]++;

    QHash<int, QPainterPath> comp;
    for (int i = 0; i < n; ++i)
        if (depth.at(i) % 2 == 0) {
            QPainterPath p;
            p.addPolygon(polys.at(i));
            p.closeSubpath();
            comp.insert(i, p);
        }
    for (int i = 0; i < n; ++i) {
        if (depth.at(i) % 2 == 0)
            continue;
        for (int j = 0; j < n; ++j) {
            if (j != i && depth.at(j) == depth.at(i) - 1 && comp.contains(j)
                && polys.at(j).containsPoint(polys.at(i).first(), Qt::OddEvenFill)) {
                comp[j].addPolygon(polys.at(i));
                comp[j].closeSubpath();
                break;
            }
        }
    }
    return comp.values();
}
#endif // HAVE_CLIPPER2

// ---- rest machining (both backends) ---------------------------------------
static QPainterPath regionFromRings(const QList<QPolygonF> &rings)
{
    QPainterPath r;
    r.setFillRule(Qt::OddEvenFill);
    for (const QPolygonF &p : rings)
        if (p.size() > 2)
            r.addPolygon(p);
    return r;
}

#ifdef HAVE_CLIPPER2
static QPainterPath subtractRegion(const QPainterPath &a, const QPainterPath &b)
{
    return regionFromRings(ringsFromPaths(
        C2::Difference(pathsOfRegion(a), pathsOfRegion(b), C2::FillRule::EvenOdd)));
}
#else
static QPainterPath subtractRegion(const QPainterPath &a, const QPainterPath &b)
{
    QPainterPath r = a.subtracted(b).simplified();
    r.setFillRule(Qt::OddEvenFill);
    return r;
}
#endif

// Rest machining: the tool-centre regions this cutter must still visit after
// a prevR cutter roughed the same pocket. `mine` is where this tool fits
// (inset by toolR + stock); `cleared` is what the previous tool swept (its
// centre region outset by its own radius, i.e. the morphological opening of
// the pocket). Every centre whose whole disc lies inside `cleared` —
// inset(cleared, toolR) — can be skipped; what remains hugs the corners and
// channels the big tool could not enter, plus one tool radius of overlap into
// the cleared floor so the two cuts blend. (Subtracting `cleared` itself
// would leave the material within toolR of its rounded corners uncut.)
static QList<QPainterPath> restRegions(const QPainterPath &comp, double toolR,
                                       double prevR, double leave)
{
    const QPainterPath mine = regionFromRings(insetRings(comp, toolR + leave));
    if (mine.isEmpty())
        return {};
    const QPainterPath prevCentres = regionFromRings(insetRings(comp, prevR + leave));
    if (prevCentres.isEmpty())
        return components(mine);          // the big tool never fitted: all rest
    const QPainterPath cleared = regionFromRings(outsetRings(prevCentres, prevR));
    const QPainterPath skip = regionFromRings(insetRings(cleared, toolR));
    const QPainterPath rest = skip.isEmpty() ? mine : subtractRegion(mine, skip);
    if (rest.isEmpty())
        return {};
    return components(rest);
}

// Circular-ring detection: every vertex equidistant from the vertex centroid,
// letting the ring go out as two G2/G3 half arcs instead of line segments.
static bool asCircle(const QPolygonF &poly, QPointF *center, double *radius, bool *ccw)
{
    if (poly.size() < 8 || poly.first() != poly.last())
        return false;
    const int n = poly.size() - 1;
    QPointF c(0, 0);
    for (int i = 0; i < n; ++i)
        c += poly.at(i);
    c /= double(n);
    double rSum = 0;
    for (int i = 0; i < n; ++i)
        rSum += QLineF(c, poly.at(i)).length();
    const double r = rSum / n;
    if (r < 0.05)
        return false;
    for (int i = 0; i < n; ++i)
        if (qAbs(QLineF(c, poly.at(i)).length() - r) > qMax(0.01, r * 0.004))
            return false;
    // A regular polygon also has equidistant vertices; only accept rings
    // dense enough that the chords themselves stay on the circle (sagitta
    // ≤ 0.02 mm), i.e. flattened arcs, not hexagons.
    for (int i = 0; i < n; ++i) {
        const QPointF &a = poly.at(i), &b = poly.at(i + 1);
        const double chord = QLineF(a, b).length();
        if (chord > 1e-9 && r - qSqrt(qMax(0.0, r * r - chord * chord / 4)) > 0.02)
            return false;
    }
    double area2 = 0;
    for (int i = 0; i < n; ++i) {
        const QPointF &a = poly.at(i), &b = poly.at(i + 1);
        area2 += a.x() * b.y() - b.x() * a.y();
    }
    *center = c;
    *radius = r;
    *ccw = area2 > 0;
    return true;
}

// ---- Polyline compression: greedy longest-run line/arc fitting ------------
// Clipper's round joins and text outlines arrive as dense runs of short
// segments; GRBL's planner starves on those at high feed. Fit maximal
// collinear runs to single G1s and maximal circular runs to G2/G3, keeping
// every original vertex within `tol` of the emitted path.

struct FitSeg {
    int end;        // index into the polyline this segment ends at
    bool arc;
    QPointF c;      // arc center (absolute)
    bool cw;        // G2 vs G3
};

static bool circumcenter(const QPointF &A, const QPointF &B, const QPointF &C, QPointF *o)
{
    const double d = 2 * (A.x() * (B.y() - C.y()) + B.x() * (C.y() - A.y())
                          + C.x() * (A.y() - B.y()));
    if (qAbs(d) < 1e-9)
        return false;
    const double a2 = A.x() * A.x() + A.y() * A.y();
    const double b2 = B.x() * B.x() + B.y() * B.y();
    const double c2 = C.x() * C.x() + C.y() * C.y();
    o->setX((a2 * (B.y() - C.y()) + b2 * (C.y() - A.y()) + c2 * (A.y() - B.y())) / d);
    o->setY((a2 * (C.x() - B.x()) + b2 * (A.x() - C.x()) + c2 * (B.x() - A.x())) / d);
    return true;
}

// pts[i..j] on one straight segment i->j within tol, projections monotonic.
static bool lineSpanOk(const QPolygonF &pts, int i, int j, double tol)
{
    const QPointF d = pts.at(j) - pts.at(i);
    const double len2 = d.x() * d.x() + d.y() * d.y();
    if (len2 < 1e-12)
        return false;
    double tPrev = 0;
    for (int k = i + 1; k < j; ++k) {
        const QPointF v = pts.at(k) - pts.at(i);
        const double t = (v.x() * d.x() + v.y() * d.y()) / len2;
        if (t <= tPrev || t >= 1.0)
            return false;
        if (qAbs(v.x() * d.y() - v.y() * d.x()) / qSqrt(len2) > tol)
            return false;
        tPrev = t;
    }
    return true;
}

// pts[i..j] on one circular arc within tol. The center is re-projected onto
// the chord's perpendicular bisector so the start and end radii match exactly
// (GRBL rejects arcs whose I/J radius disagrees with the endpoint radius).
static bool arcSpanOk(const QPolygonF &pts, int i, int j, double tol,
                      QPointF *center, bool *cwOut)
{
    if (j - i < 4)
        return false;
    QPointF c0;
    if (!circumcenter(pts.at(i), pts.at((i + j) / 2), pts.at(j), &c0))
        return false;
    const QPointF chord = pts.at(j) - pts.at(i);
    const double clen = qSqrt(chord.x() * chord.x() + chord.y() * chord.y());
    if (clen < 1e-6)
        return false;
    const QPointF mid((pts.at(i).x() + pts.at(j).x()) / 2,
                      (pts.at(i).y() + pts.at(j).y()) / 2);
    const QPointF perp(-chord.y() / clen, chord.x() / clen);
    const double h = (c0.x() - mid.x()) * perp.x() + (c0.y() - mid.y()) * perp.y();
    const QPointF c(mid.x() + perp.x() * h, mid.y() + perp.y() * h);
    const double r = QLineF(c, pts.at(i)).length();
    if (r < 0.2 || r > 5000.0)
        return false;
    double aPrev = qAtan2(pts.at(i).y() - c.y(), pts.at(i).x() - c.x());
    double total = 0;
    int dir = 0;
    for (int k = i + 1; k <= j; ++k) {
        if (qAbs(QLineF(c, pts.at(k)).length() - r) > tol)
            return false;
        // The original straight chord must hug the arc too, or a regular
        // polygon (vertices co-circular, sides not) would become a circle.
        const double chord = QLineF(pts.at(k - 1), pts.at(k)).length();
        if (r - qSqrt(qMax(0.0, r * r - chord * chord / 4)) > 2 * tol)
            return false;
        const double a = qAtan2(pts.at(k).y() - c.y(), pts.at(k).x() - c.x());
        double da = a - aPrev;
        while (da > M_PI)  da -= 2 * M_PI;
        while (da < -M_PI) da += 2 * M_PI;
        if (qAbs(da) < 1e-12)
            return false;
        const int s = da > 0 ? 1 : -1;
        if (dir == 0)
            dir = s;
        else if (s != dir)
            return false;
        total += da;
        aPrev = a;
    }
    if (qAbs(total) > 2 * M_PI - 0.2)   // near-full circles stay with asCircle
        return false;
    *center = c;
    *cwOut = dir < 0;
    return true;
}

static QVector<FitSeg> fitPolyline(const QPolygonF &pts, double tol)
{
    QVector<FitSeg> out;
    const int n = pts.size();
    int i = 0;
    while (i < n - 1) {
        int lineEnd = i + 1;
        for (int j = i + 2; j < n && lineSpanOk(pts, i, j, tol); ++j)
            lineEnd = j;
        int arcEnd = -1;
        QPointF c;
        bool cw = false;
        for (int j = i + 4; j < n; ++j) {
            QPointF cj;
            bool cwj;
            if (!arcSpanOk(pts, i, j, tol, &cj, &cwj))
                break;
            arcEnd = j;
            c = cj;
            cw = cwj;
        }
        FitSeg seg;
        if (arcEnd > lineEnd) {
            seg = {arcEnd, true, c, cw};
        } else {
            seg = {lineEnd, false, QPointF(), false};
        }
        out.append(seg);
        i = seg.end;
    }
    return out;
}

static double perimeter(const QPolygonF &poly)
{
    double len = 0;
    for (int i = 1; i < poly.size(); ++i)
        len += QLineF(poly.at(i - 1), poly.at(i)).length();
    return len;
}

// ---- engraving fill: zigzag hatch clipped to a ring set -------------------
// Parallel lines at `spacing` / `angleDeg` (anchored on the rings' bounding
// box centre), cut against the closed even-odd rings by exact line/edge
// intersection: the sorted crossings along each line pair up into inside
// runs. Runs on neighbouring lines are chained into one open polyline when
// the walk along the ring boundary between their adjacent ends is short (the
// boundary is the tool-centre path of the inset, so walking it cuts nothing
// outside the region); otherwise the chain ends and the next run starts
// with a retract. Result: open polylines, ready to be one Job each.
namespace {
struct HatchHit {
    double s;       // position along the hatch line
    int ring, edge; // which ring / which edge of it was crossed
    QPointF p;
};
struct HatchRun {
    int line;
    HatchHit a, b;  // a.s < b.s
    bool used = false;
};
} // namespace

static QList<QPolygonF> hatchRings(const QList<QPolygonF> &ringsIn, double spacing,
                                   double angleDeg)
{
    QList<QPolygonF> rings;
    for (QPolygonF r : ringsIn) {
        if (r.size() > 2 && r.first() != r.last())
            r.append(r.first());
        if (r.size() > 3)
            rings.append(r);
    }
    QList<QPolygonF> out;
    if (rings.isEmpty() || spacing < 0.01)
        return out;

    // Per-ring cumulative arc length (vertex i sits at cum[i]); m = vertex count.
    QVector<QVector<double>> cum(rings.size());
    QVector<double> total(rings.size());
    QRectF bb;
    for (int r = 0; r < rings.size(); ++r) {
        const QPolygonF &ring = rings.at(r);
        cum[r].resize(ring.size());
        cum[r][0] = 0;
        for (int i = 1; i < ring.size(); ++i)
            cum[r][i] = cum[r][i - 1] + QLineF(ring.at(i - 1), ring.at(i)).length();
        total[r] = cum[r].last();
        bb |= ring.boundingRect();
    }
    const double ang = qDegreesToRadians(angleDeg);
    const QPointF dir(qCos(ang), qSin(ang));
    const QPointF nrm(-dir.y(), dir.x());
    const QPointF c = bb.center();
    const double diag = QLineF(bb.topLeft(), bb.bottomRight()).length();
    const int K = int(std::ceil(diag / 2 / spacing));

    QVector<HatchRun> runs;
    for (int k = -K; k <= K; ++k) {
        const QPointF base = c + nrm * (k * spacing);
        QVector<HatchHit> hits;
        for (int r = 0; r < rings.size(); ++r) {
            const QPolygonF &ring = rings.at(r);
            for (int i = 0; i + 1 < ring.size(); ++i) {
                const QPointF &P = ring.at(i), &Q = ring.at(i + 1);
                const double fP = (P.x() - base.x()) * nrm.x() + (P.y() - base.y()) * nrm.y();
                const double fQ = (Q.x() - base.x()) * nrm.x() + (Q.y() - base.y()) * nrm.y();
                // half-open crossing test: a vertex exactly on the line counts once
                if ((fP <= 0 && fQ > 0) || (fQ <= 0 && fP > 0)) {
                    const double t = fP / (fP - fQ);
                    const QPointF p = P + t * (Q - P);
                    hits.append({(p.x() - base.x()) * dir.x() + (p.y() - base.y()) * dir.y(),
                                 r, i, p});
                }
            }
        }
        std::sort(hits.begin(), hits.end(),
                  [](const HatchHit &a, const HatchHit &b) { return a.s < b.s; });
        for (int h = 0; h + 1 < hits.size(); h += 2)
            if (hits.at(h + 1).s - hits.at(h).s > 0.05)
                runs.append({k, hits.at(h), hits.at(h + 1), false});
    }

    // Arc-length position of a hit on its ring.
    auto posOf = [&](const HatchHit &h) {
        return cum[h.ring][h.edge] + QLineF(rings[h.ring].at(h.edge), h.p).length();
    };
    // Shortest boundary walk from hit A to hit B (same ring); the vertices in
    // between are appended to `path` when `emit` is set. Returns the length.
    auto walk = [&](const HatchHit &A, const HatchHit &B, QPolygonF *path) {
        if (A.ring != B.ring)
            return 1e18;
        const int r = A.ring;
        const int m = rings[r].size() - 1;
        const double L = total[r];
        const double sa = posOf(A), sb = posOf(B);
        double fwd = sb - sa;
        while (fwd < 0) fwd += L;
        const double back = L - fwd;
        if (!path)
            return qMin(fwd, back);
        if (fwd <= back) {
            if (!(A.edge == B.edge && sb >= sa)) {
                int idx = (A.edge + 1) % m;
                for (int guard = 0; guard <= m; ++guard) {
                    path->append(rings[r].at(idx));
                    if (idx == B.edge)
                        break;
                    idx = (idx + 1) % m;
                }
            }
            return fwd;
        }
        if (!(A.edge == B.edge && sb <= sa)) {
            int idx = A.edge;
            for (int guard = 0; guard <= m; ++guard) {
                path->append(rings[r].at(idx));
                if (idx == (B.edge + 1) % m)
                    break;
                idx = (idx - 1 + m) % m;
            }
        }
        return back;
    };

    const double maxWalk = qMax(4.0 * spacing, 2.0);
    for (int start = 0; start < runs.size(); ++start) {
        if (runs.at(start).used)
            continue;
        runs[start].used = true;
        QPolygonF path;
        path << runs.at(start).a.p << runs.at(start).b.p;
        HatchHit end = runs.at(start).b;
        int line = runs.at(start).line;
        for (;;) {
            // Nearest (by boundary walk) unused run on the next line.
            int best = -1;
            bool bestFromA = true;
            double bestD = maxWalk;
            for (int i = start + 1; i < runs.size() && runs.at(i).line <= line + 1; ++i) {
                if (runs.at(i).used || runs.at(i).line != line + 1)
                    continue;
                const double da = walk(end, runs.at(i).a, nullptr);
                const double db = walk(end, runs.at(i).b, nullptr);
                if (da <= bestD) { bestD = da; best = i; bestFromA = true; }
                if (db < bestD)  { bestD = db; best = i; bestFromA = false; }
            }
            if (best < 0)
                break;
            runs[best].used = true;
            const HatchHit &near = bestFromA ? runs.at(best).a : runs.at(best).b;
            const HatchHit &far = bestFromA ? runs.at(best).b : runs.at(best).a;
            walk(end, near, &path);
            path << near.p << far.p;
            end = far;
            ++line;
        }
        out.append(path);
    }
    return out;
}

// Deterministic pseudo-random for the texture toolpath (no wall-clock seeds).
struct Lcg {
    quint32 s;
    explicit Lcg(quint32 seed) : s(seed ? seed : 1) {}
    double next() { s = s * 1664525u + 1013904223u; return (s >> 8) / double(1 << 24); }
    double in(double lo, double hi) { return lo + (hi - lo) * next(); }
};

namespace {

// One contiguous cutting job: rings cut in order at each depth pass. The pass
// direction alternates (in→out, then out→in) so the tool plunges in place at
// each new depth instead of tracking back across the pocket.
struct Job {
    QList<QPolygonF> rings;
    bool closed = true;      // false: open engrave line (retract to repeat)
    QPointF start() const { return rings.first().first(); }
};

struct CutParams {
    double zTop = 0, zBot = 0, stepdown = 1.0;
    double linkDist = 1e9;   // stay-down link limit between rings
    double rampAngle = 0;    // >0: ramp entries on closed loops (degrees)
    double tabTop = -1e9;    // cutout tabs: lift to this Z inside a tab
    double tabWidth = 0;     // tab span along the loop (0 = no tabs)
    // Optional veto for a stay-down link (3D roughing checks it against the
    // relief); unset = any link within linkDist is taken.
    std::function<bool(const QPointF &, const QPointF &)> linkOk;
};

// Ring-fill a centre region: concentric insets every `stepover`, innermost
// first, as one job (a firstDelta of 0 starts on the region's own boundary —
// used for rest regions, already in centre space).
static void ringFillJob(const QPainterPath &area, double firstDelta, double stepover,
                        QList<Job> &jobs)
{
    Job job;
    QList<QList<QPolygonF>> shells;
    double delta = firstDelta;
    while (shells.size() < 500) {
        QList<QPolygonF> s;
        if (delta < 1e-9) {
            for (QPolygonF p : area.toSubpathPolygons())
                if (p.size() > 2) {
                    closeLoop(p);
                    s.append(p);
                }
        } else {
            s = insetRings(area, delta);
        }
        if (s.isEmpty())
            break;
        shells.append(s);
        delta += stepover;
    }
    for (int k = shells.size() - 1; k >= 0; --k)
        job.rings.append(shells.at(k));
    if (!job.rings.isEmpty())
        jobs.append(job);
}

class Emitter
{
public:
    Emitter(QVector<Op> &ops, double safeZ, double feed, double plunge)
        : m_ops(ops), m_safeZ(safeZ), m_feed(feed), m_plunge(plunge) {}

    QPointF pos() const { return QPointF(m_x, m_y); }
    void setFeeds(double feed, double plunge) { m_feed = feed; m_plunge = plunge; }

    void rapidTo(const QPointF &p)
    {
        m_ops.append(Op::rapid(p.x(), p.y(), m_safeZ));
        m_x = p.x();
        m_y = p.y();
    }

    void plungeTo(const QPointF &p, double z)
    {
        m_ops.append(Op::feedTo(p.x(), p.y(), z, m_plunge));
        m_x = p.x();
        m_y = p.y();
    }

    // One feed move with an explicit Z — v-carve chains vary depth per vertex.
    void feedAt(const QPointF &p, double z, double f)
    {
        m_ops.append(Op::feedTo(p.x(), p.y(), z, f));
        m_x = p.x();
        m_y = p.y();
    }

    // Descend along the ring at rampAngle instead of a straight plunge,
    // walking the loop (multiple laps if needed) until z is reached.
    void rampDescend(const QPolygonF &ring, double zFrom, double zTo, double angleDeg)
    {
        const double slope = qTan(qDegreesToRadians(qBound(1.0, angleDeg, 45.0)));
        double z = zFrom;
        int lap = 0;
        while (z > zTo + 1e-9 && lap < 20) {
            for (int i = 1; i < ring.size() && z > zTo + 1e-9; ++i) {
                const double L = QLineF(ring.at(i - 1), ring.at(i)).length();
                z = qMax(zTo, z - L * slope);
                m_ops.append(Op::feedTo(ring.at(i).x(), ring.at(i).y(), z, m_plunge));
                m_x = ring.at(i).x();
                m_y = ring.at(i).y();
            }
            ++lap;
        }
        // Return to the loop start at depth so the full pass begins cleanly.
        if (QLineF(pos(), ring.first()).length() > 1e-6)
            followSpan(ring, 0, ring.size() - 1, zTo);
    }

    // Follow ring vertices from index a to b (inclusive) at depth z.
    void followSpan(const QPolygonF &ring, int a, int b, double z)
    {
        for (int i = a; i <= b; ++i) {
            m_ops.append(Op::feedTo(ring.at(i).x(), ring.at(i).y(), z, m_feed));
            m_x = ring.at(i).x();
            m_y = ring.at(i).y();
        }
    }

    // Follow the polyline from its first vertex (== current XY) with greedy
    // line/arc fitting: long collinear runs collapse to one G1, circular runs
    // to G2/G3, within 0.01 mm of every original vertex.
    void followFitted(const QPolygonF &poly, double z)
    {
        const QVector<FitSeg> segs = fitPolyline(poly, 0.01);
        for (const FitSeg &s : segs) {
            const QPointF &e = poly.at(s.end);
            if (s.arc)
                m_ops.append(Op::arcTo(e.x(), e.y(), z,
                                       s.c.x() - m_x, s.c.y() - m_y, s.cw, m_feed));
            else
                m_ops.append(Op::feedTo(e.x(), e.y(), z, m_feed));
            m_x = e.x();
            m_y = e.y();
        }
    }

    // Follow one closed/open ring at depth z; arcs for circles; tab lifts when
    // z is below tabTop and tabWidth > 0.
    void followRing(const QPolygonF &poly, double z, const CutParams &cp)
    {
        const bool tabs = cp.tabWidth > 0.01 && z < cp.tabTop - 1e-9
                          && poly.isClosed() && poly.size() > 3;
        if (!tabs) {
            QPointF c;
            double r = 0;
            bool ccw = true;
            if (asCircle(poly, &c, &r, &ccw)) {
                const QPointF s = poly.first();
                const QPointF o(2 * c.x() - s.x(), 2 * c.y() - s.y());
                m_ops.append(Op::arcTo(o.x(), o.y(), z, c.x() - s.x(), c.y() - s.y(), !ccw, m_feed));
                m_ops.append(Op::arcTo(s.x(), s.y(), z, c.x() - o.x(), c.y() - o.y(), !ccw, m_feed));
                m_x = s.x();
                m_y = s.y();
                return;
            }
            followFitted(poly, z);
            return;
        }

        // Tabbed pass: evenly spaced tabs, each cp.tabWidth long, crossed at
        // cp.tabTop instead of z.
        const double len = perimeter(poly);
        const int nTabs = qBound(3, int(len / 150.0) + 3, 8);
        auto inTab = [&](double s) {
            for (int t = 0; t < nTabs; ++t) {
                const double c = len * (t + 0.5) / nTabs;
                if (qAbs(s - c) <= cp.tabWidth / 2
                    || qAbs(s - c + len) <= cp.tabWidth / 2
                    || qAbs(s - c - len) <= cp.tabWidth / 2)
                    return true;
            }
            return false;
        };
        double s = 0;
        const double step = 1.0;   // 1 mm resolution along the loop
        bool up = false;
        for (int i = 1; i < poly.size(); ++i) {
            const QLineF seg(poly.at(i - 1), poly.at(i));
            const double L = seg.length();
            double done = 0;
            while (done < L - 1e-9) {
                const double d = qMin(step, L - done);
                done += d;
                s += d;
                const QPointF p = seg.pointAt(done / L);
                const bool tab = inTab(s);
                if (tab != up) {
                    up = tab;
                    m_ops.append(Op::feedTo(m_x, m_y, up ? cp.tabTop : z,
                                            up ? m_feed : m_plunge));
                }
                m_ops.append(Op::feedTo(p.x(), p.y(), up ? cp.tabTop : z, m_feed));
                m_x = p.x();
                m_y = p.y();
            }
        }
        if (up)
            m_ops.append(Op::feedTo(m_x, m_y, z, m_plunge));
    }

    void retract()
    {
        m_ops.append(Op::rapid(m_x, m_y, m_safeZ));
    }

    void runJob(const Job &job, const CutParams &cp)
    {
        rapidTo(job.start());
        double z = cp.zTop;
        bool first = true;
        bool more = true;
        bool forward = true;
        while (more) {
            z = qMax(cp.zBot, z - cp.stepdown);
            more = z > cp.zBot + 1e-9;

            // Ring order alternates per pass so each new pass starts where the
            // last one ended — plunge in place, no track-back across the pocket.
            QList<int> order;
            for (int k = 0; k < job.rings.size(); ++k)
                order.append(forward ? k : job.rings.size() - 1 - k);

            const QPolygonF &firstRing = job.rings.at(order.first());
            if (first) {
                if (cp.rampAngle > 0 && job.closed) {
                    plungeTo(firstRing.first(), cp.zTop);
                    rampDescend(firstRing, cp.zTop, z, cp.rampAngle);
                } else {
                    plungeTo(firstRing.first(), z);
                }
                first = false;
            } else if (job.closed) {
                if (cp.rampAngle > 0)
                    rampDescend(firstRing, z + cp.stepdown, z, cp.rampAngle);
                else
                    plungeTo(pos(), z);   // in place — we ended here last pass
            } else {
                retract();
                rapidTo(job.start());
                plungeTo(job.start(), z);
            }

            for (int oi = 0; oi < order.size(); ++oi) {
                const QPolygonF &ring = job.rings.at(order.at(oi));
                const QPointF rs = ring.first();
                if (QLineF(pos(), rs).length() > 1e-6) {
                    if (QLineF(pos(), rs).length() <= cp.linkDist
                        && (!cp.linkOk || cp.linkOk(pos(), rs))) {
                        m_ops.append(Op::feedTo(rs.x(), rs.y(), z, m_feed));
                        m_x = rs.x();
                        m_y = rs.y();
                    } else {
                        retract();
                        rapidTo(rs);
                        plungeTo(rs, z);
                    }
                }
                followRing(ring, z, cp);
            }
            forward = !forward;
        }
        retract();
    }

private:
    QVector<Op> &m_ops;
    double m_safeZ, m_feed, m_plunge;
    double m_x = 0, m_y = 0;
};

// Greedy nearest-neighbor ordering of jobs to keep rapids short.
QList<Job> orderJobs(QList<Job> jobs, QPointF from)
{
    QList<Job> out;
    while (!jobs.isEmpty()) {
        int best = 0;
        double bestD = QLineF(from, jobs.first().start()).length();
        for (int i = 1; i < jobs.size(); ++i) {
            const double d = QLineF(from, jobs.at(i).start()).length();
            if (d < bestD) {
                bestD = d;
                best = i;
            }
        }
        from = jobs.at(best).start();
        out.append(jobs.takeAt(best));
    }
    return out;
}

static QRectF jobBounds(const Job &j)
{
    QRectF r;
    for (const QPolygonF &p : j.rings)
        r |= p.boundingRect();
    return r;
}

// A ring job held back so it can be interleaved with the jobs of neighbouring
// toolpaths that use the same tool (e.g. counterbore + through-hole).
struct Deferred {
    Job job;
    CutParams cp;
    int tp = 0;          // toolpath sequence number (document order)
    QString name;
    double feed = 0, plunge = 0;
    int rpm = 0;
};

// Nearest-neighbor over all deferred jobs, with one rule: a job may not run
// while an earlier toolpath still has an overlapping job pending. So at a
// given hole the counterbore always precedes the through-hole, a pocket
// precedes the cutout around it — but all work at one site finishes before
// the spindle travels to the next.
static QList<Deferred> orderDeferred(QList<Deferred> jobs, QPointF from)
{
    QList<Deferred> out;
    while (!jobs.isEmpty()) {
        int best = 0;
        double bestD = QLineF(from, jobs.first().job.start()).length();
        for (int i = 1; i < jobs.size(); ++i) {
            const double d = QLineF(from, jobs.at(i).job.start()).length();
            if (d < bestD) {
                bestD = d;
                best = i;
            }
        }
        for (bool again = true; again;) {
            again = false;
            const QRectF b = jobBounds(jobs.at(best).job);
            int pick = -1;
            double pickD = 0;
            for (int i = 0; i < jobs.size(); ++i) {
                if (jobs.at(i).tp >= jobs.at(best).tp
                    || !jobBounds(jobs.at(i).job).intersects(b))
                    continue;
                const double d = QLineF(from, jobs.at(i).job.start()).length();
                if (pick < 0 || d < pickD) {
                    pick = i;
                    pickD = d;
                }
            }
            if (pick >= 0) {
                best = pick;
                again = true;
            }
        }
        from = jobs.at(best).job.start();
        out.append(jobs.takeAt(best));
    }
    return out;
}

} // namespace

GcodeResult exportGcode(Document &doc)
{
    GcodeResult res;
    QVector<Op> ops;
    const double safeZ = doc.params().value("retract", "2.54").toDouble();
    QPointF lastPos(0, 0);

    int lastTool = -1;
    int tpIndex = 0;

    // Ring-type toolpaths (pocket / contour / cutout) are not emitted one by
    // one: consecutive ones sharing a tool are pooled and emitted together,
    // ordered by orderDeferred(), so each site is finished before moving on.
    QList<Deferred> pending;
    int pendingTool = -1;
    auto flush = [&]() {
        if (pending.isEmpty())
            return;
        QVector<Op> body;
        Emitter em(body, safeZ, 0, 0);
        int curTp = -1, curRpm = -1;
        QStringList names;
        for (const Deferred &d : orderDeferred(pending, lastPos)) {
            if (d.tp != curTp) {
                body.append(Op::comment(d.name));
                if (d.rpm != curRpm) {
                    body.append(Op::spindle(d.rpm));
                    curRpm = d.rpm;
                }
                em.setFeeds(d.feed, d.plunge);
                curTp = d.tp;
                if (!names.contains(d.name))
                    names << d.name;
            }
            em.runJob(d.job, d.cp);
            lastPos = em.pos();
        }
        if (pendingTool != lastTool) {
            ops.append(Op::tool(pendingTool));
            lastTool = pendingTool;
        }
        ops.append(body);
        ops.append(Op::spindle(0));
        res.done << names;
        pending.clear();
        pendingTool = -1;
    };

    for (const Toolpath &t : doc.toolpaths()) {
        const QJsonObject j = t.json;
        if (!j.value("enabled").toBool(true))
            continue;
        const QString name = j.value("name").toString();
        const int myIndex = tpIndex++;

        const bool contour = (t.type == QLatin1String("contour"));
        const bool pocket = (t.type == QLatin1String("pocket_toolpath"));
        const bool cutout = (t.type == QLatin1String("cutout"));
        const bool drilling = (t.type == QLatin1String("drilling_toolpath"));
        const bool keyhole = (t.type == QLatin1String("keyhole_toolpath"));
        const bool texture = (t.type == QLatin1String("texture_toolpath"));
        const bool vcarve = (t.type == QLatin1String("advanced_vcarve_toolpath"));
        const bool engrave = (t.type == QLatin1String("engrave_toolpath"));
        const bool rough3d = (t.type == QLatin1String("3d_rough_toolpath"));
        const bool finish3d = (t.type == QLatin1String("3d_finish_toolpath"));
        if (!contour && !pocket && !cutout && !drilling && !keyhole && !texture
            && !vcarve && !engrave && !rough3d && !finish3d) {
            res.skipped << QStringLiteral("%1 (%2 not supported yet)")
                               .arg(name, t.type);
            continue;
        }

        const QJsonObject speeds = j.value("speeds").toObject();
        const double feed = speeds.value("feedrate").toDouble(500);
        const double plunge = speeds.value("plungerate").toDouble(100);
        const int rpm = int(speeds.value("rpm").toDouble(10000));
        const QJsonObject tool = j.value("tool").toObject();
        const int toolNo = int(tool.value("number").toDouble(0));
        const double toolR = tool.value("diameter").toDouble(6) / 2.0;

        CutParams cp;
        cp.zTop = depthToZ(j.value("start_depth"));
        if (cutout) {
            cp.zBot = depthToZ(j.value("cut_depth"))
                      + depthToZ(j.value("break_through"));
            cp.stepdown = qMax(0.05, j.value("depth_per_pass").toDouble(1.0));
            if (!j.value("ignore_tabs").toBool(true)
                && j.value("tab_height").toDouble() > 0.01) {
                cp.tabTop = cp.zBot + j.value("tab_height").toDouble();
                cp.tabWidth = qBound(1.0, j.value("tab_width").toDouble(6.0), 30.0);
            }
        } else {
            cp.zBot = depthToZ(j.value("end_depth"));
            cp.stepdown = qMax(0.05, j.value("stepdown").toDouble(1.0));
        }
        if (j.value("enable_ramping").toBool(false))
            cp.rampAngle = qBound(1.0, j.value("ramp_angle").toDouble(20.0), 45.0);

        if (rough3d || finish3d) {
            // 3D toolpaths over the modeller's relief (cam3d.cpp). Emitted on
            // their own, never pooled with the 2D ring jobs around them.
            const HeightModel *hm = heightModelFor(doc);
            if (!hm || !hm->valid()) {
                res.skipped << QStringLiteral("%1 (no 3D model)").arg(name);
                continue;
            }
            const Cam3dParams p3 = cam3dParams(j, safeZ);
            const QVector<const Element *> belems = referenced(doc, j);
            const QPainterPath boundary =
                cam3dBoundary(*hm, belems.isEmpty() ? QPainterPath() : regionOf(belems),
                              p3.boundaryOffset);
            flush();
            QVector<Op> body;
            if (rough3d) {
                // Z-level clearing: each level's region is pocketed with the
                // 2D ring machinery at that depth (rings inset by the tool
                // radius + stock to leave, stay-down links checked against
                // the relief).
                Emitter em(body, safeZ, feed, plunge);
                const double r = p3.tool.radius();
                for (const RoughLevel &lv : roughLevels(*hm, p3, boundary)) {
                    QList<Job> jobs;
                    for (const QPainterPath &comp : components(lv.region))
                        ringFillJob(comp, r + p3.stockToLeave, p3.stepover, jobs);
                    CutParams lcp = cp;
                    lcp.zTop = lv.z + p3.stepdown;
                    lcp.zBot = lv.z;
                    lcp.stepdown = p3.stepdown;
                    lcp.linkDist = qMax(2.5 * p3.stepover, 4 * r);
                    lcp.linkOk = [&, z = lv.z](const QPointF &a, const QPointF &b) {
                        return roughLinkClear(*hm, p3, a, b, z);
                    };
                    for (const Job &job : orderJobs(jobs, em.pos()))
                        em.runJob(job, lcp);
                }
                lastPos = em.pos();
            } else {
                body = finishOps(*hm, p3, boundary);
                for (int k = body.size() - 1; k >= 0; --k)
                    if (body.at(k).kind == Op::Feed) {
                        lastPos = QPointF(body.at(k).x, body.at(k).y);
                        break;
                    }
            }
            if (body.isEmpty()) {
                res.skipped << QStringLiteral("%1 (nothing to cut)").arg(name);
                continue;
            }
            if (toolNo != lastTool) {
                ops.append(Op::tool(toolNo));
                lastTool = toolNo;
            }
            ops.append(Op::comment(name));
            ops.append(Op::spindle(rpm));
            ops.append(body);
            ops.append(Op::spindle(0));
            res.done << name;
            continue;
        }

        const QVector<const Element *> elems = referenced(doc, j);
        if (elems.isEmpty()) {
            res.skipped << QStringLiteral("%1 (no vectors)").arg(name);
            continue;
        }

        QVector<Op> body;
        Emitter em(body, safeZ, feed, plunge);

        QList<Job> jobs;
        if (drilling || keyhole) {
            const double peck = j.value("peck_distance").toDouble(0);
            const double slotLen = j.value("length").toDouble(12.7);
            const double slotAng = qDegreesToRadians(j.value("angle").toDouble(90));
            QList<Job> pts;
            for (const Element *e : elems) {
                const QJsonArray c = e->raw.value("center").toArray();
                const QPointF ctr = c.size() == 2
                    ? QPointF(c.at(0).toDouble(), c.at(1).toDouble())
                    : e->painterPath.boundingRect().center();
                Job job;
                job.rings.append(QPolygonF() << ctr);
                pts.append(job);
            }
            for (const Job &h : orderJobs(pts, lastPos)) {
                const QPointF ctr = h.start();
                em.rapidTo(ctr);
                if (drilling && peck > 0.01) {
                    double z = cp.zTop;
                    while (z > cp.zBot + 1e-9) {
                        z = qMax(cp.zBot, z - peck);
                        em.plungeTo(ctr, z);
                        em.retract();
                    }
                } else {
                    em.plungeTo(ctr, cp.zBot);
                    if (keyhole) {
                        // Slide the keyhole bit out and back at depth.
                        const QPointF end = ctr + slotLen
                            * QPointF(qCos(slotAng), qSin(slotAng));
                        body.append(Op::feedTo(end.x(), end.y(), cp.zBot, feed));
                        body.append(Op::feedTo(ctr.x(), ctr.y(), cp.zBot, feed));
                    }
                    em.retract();
                }
                lastPos = ctr;
            }
        } else if (texture) {
            // Decorative strokes: hatch lines at `angle`, clipped to the
            // region, broken into random-length strokes at random depths.
            const QPainterPath region = regionOf(elems);
            const QRectF bb = region.boundingRect();
            const double ang = qDegreesToRadians(j.value("angle").toDouble(0));
            const QPointF dir(qCos(ang), qSin(ang));
            const QPointF nrm(-dir.y(), dir.x());
            const double stepover = qMax(0.2, j.value("stepover").toDouble(3));
            const double sVar = qBound(0.0, j.value("stepover_variation").toDouble(0), 1.0);
            const double minL = qMax(1.0, j.value("min_length").toDouble(15));
            const double maxL = qMax(minL, j.value("max_length").toDouble(30));
            const double zMin = depthToZ(j.value("min_depth"));
            const double zMax = depthToZ(j.value("max_depth"));
            Lcg rng(qHash(t.uuid));

            const double diag = QLineF(bb.topLeft(), bb.bottomRight()).length();
            const QPointF mid = bb.center();
            int guard = 0;
            for (double off = -diag / 2; off < diag / 2 && guard < 20000; off +=
                 stepover * (1.0 + sVar * (rng.next() - 0.5))) {
                const QPointF base = mid + nrm * off;
                // walk the hatch line, extracting in-region runs
                double runStart = -1;
                for (double s = -diag / 2; s <= diag / 2 + 0.5; s += 0.5) {
                    const QPointF p = base + dir * s;
                    const bool in = s <= diag / 2 && region.contains(p);
                    if (in && runStart < 0)
                        runStart = s;
                    if (!in && runStart >= 0) {
                        double a = runStart;
                        const double runEnd = s - 0.5;
                        runStart = -1;
                        while (a < runEnd - 1.0 && guard < 20000) {
                            ++guard;
                            const double L =
                                qMin(runEnd - a, rng.in(minL, maxL));
                            const QPointF sp = base + dir * a;
                            const QPointF ep = base + dir * (a + L);
                            const double z = rng.in(qMin(zMin, zMax), qMax(zMin, zMax));
                            em.rapidTo(sp);
                            em.plungeTo(sp, z);
                            body.append(Op::feedTo(ep.x(), ep.y(), z, feed));
                            em.retract();
                            a += qMax(1.0, L * 0.7 + rng.in(0, L * 0.3));
                        }
                    }
                }
            }
        } else if (vcarve) {
            const double bitAngle = tool.value("angle").toDouble(60);
            const double halfTan = qTan(qDegreesToRadians(qBound(10.0, bitAngle, 179.0) / 2));
            const double stepover = qMax(0.05, j.value("stepover").toDouble(0.2));
            const QPainterPath region = regionOf(elems);   // fine; spurs are pruned in medialAxis()
            // Max clearance the bit can reach before bottoming out at zBot;
            // anything wider needs a flat clearing pass at full depth.
            const double dMax = qMax(0.0, (cp.zTop - cp.zBot) * halfTan);
            // Carve one component (letter/shape) completely before the next.
            QList<QPainterPath> comps = components(region);
            std::sort(comps.begin(), comps.end(),
                      [](const QPainterPath &a, const QPainterPath &b) {
                          const QPointF ca = a.boundingRect().center();
                          const QPointF cb = b.boundingRect().center();
                          return ca.x() + ca.y() * 0.01 < cb.x() + cb.y() * 0.01;
                      });
            const CutParams flat;   // single full-depth lap per ring
            for (const QPainterPath &comp : comps) {
                bool medialDone = false;
                if (medialAxisAvailable()) {
                    // True v-carve: ride the medial axis, the bit tip sinking
                    // to clearance/tan(halfAngle) so the cone kisses both
                    // walls — sharp corners come from the spurs, where the
                    // clearance (and the bit) runs out to zero.
                    QVector<QVector<VPoint>> pend =
                        medialAxis(comp.toSubpathPolygons());
                    medialDone = !pend.isEmpty();
                    auto zOf = [&](const VPoint &p) {
                        return qMax(cp.zBot, cp.zTop - p.d / halfTan);
                    };
                    QPointF cur = em.pos();
                    while (!pend.isEmpty()) {
                        int best = 0;
                        bool rev = false;
                        double bd = 1e18;
                        for (int i = 0; i < pend.size(); ++i) {
                            const VPoint &f0 = pend.at(i).first();
                            const VPoint &l0 = pend.at(i).last();
                            const double d0 = QLineF(cur, QPointF(f0.x, f0.y)).length();
                            const double d1 = QLineF(cur, QPointF(l0.x, l0.y)).length();
                            if (d0 < bd) { bd = d0; best = i; rev = false; }
                            if (d1 < bd) { bd = d1; best = i; rev = true; }
                        }
                        QVector<VPoint> ch = pend.takeAt(best);
                        if (rev)
                            std::reverse(ch.begin(), ch.end());
                        const QPointF s(ch.first().x, ch.first().y);
                        em.rapidTo(s);
                        em.plungeTo(s, zOf(ch.first()));
                        for (int k = 1; k < ch.size(); ++k)
                            em.feedAt(QPointF(ch.at(k).x, ch.at(k).y),
                                      zOf(ch.at(k)), feed);
                        em.retract();
                        cur = em.pos();
                    }
                    // Flat-bottom clearing where the shape is wider than the
                    // bit's reach at full depth.
                    if (medialDone && dMax > 1e-9) {
                        double delta = dMax;
                        int guard = 0;
                        while (guard++ < 2000) {
                            const QList<QPolygonF> rings = insetRings(comp, delta);
                            if (rings.isEmpty())
                                break;
                            for (const QPolygonF &ring : rings) {
                                em.rapidTo(ring.first());
                                em.plungeTo(ring.first(), cp.zBot);
                                em.followRing(ring, cp.zBot, flat);
                                em.retract();
                            }
                            delta += stepover;
                        }
                    }
                }
                if (!medialDone) {
                    // Fallback (no Voronoi backend): depth-graded inset rings.
                    if (j.value("pocket_enabled").toBool(false))
                        res.skipped << QStringLiteral("%1 (flat pocket pass not emitted)")
                                           .arg(name);
                    double delta = stepover;
                    int guard = 0;
                    while (guard++ < 2000) {
                        const QList<QPolygonF> rings = insetRings(comp, delta);
                        if (rings.isEmpty())
                            break;
                        const double z = qMax(cp.zBot, cp.zTop - delta / halfTan);
                        for (const QPolygonF &ring : rings) {
                            em.rapidTo(ring.first());
                            em.plungeTo(ring.first(), z);
                            em.followRing(ring, z, flat);
                            em.retract();
                        }
                        if (z <= cp.zBot + 1e-9)
                            break;   // walls at full depth
                        delta += stepover;
                    }
                }
                lastPos = em.pos();
            }
        } else if (engrave) {
            // ENGRAVE (phobiCCCP type, see toolpathfactory.cpp). mode
            // "outline": trace every vector exactly, no offset. "fill": hatch
            // the closed regions at line_spacing / angle, clipped to the
            // region inset by the tool radius (for a V/engraving cutter the
            // radius it actually has at the cut depth), then an outline pass
            // on that inset boundary. "both": fill plus the exact trace.
            const QString mode = j.value("mode").toString(QStringLiteral("outline"));
            const bool fill = mode == QLatin1String("fill") || mode == QLatin1String("both");
            const bool trace = mode != QLatin1String("fill");
            cp.stepdown = qMax(0.02, j.value("stepdown").toDouble(cp.zTop - cp.zBot));
            QVector<const Element *> closedEls;
            for (const Element *e : elems) {
                bool anyClosed = false;
                for (const QPolygonF &p : finePolygons(e->painterPath)) {
                    const bool closed = p.isClosed() && p.size() > 3;
                    anyClosed = anyClosed || closed;
                    // Open vectors are traced in every mode (a fill has
                    // nothing to hatch there); closed ones only when tracing.
                    if (p.size() >= 2 && (trace || !closed)) {
                        Job job;
                        job.rings.append(p);
                        job.closed = closed;
                        jobs.append(job);
                    }
                }
                if (anyClosed)
                    closedEls.append(e);
            }
            if (fill && !closedEls.isEmpty()) {
                const double bitAngle = tool.value("angle").toDouble(0);
                double effR = toolR;
                if (bitAngle > 0)
                    effR = qMin(toolR, (cp.zTop - cp.zBot)
                                           * qTan(qDegreesToRadians(qBound(1.0, bitAngle, 179.0) / 2)));
                effR = qMax(0.01, effR);
                const double spacing = qMax(0.05, j.value("line_spacing").toDouble(1.0));
                const double angle = j.value("angle").toDouble(45);
                const QList<QPolygonF> inset = insetRings(regionOf(closedEls), effR);
                QList<QPolygonF> lines = hatchRings(inset, spacing, angle);
                if (j.value("crosshatch").toBool(false))
                    lines += hatchRings(inset, spacing, angle + 90);
                for (const QPolygonF &p : lines) {
                    Job job;
                    job.rings.append(p);
                    job.closed = false;
                    jobs.append(job);
                }
                for (const QPolygonF &p : inset) {   // final clean-up outline
                    Job job;
                    job.rings.append(p);
                    jobs.append(job);
                }
            }
        } else if (contour && j.value("ofset_dir").toInt(0) == 0) {
            for (const Element *e : elems)
                for (const QPolygonF &p : finePolygons(e->painterPath))
                    if (p.size() >= 2) {
                        Job job;
                        job.rings.append(p);
                        job.closed = p.isClosed();
                        jobs.append(job);
                    }
        } else if (contour || cutout) {
            const QPainterPath region = regionOf(elems);
            const bool inside = contour ? j.value("ofset_dir").toInt(0) < 0
                                        : j.value("flip_inside_outside").toBool(false);
            // stock_to_leave keeps the cut that much further from the vector
            // on the material side (cutouts never leave stock).
            const double leave = contour ? qMax(0.0, j.value("stock_to_leave").toDouble(0)) : 0.0;
            const QList<QPolygonF> rings = inside ? insetRings(region, toolR + leave)
                                                  : outsetRings(region, toolR + leave);
            for (const QPolygonF &p : rings) {
                Job job;
                job.rings.append(p);
                jobs.append(job);
            }
        } else { // pocket
            const double stepover = qMax(0.1, j.value("stepover").toDouble(toolR));
            cp.linkDist = qMax(2.5 * stepover, 4 * toolR);
            const double leave = qMax(0.0, j.value("stock_to_leave").toDouble(0));
            // Rest machining: with enable_rest and a rest_diameter larger than
            // this tool, only ring-fill what that previous cutter left behind.
            const double prevR = j.value("rest_diameter").toDouble(0) / 2.0;
            const bool useRest = j.value("enable_rest").toBool(false) && prevR > toolR + 1e-6;

            for (const QPainterPath &comp : components(regionOf(elems))) {
                if (useRest) {
                    for (const QPainterPath &rc : restRegions(comp, toolR, prevR, leave))
                        ringFillJob(rc, 0.0, stepover, jobs);
                } else {
                    ringFillJob(comp, toolR + leave, stepover, jobs);
                }
            }
        }

        if (!jobs.isEmpty()) {
            if (pendingTool != toolNo)
                flush();                      // different tool: close the pool
            pendingTool = toolNo;
            for (const Job &job : jobs)
                pending.append({job, cp, myIndex, name, feed, plunge, rpm});
            continue;
        }

        if (body.isEmpty()) {
            res.skipped << QStringLiteral("%1 (empty)").arg(name);
            continue;
        }
        flush();   // direct-emission toolpath: keep document order around it
        if (toolNo != lastTool) {
            ops.append(Op::tool(toolNo));
            lastTool = toolNo;
        }
        ops.append(Op::comment(name));
        ops.append(Op::spindle(rpm));
        ops.append(body);
        ops.append(Op::spindle(0));
        res.done << name;
    }
    flush();

    res.gcode = GrblPost(true).generate(ops);
    res.ops = ops;
    return res;
}

// CC tool `type`: 0 = flat end mill, 1 = ball, 2 = V-bit. Be lenient with
// files whose type is missing or inconsistent: a positive angle means a V-bit,
// a corner radius equal to the tool radius means a ball.
static ToolGeom toolGeomOf(const QJsonObject &tool)
{
    ToolGeom g;
    g.number = int(tool.value("number").toDouble(0));
    g.diameter = tool.value("diameter").toDouble(3.175);
    if (!(g.diameter > 0))
        g.diameter = 3.175;
    g.angle = tool.value("angle").toDouble(0);
    g.cornerRadius = qMax(0.0, tool.value("corner_radius").toDouble(0));
    g.name = tool.value("name").toString();
    const int type = int(tool.value("type").toDouble(0));
    if (type == 2 || (g.angle > 0 && type != 1)) {
        g.kind = ToolGeom::VBit;
        g.angle = qBound(5.0, g.angle > 0 ? g.angle : 60.0, 179.0);
    } else if (type == 1 || g.cornerRadius >= g.radius() - 1e-6) {
        g.kind = ToolGeom::Ball;
        g.cornerRadius = g.radius();
    } else {
        g.kind = ToolGeom::Flat;
        g.cornerRadius = qMin(g.cornerRadius, g.radius());
    }
    return g;
}

ToolGeom toolGeomFromJson(const QJsonObject &tool)
{
    return toolGeomOf(tool);
}

QHash<int, ToolGeom> toolGeometry(const Document &doc)
{
    QHash<int, ToolGeom> table;
    for (const Toolpath &t : doc.toolpaths()) {
        for (const char *key : {"tool", "tool_pocket"}) {
            const QJsonObject tool = t.json.value(QLatin1String(key)).toObject();
            if (tool.isEmpty())
                continue;
            const ToolGeom g = toolGeomOf(tool);
            if (!table.contains(g.number))
                table.insert(g.number, g);
        }
    }
    return table;
}

} // namespace c2d
