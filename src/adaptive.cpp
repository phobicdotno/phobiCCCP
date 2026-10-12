#include "adaptive.h"

#include <QLineF>
#include <QtGlobal>
#include <QRectF>
#include <algorithm>
#include <cmath>
#include <vector>

#ifdef HAVE_CLIPPER2
#include <clipper2/clipper.h>
namespace C2 = Clipper2Lib;
#endif

namespace c2d {
namespace adaptive {

#ifdef HAVE_CLIPPER2

namespace {

const double kScale = 1000.0;   // integer microns

C2::Path64 toPath(const QPolygonF &poly)
{
    C2::Path64 p;
    p.reserve(poly.size());
    for (const QPointF &q : poly)
        p.push_back(C2::Point64(std::llround(q.x() * kScale), std::llround(q.y() * kScale)));
    if (p.size() > 1 && p.front() == p.back())
        p.pop_back();
    return p;
}

QPolygonF fromPath(const C2::Path64 &p)
{
    QPolygonF out;
    out.reserve(int(p.size()) + 1);
    for (const C2::Point64 &q : p)
        out << QPointF(q.x / kScale, q.y / kScale);
    if (!out.isEmpty())
        out << out.first();
    return out;
}

C2::Paths64 inflate(const C2::Paths64 &paths, double mm)
{
    if (paths.empty())
        return paths;
    // 10 micron arc tolerance, then the same again as a simplification:
    // these shapes are re-inflated pass after pass and would otherwise
    // gather vertices without end.
    return C2::SimplifyPaths(C2::InflatePaths(paths, mm * kScale, C2::JoinType::Round,
                                              C2::EndType::Polygon, 2.0, 10.0),
                             10.0);
}

double area(const C2::Paths64 &paths)
{
    double a = 0;
    for (const C2::Path64 &p : paths)
        a += C2::Area(p);
    return std::fabs(a) / (kScale * kScale);
}

double shoelace(const QPolygonF &r)
{
    double a = 0;
    for (int i = 0; i + 1 < r.size(); ++i)
        a += r.at(i).x() * r.at(i + 1).y() - r.at(i + 1).x() * r.at(i).y();
    return a / 2;
}

// The rings of a region, oriented for climb milling with a clockwise
// spindle: outer boundaries counter-clockwise (material outside them), hole
// boundaries clockwise (material inside them) -- reversed for conventional.
void orientWalk(const C2::PolyPath64 &node, bool climb, QVector<QPolygonF> &out)
{
    QPolygonF r = fromPath(node.Polygon());
    if (r.size() > 3) {
        const bool wantCcw = !node.IsHole() == climb;
        if ((shoelace(r) > 0) != wantCcw)
            std::reverse(r.begin(), r.end());
        out.append(r);
    }
    for (const auto &ch : node)
        orientWalk(*ch, climb, out);
}

QVector<QPolygonF> orientedRings(const C2::Paths64 &region, bool climb)
{
    C2::Clipper64 c;
    c.AddSubject(region);
    C2::PolyTree64 tree;
    c.Execute(C2::ClipType::Union, C2::FillRule::NonZero, tree);
    QVector<QPolygonF> out;
    for (const auto &ch : tree)
        orientWalk(*ch, climb, out);
    return out;
}

// Connected parts: each outer boundary with its holes.
QVector<C2::Paths64> parts(const C2::Paths64 &region)
{
    C2::Clipper64 c;
    c.AddSubject(region);
    C2::PolyTree64 tree;
    c.Execute(C2::ClipType::Union, C2::FillRule::NonZero, tree);
    QVector<C2::Paths64> out;
    std::function<void(const C2::PolyPath64 &)> walk = [&](const C2::PolyPath64 &outer) {
        C2::Paths64 part{outer.Polygon()};
        for (const auto &hole : outer) {
            part.push_back(hole->Polygon());
            for (const auto &island : *hole)
                walk(*island);
        }
        out.append(part);
    };
    for (const auto &ch : tree)
        walk(*ch);
    return out;
}

bool inside(const C2::Paths64 &region, const QPointF &q)
{
    const C2::Point64 pt(std::llround(q.x() * kScale), std::llround(q.y() * kScale));
    int crossings = 0;
    for (const C2::Path64 &p : region) {
        const C2::PointInPolygonResult r = C2::PointInPolygon(pt, p);
        if (r == C2::PointInPolygonResult::IsOn)
            return true;
        if (r == C2::PointInPolygonResult::IsInside)
            ++crossings;
    }
    return crossings % 2 == 1;
}

// What has been cut: a raster over the region, a cell cleared once a tool
// position came within the tool radius of its centre.
class Cleared
{
public:
    Cleared(const QRectF &box, double toolR)
    {
        // Fine enough to see a tenth of the stepover on a small tool, small
        // enough to stay a few megabytes on a full sheet.
        m_res = std::max({0.02, toolR / 25.0,
                          std::sqrt(box.width() * box.height() / 6e6)});
        m_org = box.topLeft() - QPointF(toolR, toolR);
        m_w = int(std::ceil((box.width() + 2 * toolR) / m_res)) + 1;
        m_h = int(std::ceil((box.height() + 2 * toolR) / m_res)) + 1;
        m_cells.assign(size_t(m_w) * size_t(m_h), 0);
        m_r = toolR;
    }

    bool cut(const QPointF &q) const
    {
        const int i = int(std::floor((q.x() - m_org.x()) / m_res));
        const int j = int(std::floor((q.y() - m_org.y()) / m_res));
        if (i < 0 || j < 0 || i >= m_w || j >= m_h)
            return true;   // outside the region's box: nothing to cut there
        return m_cells[size_t(j) * size_t(m_w) + size_t(i)] != 0;
    }

    void stamp(const QPointF &c)
    {
        const int i0 = std::max(0, int(std::floor((c.x() - m_r - m_org.x()) / m_res)));
        const int i1 = std::min(m_w - 1, int(std::ceil((c.x() + m_r - m_org.x()) / m_res)));
        const int j0 = std::max(0, int(std::floor((c.y() - m_r - m_org.y()) / m_res)));
        const int j1 = std::min(m_h - 1, int(std::ceil((c.y() + m_r - m_org.y()) / m_res)));
        const double r2 = m_r * m_r;
        for (int j = j0; j <= j1; ++j) {
            const double y = m_org.y() + (j + 0.5) * m_res - c.y();
            for (int i = i0; i <= i1; ++i) {
                const double x = m_org.x() + (i + 0.5) * m_res - c.x();
                if (x * x + y * y <= r2)
                    m_cells[size_t(j) * size_t(m_w) + size_t(i)] = 1;
            }
        }
    }

    // Discs every r/8 along the path: the sliver left between two
    // neighbouring discs is (r/8)^2 / 8r = r/512 deep, far below the raster.
    void stampPath(const QPolygonF &path)
    {
        const double spacing = std::max(m_res, m_r / 8);
        for (int k = 0; k + 1 < path.size(); ++k) {
            const QLineF seg(path.at(k), path.at(k + 1));
            const int n = std::max(1, int(std::ceil(seg.length() / spacing)));
            for (int s = 0; s < n; ++s)
                stamp(seg.pointAt(double(s) / n));
        }
        if (!path.isEmpty())
            stamp(path.last());
    }

    double res() const { return m_res; }

private:
    QPointF m_org;
    double m_res = 0.1, m_r = 1;
    int m_w = 0, m_h = 0;
    std::vector<unsigned char> m_cells;
};

// Samples on the cutter's rim, just inside it so a cell stamped by a
// neighbouring position at exactly one radius does not alias.
const int kRim = 72;

// Degrees of the cutter's front half (facing `dir`) buried in uncut material.
double engagement(const Cleared &cl, const QPointF &c, const QPointF &dir, double r)
{
    const double a0 = std::atan2(dir.y(), dir.x());
    int buried = 0;
    const int half = kRim / 2;
    for (int k = -half / 2; k <= half / 2; ++k) {
        const double a = a0 + k * 2 * M_PI / kRim;
        if (!cl.cut(c + QPointF(std::cos(a), std::sin(a)) * (r * 0.97)))
            ++buried;
    }
    return buried * 360.0 / kRim;
}

// Would a tool here cut anything at all?
bool cutsHere(const Cleared &cl, const QPointF &c, double r)
{
    for (int k = 0; k < kRim; k += 2) {
        const double a = k * 2 * M_PI / kRim;
        if (!cl.cut(c + QPointF(std::cos(a), std::sin(a)) * (r * 0.97)))
            return true;
    }
    for (int k = 0; k < 12; ++k) {
        const double a = k * 2 * M_PI / 12;
        if (!cl.cut(c + QPointF(std::cos(a), std::sin(a)) * (r * 0.5)))
            return true;
    }
    return !cl.cut(c);
}

QPolygonF densify(const QPolygonF &ring, double step)
{
    QPolygonF out;
    for (int k = 0; k + 1 < ring.size(); ++k) {
        const QLineF seg(ring.at(k), ring.at(k + 1));
        const int n = std::max(1, int(std::ceil(seg.length() / step)));
        for (int s = 0; s < n; ++s)
            out << seg.pointAt(double(s) / n);
    }
    if (!ring.isEmpty())
        out << ring.last();
    return out;
}

struct Candidate {
    QVector<QPolygonF> runs;
    double maxEngage = 0;
};

// The stretches of `rings` that cut something, with their worst engagement.
Candidate runsOf(const QVector<QPolygonF> &rings, const Cleared &cl, double r, double step)
{
    Candidate c;
    for (const QPolygonF &ring : rings) {
        QPolygonF d = densify(ring, step);
        const int n = d.size();
        if (n < 2)
            continue;
        const bool closed = QLineF(d.first(), d.last()).length() < 1e-9;
        const int m = closed ? n - 1 : n;   // distinct points
        QVector<bool> live(m);
        int liveCount = 0;
        for (int k = 0; k < m; ++k) {
            live[k] = cutsHere(cl, d.at(k), r);
            liveCount += live[k];
        }
        if (!liveCount)
            continue;
        // Bridge short dead stretches: feeding across a few mm of cleared
        // ground is quicker than lifting out and plunging back in, and it
        // keeps one pass from shattering into slivers.
        if (closed && liveCount < m) {
            const int bridge = int(std::ceil(std::max(10.0, 3 * r) / step));
            int k0 = 0;
            while (live.at(k0))   // start on a dead point
                ++k0;
            for (int t = 0; t < m;) {
                const int k = (k0 + t) % m;
                if (live.at(k)) { ++t; continue; }
                int len = 0;
                while (len < m && !live.at((k + len) % m))
                    ++len;
                if (len <= bridge && len < m - liveCount + 1 && liveCount + len < m) {
                    for (int j = 0; j < len; ++j)
                        live[(k + j) % m] = true;
                    liveCount += len;
                }
                t += len;
            }
        }
        auto dirAt = [&](int k) {
            const QPointF a = d.at(closed ? (k - 1 + m) % m : std::max(0, k - 1));
            const QPointF b = d.at(closed ? (k + 1) % m : std::min(m - 1, k + 1));
            const QPointF v = b - a;
            const double l = std::hypot(v.x(), v.y());
            return l > 0 ? v / l : QPointF(1, 0);
        };
        if (liveCount == m && closed) {   // the whole loop is new cut
            for (int k = 0; k < m; ++k)
                c.maxEngage = std::max(c.maxEngage, engagement(cl, d.at(k), dirAt(k), r));
            c.runs.append(d);
            continue;
        }
        // Start just after a dead point so runs do not straddle the seam.
        int start = 0;
        if (closed)
            while (live.at(start))
                ++start;
        QPolygonF run;
        for (int t = 0; t <= m; ++t) {
            const int k = closed ? (start + t) % m : t;
            if (!closed && t == m)
                break;
            if (live.at(k)) {
                if (run.isEmpty()) {   // pad back one point: start in the clear
                    const int pk = closed ? (k - 1 + m) % m : k - 1;
                    if (pk >= 0)
                        run << d.at(pk);
                }
                run << d.at(k);
                c.maxEngage = std::max(c.maxEngage, engagement(cl, d.at(k), dirAt(k), r));
            } else if (!run.isEmpty()) {
                run << d.at(k);   // and finish one point into the clear
                c.runs.append(run);
                run.clear();
            }
        }
        if (!run.isEmpty())
            c.runs.append(run);
    }
    return c;
}

// The deepest point of a region (furthest from its boundary) and how far
// that is, by bisection on the inset.
bool deepest(const C2::Paths64 &region, QPointF *at, double *clearance)
{
    double lo = 0, hi = 1;
    while (!inflate(region, -hi).empty() && hi < 1e5)
        hi *= 2;
    for (int it = 0; it < 30 && hi - lo > 0.002; ++it) {
        const double mid = (lo + hi) / 2;
        if (inflate(region, -mid).empty()) hi = mid; else lo = mid;
    }
    const C2::Paths64 core = inflate(region, -lo * 0.98);
    if (core.empty())
        return false;
    // Area-weighted centre of the biggest piece of the core, if it is
    // inside it; else any vertex of it.
    size_t best = 0;
    for (size_t i = 1; i < core.size(); ++i)
        if (std::fabs(C2::Area(core[i])) > std::fabs(C2::Area(core[best])))
            best = i;
    const QPolygonF poly = fromPath(core[best]);
    double a = 0, cx = 0, cy = 0;
    for (int i = 0; i + 1 < poly.size(); ++i) {
        const double w = poly.at(i).x() * poly.at(i + 1).y() - poly.at(i + 1).x() * poly.at(i).y();
        a += w;
        cx += (poly.at(i).x() + poly.at(i + 1).x()) * w;
        cy += (poly.at(i).y() + poly.at(i + 1).y()) * w;
    }
    QPointF c = std::fabs(a) > 1e-12 ? QPointF(cx / (3 * a), cy / (3 * a)) : poly.first();
    if (!inside(C2::Paths64{core[best]}, c))
        c = poly.first();
    *at = c;
    *clearance = lo;
    return true;
}

QPolygonF circle(const QPointF &c, double r, double step)
{
    const int n = std::max(12, int(std::ceil(2 * M_PI * r / step)));
    QPolygonF out;
    for (int k = 0; k <= n; ++k) {
        const double a = 2 * M_PI * k / n;
        out << c + QPointF(std::cos(a), std::sin(a)) * r;
    }
    return out;
}

} // namespace

Plan plan(const QVector<QPolygonF> &rings, const Params &p, const std::function<bool()> &cancel)
{
    Plan out;
    const double r = std::max(0.01, p.toolR);
    const double s = std::clamp(p.stepover, 0.01, 0.95 * r);
    out.straightEngageDeg = std::acos(1 - s / r) * 180 / M_PI;
    out.limitDeg = std::max(out.straightEngageDeg * p.maxEngageFactor,
                            out.straightEngageDeg + 10.0);

    C2::Paths64 regionIn;
    QRectF box;
    for (const QPolygonF &ring : rings) {
        if (ring.size() < 3)
            continue;
        regionIn.push_back(toPath(ring));
        box = box.isNull() ? ring.boundingRect() : box.united(ring.boundingRect());
    }
    const C2::Paths64 region = C2::Union(regionIn, C2::FillRule::EvenOdd);
    if (region.empty())
        return out;
    // Where the tool centre may go.
    const C2::Paths64 allowed = inflate(region, -(r + std::max(0.0, p.leave)));
    if (allowed.empty())
        return out;

    Cleared cl(box, r);
    const double step = std::max(cl.res() * 2, std::min(0.25, s / 2));
    QPointF last;
    bool haveLast = false;
    QPointF partEntry;
    C2::Paths64 partLoose;   // the part, 20 microns looser: the passes' own
                             // rounding must not count as leaving it
    QVector<QPointF> partCorners;
    QVector<QPolygonF> partRings;

    auto addCut = [&](const QPolygonF &path) {
        Move mv;
        mv.kind = Move::Cut;
        mv.path = path;
        if (haveLast) {
            const QLineF hop(last, path.first());
            const int n = std::max(1, int(std::ceil(hop.length() / step)));
            // Stay down when the tool would cut nothing on the way.
            bool clear = true;
            for (int k = 0; clear && k <= n; ++k)
                clear = !cutsHere(cl, hop.pointAt(double(k) / n), r);
            // Otherwise hop just above the level if the tool can stay over
            // the pocket the whole way: straight, or with one dog-leg via a
            // point it can see both ends from (the part's entry point first,
            // then the ends of earlier passes, most recent first).
            auto overPocket = [&](QPointF a, QPointF b) {
                const QLineF l(a, b);
                const int m = std::max(1, int(std::ceil(l.length() / step)));
                for (int k = 0; k <= m; ++k)
                    if (!inside(partLoose, l.pointAt(double(k) / m)))
                        return false;
                return true;
            };
            if (clear) {
                mv.link = Move::StayDown;
            } else if (overPocket(last, path.first())) {
                mv.link = Move::Lift;
            } else {
                mv.link = Move::Retract;
                QVector<QPointF> cands{partEntry};
                for (int k = out.moves.size() - 1; k >= 0 && cands.size() < 40; --k)
                    if (out.moves.at(k).kind == Move::Cut)
                        cands << out.moves.at(k).path.first() << out.moves.at(k).path.last();
                cands += partCorners;   // round the islands and reflex corners
                QVector<QPointF> fromHere, toThere;
                for (const QPointF &v : cands) {
                    const bool a = overPocket(last, v), b = overPocket(v, path.first());
                    if (a && b) {
                        mv.link = Move::Lift;
                        mv.via = QPolygonF({v});
                        break;
                    }
                    if (a) fromHere << v;
                    if (b) toThere << v;
                }
                // Round an island or a run of corners: walk along one of the
                // part's inset outlines, from a point of it the tool can see
                // from here to one it can see the start from, whichever way
                // round is shorter.
                if (mv.link == Move::Retract) {
                    double best = 1e30;
                    for (const QPolygonF &ring : partRings) {
                        const int m = ring.size() - 1;   // closed: last == first
                        if (m < 3)
                            continue;
                        QVector<double> along(m + 1, 0);
                        for (int k = 1; k <= m; ++k)
                            along[k] = along[k - 1] + QLineF(ring.at(k - 1), ring.at(k)).length();
                        const double perim = along[m];
                        QVector<int> I, J;
                        for (int k = 0; k < m; ++k) {
                            if (overPocket(last, ring.at(k))) I << k;
                            if (overPocket(ring.at(k), path.first())) J << k;
                        }
                        for (int i : I) {
                            for (int j : J) {
                                const double fwd = j >= i ? along[j] - along[i] : perim - (along[i] - along[j]);
                                const double rev = perim - fwd;
                                const double walk = std::min(fwd, rev);
                                const double len = QLineF(last, ring.at(i)).length() + walk
                                                 + QLineF(ring.at(j), path.first()).length();
                                if (len >= best)
                                    continue;
                                best = len;
                                QPolygonF v;
                                const bool forward = fwd <= rev;
                                for (int k = i;; k = forward ? (k + 1) % m : (k - 1 + m) % m) {
                                    v << ring.at(k);
                                    if (k == j)
                                        break;
                                }
                                mv.via = v;
                                mv.link = Move::Lift;
                            }
                        }
                    }
                }
            }
        }
        out.moves.append(mv);
        cl.stampPath(path);
        last = path.last();
        haveLast = true;
    };

    const QVector<C2::Paths64> pieces = parts(allowed);
    for (const C2::Paths64 &part : pieces) {
        haveLast = false;   // a new part starts with its own helix
        QPointF c;
        double clearance = 0;
        if (!deepest(part, &c, &clearance) || clearance < 0.02 * r) {
            ++out.unreached;
            continue;
        }
        // Entry: a helix around the widest point, then a lap at depth.
        const double hr = std::min(0.5 * r, 0.9 * clearance);
        partEntry = c;
        partLoose = inflate(part, 0.02);
        // Dog-leg points: the part's boundary pulled in a little, every few
        // mm (on the boundary itself a straight leg would graze it).
        // Outlines a little and a tool radius in from the walls: hops round
        // islands walk along them (see addCut); their points are dog-leg
        // candidates too.
        partCorners.clear();
        partRings.clear();
        for (const double in : {0.05, r}) {
            if (in > 0.05 && in > 0.8 * clearance)
                break;
            for (const QPolygonF &ring : orientedRings(inflate(part, -in), true)) {
                const QPolygonF d = densify(ring, std::max(1.0, r / 2));
                partRings << d;
                for (int k = 0; k + 1 < d.size(); k += 4)
                    partCorners << d.at(k);
            }
        }
        Move hx;
        hx.kind = Move::Helix;
        hx.center = c;
        hx.radius = hr;
        out.moves.append(hx);
        const QPolygonF lap = circle(c, hr, step);
        cl.stampPath(lap);
        last = lap.first();
        haveLast = true;

        // Visited centres so far: the helix disc.
        C2::Paths64 visited = C2::Intersect(C2::Paths64{toPath(circle(c, hr, step))}, part,
                                            C2::FillRule::NonZero);
        for (int guard = 0; guard < 20000; ++guard) {
            if (cancel && cancel())
                return out;
            const double before = area(visited);
            double grow = s;
            C2::Paths64 next;
            Candidate cand;
            for (;;) {
                next = C2::Intersect(inflate(visited, grow), part, C2::FillRule::NonZero);
                cand = runsOf(orientedRings(next, p.climb), cl, r, step);
                if (cand.maxEngage <= out.limitDeg || grow <= s / 8 + 1e-12)
                    break;
                grow /= 2;
            }
            const double after = area(next);
            for (const QPolygonF &run : cand.runs)
                addCut(run);
            if (!cand.runs.isEmpty())
                out.maxEngageDeg = std::max(out.maxEngageDeg, cand.maxEngage);
            visited = next;
            if (after - before < 1e-4 * std::max(1.0, after) && cand.runs.isEmpty())
                break;
        }
    }
    return out;
}

#else

Plan plan(const QVector<QPolygonF> &, const Params &, const std::function<bool()> &)
{
    Plan out;
    out.available = false;
    return out;
}

#endif

} // namespace adaptive
} // namespace c2d
