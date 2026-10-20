#include "vcarve.h"

#include <algorithm>
#include <cmath>

#include <QHash>
#include <QLineF>
#include <QPainterPath>
#include <QPair>
#include <QPointF>
#include <QSet>
#include <QtMath>

#ifdef HAVE_BOOST_VORONOI
// Boost headers must stay outside namespace c2d.
#include <boost/polygon/voronoi.hpp>

namespace {
struct IPt {
    int x, y;
};
struct ISeg {
    IPt p0, p1;
};
} // namespace

namespace boost {
namespace polygon {
template <> struct geometry_concept<IPt> {
    typedef point_concept type;
};
template <> struct point_traits<IPt> {
    typedef int coordinate_type;
    static int get(const IPt &p, orientation_2d o)
    {
        return o == HORIZONTAL ? p.x : p.y;
    }
};
template <> struct geometry_concept<ISeg> {
    typedef segment_concept type;
};
template <> struct segment_traits<ISeg> {
    typedef int coordinate_type;
    typedef IPt point_type;
    static IPt get(const ISeg &s, direction_1d dir)
    {
        return dir.to_int() ? s.p1 : s.p0;
    }
};
} // namespace polygon
} // namespace boost
#endif // HAVE_BOOST_VORONOI

namespace c2d {

#ifndef HAVE_BOOST_VORONOI

bool medialAxisAvailable() { return false; }
QVector<QVector<VPoint>> medialAxis(const QList<QPolygonF> &) { return {}; }

#else

bool medialAxisAvailable() { return true; }

namespace {

const double kScale = 1000.0;   // mm -> integer micrometres

// Distance from p to the closest boundary segment (mm space).
double distToBoundary(const QPointF &p, const QVector<QLineF> &segs)
{
    double best = 1e18;
    for (const QLineF &s : segs) {
        const QPointF d(s.dx(), s.dy());
        const double len2 = d.x() * d.x() + d.y() * d.y();
        double t = 0;
        if (len2 > 1e-12) {
            t = ((p.x() - s.x1()) * d.x() + (p.y() - s.y1()) * d.y()) / len2;
            t = qBound(0.0, t, 1.0);
        }
        const double dx = p.x() - (s.x1() + d.x() * t);
        const double dy = p.y() - (s.y1() + d.y() * t);
        const double dd = dx * dx + dy * dy;
        if (dd < best)
            best = dd;
    }
    return qSqrt(best);
}

IPt pointSiteOf(const boost::polygon::voronoi_cell<double> &cell,
                const std::vector<ISeg> &segs)
{
    using namespace boost::polygon;
    const ISeg &s = segs[cell.source_index()];
    return cell.source_category() == SOURCE_CATEGORY_SEGMENT_START_POINT ? s.p0
                                                                         : s.p1;
}

bool samePt(const IPt &a, const IPt &b) { return a.x == b.x && a.y == b.y; }

} // namespace

QVector<QVector<VPoint>> medialAxis(const QList<QPolygonF> &rings)
{
    using boost::polygon::voronoi_diagram;

    // Densify the boundary to <= 1 mm pieces: curved Voronoi edges then span
    // so little that their chords are exact to well under a hundredth.
    std::vector<ISeg> isegs;
    QVector<QLineF> mmSegs;
    // Integer coordinates of the *original* polygon vertices. A densification
    // joint is never a corner, but rounding it to integer micrometres makes it
    // look like one: the joint lands up to 0.5 um off the line it was cut
    // from, which over a 1 mm piece is ~1e-3 rad - ten times the collinearity
    // threshold below - so the joint kept its bisector, and the spur pruning
    // could not remove it either, because that only prunes tips sitting on a
    // polygon vertex. A 20 mm square rotated 17 deg came out as 72 chains and
    // 243 mm of medial axis instead of 4 and 56.6, every spurious spur being
    // a full-depth plunge and a drag out to the surface.
    QSet<QPair<int, int>> cornerPts;
    QPainterPath region;
    region.setFillRule(Qt::OddEvenFill);
    for (const QPolygonF &ringIn : rings) {
        QPolygonF ring = ringIn;
        if (ring.size() < 3)
            continue;
        if (ring.first() != ring.last())
            ring.append(ring.first());
        region.addPolygon(ring);
        for (int i = 1; i < ring.size(); ++i) {
            const QPointF a = ring.at(i - 1), b = ring.at(i);
            cornerPts.insert({int(qRound(a.x() * kScale)), int(qRound(a.y() * kScale))});
            const double L = QLineF(a, b).length();
            if (L < 1e-6)
                continue;
            const int pieces = qMax(1, int(std::ceil(L / 1.0)));
            QPointF prev = a;
            for (int k = 1; k <= pieces; ++k) {
                const QPointF q = a + (b - a) * (double(k) / pieces);
                mmSegs.append(QLineF(prev, q));
                isegs.push_back({{int(qRound(prev.x() * kScale)),
                                  int(qRound(prev.y() * kScale))},
                                 {int(qRound(q.x() * kScale)),
                                  int(qRound(q.y() * kScale))}});
                prev = q;
            }
        }
    }
    if (isegs.size() < 3)
        return {};

    voronoi_diagram<double> vd;
    boost::polygon::construct_voronoi(isegs.begin(), isegs.end(), &vd);

    // Collect medial edges: finite primary edges whose midpoint lies inside
    // the region, minus the artifacts of a segment-site boundary:
    //  - an edge between a segment and one of its own endpoints (the
    //    perpendicular fan at segment ends),
    //  - an edge between two point sites that bound the same segment,
    //  - an edge between incident, collinear segments (densification joints).
    // The bisector of incident but genuinely angled segments survives — that
    // is the corner spur the V-bit rides into to cut a sharp point.
    QHash<QPair<qint64, qint64>, int> nodeIndex;
    QVector<QPointF> nodes;      // mm
    QVector<QPair<int, int>> edges;
    // Edges with a point site on either side (a reflex corner): along them
    // the clearance is not linear between the end vertices - it dips
    // towards the bisector's foot, by up to 0.6 mm between two island tips
    // facing each other across a neck - so they are sampled when chained.
    QVector<bool> pointEdge;
    QVector<QPointF> edgeSite;   // that point site (mm): the dip is deepest
                                 // where the edge passes closest to it
    QHash<QPair<int, int>, int> seen;
    auto nodeAt = [&](double xs, double ys) -> int {
        const QPair<qint64, qint64> key(qRound64(xs * 4.0), qRound64(ys * 4.0));
        auto it = nodeIndex.constFind(key);
        if (it != nodeIndex.constEnd())
            return it.value();
        nodes.append(QPointF(xs / kScale, ys / kScale));
        nodeIndex.insert(key, int(nodes.size() - 1));
        return int(nodes.size() - 1);
    };

    for (const auto &edge : vd.edges()) {
        if (!edge.is_finite() || !edge.is_primary())
            continue;
        if (&edge > edge.twin())
            continue;   // each edge once, not its twin
        const auto *c1 = edge.cell();
        const auto *c2 = edge.twin()->cell();
        const bool s1 = c1->contains_segment();
        const bool s2 = c2->contains_segment();
        const bool hasPoint = !s1 || !s2;
        const IPt site = hasPoint ? pointSiteOf(*(s1 ? c2 : c1), isegs) : IPt{0, 0};
        if (s1 != s2) {
            // segment vs point: drop when the point is that segment's own end
            const ISeg &s = isegs[(s1 ? c1 : c2)->source_index()];
            const IPt p = pointSiteOf(*(s1 ? c2 : c1), isegs);
            if (samePt(p, s.p0) || samePt(p, s.p1))
                continue;
        } else if (!s1) {
            // point vs point: drop the bisector of one boundary segment
            const IPt a = pointSiteOf(*c1, isegs);
            const IPt b = pointSiteOf(*c2, isegs);
            bool sameSeg = false;
            for (const ISeg &s : {isegs[c1->source_index()], isegs[c2->source_index()]})
                if ((samePt(a, s.p0) && samePt(b, s.p1))
                    || (samePt(a, s.p1) && samePt(b, s.p0)))
                    sameSeg = true;
            if (sameSeg)
                continue;
        } else {
            // segment vs segment: drop collinear incident pairs
            const ISeg &sa = isegs[c1->source_index()];
            const ISeg &sb = isegs[c2->source_index()];
            IPt shared{0, 0};
            bool touch = false;
            for (const IPt &pa : {sa.p0, sa.p1})
                for (const IPt &pb : {sb.p0, sb.p1})
                    if (samePt(pa, pb)) {
                        shared = pa;
                        touch = true;
                    }
            if (touch) {
                // Not an original vertex: a densification joint, so the two
                // pieces are collinear by construction whatever the rounding
                // says.
                if (!cornerPts.contains({shared.x, shared.y}))
                    continue;
                const IPt oa = samePt(sa.p0, shared) ? sa.p1 : sa.p0;
                const IPt ob = samePt(sb.p0, shared) ? sb.p1 : sb.p0;
                const double cross =
                    double(shared.x - oa.x) * double(ob.y - shared.y)
                    - double(shared.y - oa.y) * double(ob.x - shared.x);
                const double la = std::hypot(double(shared.x - oa.x),
                                             double(shared.y - oa.y));
                const double lb = std::hypot(double(ob.x - shared.x),
                                             double(ob.y - shared.y));
                if (la > 0 && lb > 0 && qAbs(cross) / (la * lb) < 1e-4)
                    continue;
            }
        }

        const double mx = (edge.vertex0()->x() + edge.vertex1()->x()) / 2;
        const double my = (edge.vertex0()->y() + edge.vertex1()->y()) / 2;
        if (!region.contains(QPointF(mx / kScale, my / kScale)))
            continue;

        const int a = nodeAt(edge.vertex0()->x(), edge.vertex0()->y());
        const int b = nodeAt(edge.vertex1()->x(), edge.vertex1()->y());
        if (a == b)
            continue;
        const QPair<int, int> key(qMin(a, b), qMax(a, b));
        const QPointF siteMm(site.x / kScale, site.y / kScale);
        const auto it = seen.constFind(key);
        if (it != seen.constEnd()) {
            if (hasPoint && !pointEdge.at(it.value())) {
                pointEdge[it.value()] = true;
                edgeSite[it.value()] = siteMm;
            }
            continue;
        }
        seen.insert(key, int(edges.size()));
        edges.append({a, b});
        pointEdge.append(hasPoint);
        edgeSite.append(siteMm);
    }
    if (edges.isEmpty())
        return {};

    QVector<QVector<int>> adj(nodes.size());
    for (int i = 0; i < edges.size(); ++i) {
        adj[edges.at(i).first].append(i);
        adj[edges.at(i).second].append(i);
    }
    QVector<bool> used(edges.size(), false);
    QVector<double> clearance(nodes.size(), -1.0);
    auto dOf = [&](int n) {
        if (clearance[n] < 0)
            clearance[n] = distToBoundary(nodes.at(n), mmSegs);
        return clearance[n];
    };

    // Prune flattening spurs. Every polygon vertex sends a Voronoi branch into
    // the region; at a real corner that branch is the spur the bit rides into
    // the point, but on a flattened curve (a circle is a 60-gon here) it is
    // noise — the cone cut from the neighbouring axis already covers it — and
    // a circle would otherwise be carved as sixty spokes. A vertex counts as
    // "curve" when the boundary turns less than kMaxSmoothTurn there.
    {
        const double kMaxSmoothTurn = qDegreesToRadians(20.0);
        struct BVert { QPointF p; double turn; };
        QVector<BVert> bverts;
        for (const QPolygonF &ringIn : rings) {
            QPolygonF ring = ringIn;
            if (ring.size() < 3)
                continue;
            if (ring.first() == ring.last())
                ring.removeLast();
            const int n = ring.size();
            for (int i = 0; i < n; ++i) {
                const QPointF a = ring.at((i + n - 1) % n), b = ring.at(i), c = ring.at((i + 1) % n);
                const QPointF u = b - a, v = c - b;
                const double lu = std::hypot(u.x(), u.y()), lv = std::hypot(v.x(), v.y());
                if (lu < 1e-9 || lv < 1e-9)
                    continue;
                const double cosT = qBound(-1.0, (u.x() * v.x() + u.y() * v.y()) / (lu * lv), 1.0);
                bverts.append({b, std::acos(cosT)});
            }
        }
        auto smoothVertexAt = [&](const QPointF &p) {
            // A tip sits on a boundary vertex (its clearance is ~0); find it.
            for (const BVert &bv : bverts)
                if (qAbs(bv.p.x() - p.x()) < 0.02 && qAbs(bv.p.y() - p.y()) < 0.02)
                    return bv.turn < kMaxSmoothTurn ? 1 : 0;
            return -1;                       // not on a vertex
        };
        QVector<bool> alive(edges.size(), true);
        auto degree = [&](int n) {
            int d = 0;
            for (int e : adj.at(n)) d += alive[e] ? 1 : 0;
            return d;
        };
        auto aliveEdge = [&](int n) {
            for (int e : adj.at(n)) if (alive[e]) return e;
            return -1;
        };
        auto otherEnd = [&](int e, int n) {
            return edges.at(e).first == n ? edges.at(e).second : edges.at(e).first;
        };
        // One pass over the spurs that start on a smooth boundary vertex.
        auto pruneVertexSpurs = [&] {
            bool changed = false;
            for (int n = 0; n < nodes.size(); ++n) {
                if (degree(n) != 1 || smoothVertexAt(nodes.at(n)) != 1)
                    continue;
                // Walk the spur inward until the axis proper (a junction) or a
                // node that is itself a real corner.
                int cur = n;
                while (degree(cur) == 1) {
                    const int e = aliveEdge(cur);
                    alive[e] = false;
                    changed = true;
                    const int next = otherEnd(e, cur);
                    // Stop at the axis proper: a node that was a junction before
                    // pruning (adj holds the original degree) — even if this was
                    // its last spoke, as at the ends of a rounded slot — or a
                    // real corner. Only chain nodes of the spur itself are walked.
                    if (adj.at(next).size() >= 3 || smoothVertexAt(nodes.at(next)) == 0
                        || degree(next) != 1)
                        break;
                    cur = next;
                }
            }
            return changed;
        };
        // On a finely flattened curve the vertex spurs meet in a tree of
        // short junction edges, not at one node (micrometre rounding), and
        // once the spurs are gone its junctions are tips off any vertex: a
        // circle of r = 15 at 0.005 mm came out as 148 chains, each a plunge.
        // A tip whose disc lies inside its neighbour's adds nothing the cone
        // there does not already cut, so it goes too, and so on inward. Each
        // node carries one disc enclosing every disc pruned into it, so the
        // test stays honest along a chain of small steps (the end of an
        // oval's axis is only trimmed by what it really leaves uncut, at most
        // kCover). Tips on a real corner are never touched: that spur is the
        // one the bit rides into the point.
        const double kCover = 0.01;           // mm of cut a prune may give up
        QVector<QPointF> discC(nodes.size());
        QVector<double> discR(nodes.size(), -1.0);
        auto discOf = [&](int n, QPointF *c, double *r) {
            if (discR[n] < 0) {
                discC[n] = nodes.at(n);
                discR[n] = dOf(n);
            }
            *c = discC[n];
            *r = discR[n];
        };
        auto pruneNestedTips = [&] {
            bool changed = false;
            QVector<int> work;
            for (int n = 0; n < nodes.size(); ++n)
                if (degree(n) == 1)
                    work.append(n);
            while (!work.isEmpty()) {
                const int n = work.takeLast();
                if (degree(n) != 1 || smoothVertexAt(nodes.at(n)) == 0)
                    continue;
                const int e = aliveEdge(n);
                const int m = otherEnd(e, n);
                QPointF cn, cm;
                double rn, rm;
                discOf(n, &cn, &rn);
                discOf(m, &cm, &rm);
                if (QLineF(cn, nodes.at(m)).length() + rn > dOf(m) + kCover)
                    continue;
                alive[e] = false;
                changed = true;
                // m's disc grows to enclose n's (still within kCover of m's
                // own clearance, by the test above).
                const double dist = QLineF(cn, cm).length();
                if (dist + rn > rm) {
                    const double R = (dist + rn + rm) / 2;
                    discC[m] = dist > 1e-12 ? cm + (cn - cm) * ((R - rm) / dist) : cm;
                    discR[m] = R;
                }
                if (degree(m) == 1)
                    work.append(m);
            }
            return changed;
        };
        // Each kind of prune exposes tips for the other; run both until
        // neither finds anything.
        for (int pass = 0; pass < 100; ++pass) {
            const bool a = pruneVertexSpurs();
            const bool b = pruneNestedTips();
            if (!a && !b)
                break;
        }
        QVector<QPair<int, int>> kept;
        QVector<bool> keptPoint;
        QVector<QPointF> keptSite;
        for (int e = 0; e < edges.size(); ++e)
            if (alive[e]) {
                kept.append(edges.at(e));
                keptPoint.append(pointEdge.at(e));
                keptSite.append(edgeSite.at(e));
            }
        if (kept.isEmpty()) {
            // Everything was spur (a circle, an ellipse): the axis collapses to
            // its deepest point — one plunge there carves the whole cone.
            int best = 0;
            for (int i = 1; i < nodes.size(); ++i)
                if (dOf(i) > dOf(best))
                    best = i;
            const VPoint v{nodes.at(best).x(), nodes.at(best).y(), dOf(best)};
            return {{v, v}};
        }
        if (kept.size() != edges.size()) {
            edges = kept;
            pointEdge = keptPoint;
            edgeSite = keptSite;
            adj = QVector<QVector<int>>(nodes.size());
            for (int i = 0; i < edges.size(); ++i) {
                adj[edges.at(i).first].append(i);
                adj[edges.at(i).second].append(i);
            }
            used = QVector<bool>(edges.size(), false);
        }
    }

    // Chain the edge soup into polylines: start walks at junctions and tips
    // (degree != 2), then sweep up any remaining pure loops.
    QVector<QVector<VPoint>> chains;
    // Pieces of at most kSample mm along a point-site edge, plus the point
    // nearest the site (the bottom of the dip), each with its own
    // clearance, wherever the straight line between the ends would let the
    // bit sink deeper than the walls allow (by more than kDip).
    const double kSample = 0.2, kDip = 0.002;
    auto sampleEdge = [&](int e, int from, int to, QVector<VPoint> &ch) {
        const QPointF a = nodes.at(from), b = nodes.at(to), ab = b - a;
        const double len2 = ab.x() * ab.x() + ab.y() * ab.y();
        if (len2 < 1e-12)
            return;
        const int pieces = int(std::ceil(std::sqrt(len2) / kSample));
        QVector<double> ts;
        for (int k = 1; k < pieces; ++k)
            ts.append(double(k) / pieces);
        const QPointF site = edgeSite.at(e);
        const double tFoot = ((site.x() - a.x()) * ab.x() + (site.y() - a.y()) * ab.y()) / len2;
        if (tFoot > 1e-3 && tFoot < 1 - 1e-3) {
            ts.append(tFoot);
            std::sort(ts.begin(), ts.end());
        }
        QVector<VPoint> mid;
        bool dips = false;
        for (const double t : ts) {
            const QPointF q = a + ab * t;
            const double d = distToBoundary(q, mmSegs);
            dips = dips || dOf(from) + (dOf(to) - dOf(from)) * t - d > kDip;
            mid.append({q.x(), q.y(), d});
        }
        if (dips)
            ch += mid;
    };
    auto walk = [&](int startNode, int startEdge) {
        QVector<VPoint> ch;
        int n = startNode;
        int e = startEdge;
        ch.append({nodes.at(n).x(), nodes.at(n).y(), dOf(n)});
        while (true) {
            used[e] = true;
            const int from = n;
            n = edges.at(e).first == n ? edges.at(e).second : edges.at(e).first;
            if (pointEdge.at(e))
                sampleEdge(e, from, n, ch);
            ch.append({nodes.at(n).x(), nodes.at(n).y(), dOf(n)});
            if (adj.at(n).size() != 2)
                break;
            const int e0 = adj.at(n).at(0), e1 = adj.at(n).at(1);
            e = (e0 == e ? e1 : e0);
            if (used[e])
                break;
        }
        if (ch.size() >= 2)
            chains.append(ch);
    };
    for (int n = 0; n < nodes.size(); ++n) {
        if (adj.at(n).size() == 2)
            continue;
        for (int e : adj.at(n))
            if (!used[e])
                walk(n, e);
    }
    for (int e = 0; e < edges.size(); ++e)
        if (!used[e])
            walk(edges.at(e).first, e);   // leftover loops
    return chains;
}

#endif // HAVE_BOOST_VORONOI

} // namespace c2d
