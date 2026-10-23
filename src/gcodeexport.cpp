#include "gcodeexport.h"
#include "adaptive.h"
#include "c2ddocument.h"
#include "cam3d.h"
#include "heightmodel.h"
#include "post_grbl.h"
#include "vcarve.h"
#include "vectorops.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QLineF>
#include <QPainterPathStroker>
#include <QPolygonF>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>

#ifdef HAVE_CLIPPER2
#include <clipper2/clipper.h>
namespace C2 = Clipper2Lib;
#endif

namespace c2d {

// Depth values are strings whose sign convention flipped between CC builds
// (843 stores "-19.126", 853 stores "19.126" for the same cut). Machine Z is
// negative below stock top either way.
// Depths are the numbers that decide how deep the tool goes, so they get the
// strictest reading of all. An absent key means "from the top" and is fine; a
// key that is present but unreadable is NOT a depth, and `ok` says so.
//
// This used to be a bare toString().toDouble(), which checked neither the
// parse flag nor finiteness, and it was the only reader in this file that did
// not go through numOr(). Four things got through it: "nan" (which toDouble
// returns with ok == true) put a literal `G1Znan` in the program; a comma
// decimal or "19.05mm" became 0 and traced the shape on the surface, cutting
// nothing, and reported success; and "inf", "1e400" or even a finite "1e15"
// made the pass loop below run for ever, freezing the app, because the export
// runs on the GUI thread.
static double depthToZ(const QJsonValue &v, bool *ok = nullptr)
{
    if (ok)
        *ok = true;
    if (v.isUndefined() || v.isNull())
        return 0.0;
    const double d = numOr(v, std::numeric_limits<double>::quiet_NaN());
    if (!std::isfinite(d)) {
        if (ok)
            *ok = false;
        return 0.0;
    }
    return -qAbs(d);
}


// The vectors a toolpath machines. Construction geometry never is, and
// sketch points only mark holes, so only drilling and keyhole take them.
static QVector<const Element *> referenced(Document &doc, const QJsonObject &tp)
{
    const QString type = tp.value("type").toString();
    const bool takesPoints = type == QLatin1String("drilling_toolpath")
                             || type == QLatin1String("keyhole_toolpath");
    QVector<const Element *> out;
    for (const QJsonValue &ev : tp.value("elements").toArray()) {
        const QString id = ev.toObject().value("uuid").toString();
        if (const Element *e = doc.elementById(id)) {
            if (Element::isConstruction(*e) || (Element::isPoint(*e) && !takesPoints))
                continue;
            out.append(e);
        }
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

// How far a stay-down link may stray past the tool-centre region it is
// checked against: the rings themselves lie on its boundary, and a link
// between two of them must not fail on rounding.
static const double kLinkLeeway = 0.02;

// A stay-down link check for CutParams::linkOk: the straight feed from a to
// b is kept only when it stays inside `centres` (a tool-centre region, grown
// by kLinkLeeway here). Exact, not sampled: both ends inside and no edge
// crossed. Without it a pocket that splits around an island into lobes less
// than linkDist apart fed straight across the island at every depth.
static std::function<bool(const QPointF &, const QPointF &)> linkInside(const QPainterPath &centres)
{
    struct Area {
        QList<QPolygonF> polys;
        QList<QRectF> boxes;
    };
    auto area = std::make_shared<Area>();
    for (const QPolygonF &p : outsetRings(centres, kLinkLeeway)) {
        area->polys << p;
        area->boxes << p.boundingRect();
    }
    return [area](const QPointF &a, const QPointF &b) {
        // Even-odd over the ring polygons (see restAdjust).
        auto inside = [&](const QPointF &p) {
            int crossings = 0;
            for (const QPolygonF &poly : area->polys)
                if (poly.containsPoint(p, Qt::OddEvenFill))
                    ++crossings;
            return (crossings & 1) != 0;
        };
        if (!inside(a) || !inside(b))
            return false;
        const QLineF ab(a, b);
        const QRectF box = QRectF(a, b).normalized();
        for (int k = 0; k < area->polys.size(); ++k) {
            if (!area->boxes.at(k).intersects(box.adjusted(-1e-6, -1e-6, 1e-6, 1e-6)))
                continue;
            const QPolygonF &poly = area->polys.at(k);
            for (int i = 0; i + 1 < poly.size(); ++i)
                if (ab.intersects(QLineF(poly.at(i), poly.at(i + 1)), nullptr)
                    == QLineF::BoundedIntersection)
                    return false;
        }
        return true;
    };
}

#ifdef HAVE_CLIPPER2
static QPainterPath subtractRegion(const QPainterPath &a, const QPainterPath &b)
{
    return regionFromRings(ringsFromPaths(
        C2::Difference(pathsOfRegion(a), pathsOfRegion(b), C2::FillRule::EvenOdd)));
}
static QPainterPath intersectRegion(const QPainterPath &a, const QPainterPath &b)
{
    return regionFromRings(ringsFromPaths(
        C2::Intersect(pathsOfRegion(a), pathsOfRegion(b), C2::FillRule::EvenOdd)));
}
#else
static QPainterPath subtractRegion(const QPainterPath &a, const QPainterPath &b)
{
    QPainterPath r = a.subtracted(b).simplified();
    r.setFillRule(Qt::OddEvenFill);
    return r;
}

static QPainterPath intersectRegion(const QPainterPath &a, const QPainterPath &b)
{
    QPainterPath r = a.intersected(b).simplified();
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


// ---- automatic rest depth ---------------------------------------------------
//
// Toolpaths that start where the ones above them stopped. Each ring toolpath
// (pocket / contour / cutout) and each adaptive one leaves behind the region
// its tool swept and the floor it reached. A later toolpath with `rest_auto`
// whose every tool-centre point lies inside what was already cleared - inset
// by its own radius, so the whole disc is in cut air - starts `rest_overlap`
// above the deepest floor that covers it, instead of at the stock top. A
// pocket or adaptive only partly inside cuts just what is left, in depth
// bands (restBands); a contour or cutout only partly inside keeps its full
// depth. One whose floor is already reached is left out.
static ToolGeom toolGeomOf(const QJsonObject &tool);

// The tool change for one embedded tool object: its number, a key that is
// the same only for the same bit, and the bit's shape for the simulation
// (x diameter, y V angle, z corner radius, feed kind).
static Op toolOp(const QJsonObject &tool)
{
    const ToolGeom g = toolGeomOf(tool);
    Op o = Op::tool(g.number);
    o.text = QStringLiteral("%1|%2|%3|%4|%5").arg(g.number).arg(int(g.kind))
                 .arg(g.diameter, 0, 'g', 10).arg(g.angle, 0, 'g', 10).arg(g.cornerRadius, 0, 'g', 10);
    o.x = g.diameter;
    o.y = g.angle;
    o.z = g.cornerRadius;
    o.feed = int(g.kind) + 1;
    return o;
}

struct Cleared {
    QPainterPath foot;   // material removed, as a region
    double zBot = 0;     // the floor it reached (Z, negative down)
};

#ifdef HAVE_CLIPPER2
static QPainterPath sweptRegion(const QList<QPolygonF> &centres, double toolR)
{
    // A closed ring is a joined polyline (offset both sides, no ends): toPath64
    // drops its repeated closing point, so as an open path it would lose a side.
    C2::Paths64 loops, open;
    for (const QPolygonF &poly : centres) {
        if (poly.size() < 2)
            continue;
        (poly.isClosed() && poly.size() > 3 ? loops : open).push_back(toPath64(poly));
    }
    C2::Paths64 all = C2::InflatePaths(loops, toolR * kScale, C2::JoinType::Round,
                                       C2::EndType::Joined, 2.0, 10.0);
    for (const C2::Path64 &p : C2::InflatePaths(open, toolR * kScale, C2::JoinType::Round,
                                                C2::EndType::Round, 2.0, 10.0))
        all.push_back(p);
    return regionFromRings(ringsFromPaths(C2::Union(all, C2::FillRule::NonZero)));
}

static QPainterPath unionRegions(const QList<QPainterPath> &regions)
{
    C2::Paths64 ps;
    for (const QPainterPath &r : regions)
        for (const C2::Path64 &p : pathsOfRegion(r))
            ps.push_back(p);
    return regionFromRings(ringsFromPaths(C2::Union(ps, C2::FillRule::NonZero)));
}
#else
static QPainterPath sweptRegion(const QList<QPolygonF> &centres, double toolR)
{
    QPainterPathStroker st;
    st.setWidth(2 * toolR);
    st.setJoinStyle(Qt::RoundJoin);
    st.setCapStyle(Qt::RoundCap);
    QPainterPath out;
    for (const QPolygonF &poly : centres) {
        if (poly.size() < 2)
            continue;
        QPainterPath c;
        c.addPolygon(poly);
        out = out.united(st.createStroke(c));
    }
    out = out.simplified();
    out.setFillRule(Qt::OddEvenFill);
    return out;
}

static QPainterPath unionRegions(const QList<QPainterPath> &regions)
{
    QPainterPath out;
    for (const QPainterPath &r : regions)
        out = out.united(r);
    out = out.simplified();
    out.setFillRule(Qt::OddEvenFill);
    return out;
}
#endif

enum class RestVerdict { Untouched, Raised, Done, Partial };

// Points along a tool-centre line no more than `step` apart: the rest test
// needs the whole line, not just its corners - a long straight pass between
// two corners in cut air can still run through an island left standing.
static void addAlong(QVector<QPointF> &pts, const QPolygonF &line, double step, bool closed)
{
    const int n = line.size();
    for (int i = 0; i < n; ++i) {
        pts << line.at(i);
        if (i + 1 == n && !closed)
            break;
        const QPointF a = line.at(i), b = line.at((i + 1) % n);
        const int k = int(std::ceil(QLineF(a, b).length() / step));
        for (int m = 1; m < k; ++m)
            pts << a + (b - a) * (double(m) / k);
    }
}

// Deepest floor among `cleared` under which every point of `pts` (tool
// centres of radius toolR) is in cut air; `*zTop` is lowered to that floor
// plus `overlap`. Done: that leaves nothing above zBot.
static RestVerdict restAdjust(const QList<Cleared> &cleared, const QVector<QPointF> &pts,
                              double toolR, double overlap, double *zTop, double zBot,
                              double *floor)
{
    if (cleared.isEmpty() || pts.isEmpty())
        return RestVerdict::Untouched;
    QList<double> floors;
    for (const Cleared &c : cleared)
        if (!floors.contains(c.zBot))
            floors << c.zBot;
    std::sort(floors.begin(), floors.end());          // deepest (most negative) first
    bool anyInside = false;
    for (double z : floors) {
        QList<QPainterPath> regs;
        for (const Cleared &c : cleared)
            if (c.zBot <= z + 1e-6)
                regs << c.foot;
        const QPainterPath air = regionFromRings(insetRings(unionRegions(regs), qMax(0.0, toolR - 0.02)));
        if (air.isEmpty())
            continue;
        // Even-odd over the ring polygons: QPainterPath::contains() is not
        // reliable on rings with a repeated closing point.
        const QList<QPolygonF> airPolys = air.toSubpathPolygons();
        auto inAir = [&airPolys](const QPointF &p) {
            int crossings = 0;
            for (const QPolygonF &poly : airPolys)
                if (poly.containsPoint(p, Qt::OddEvenFill))
                    ++crossings;
            return (crossings & 1) != 0;
        };
        int in = 0;
        for (const QPointF &p : pts)
            if (inAir(p))
                ++in;
        anyInside = anyInside || in > 0;
        if (in == pts.size()) {
            *floor = z;
            *zTop = qMin(*zTop, z + overlap);
            return *zTop <= zBot + 1e-6 ? RestVerdict::Done : RestVerdict::Raised;
        }
    }
    return anyInside ? RestVerdict::Partial : RestVerdict::Untouched;
}

// A toolpath only partly inside what the toolpaths above cleared (Partial)
// keeps its own depth passes, but each pass only visits the tool centres of
// `mine` whose disc is not already in cut air at that depth - the same
// inset-by-own-radius test as above, against the toolpaths whose floor is at
// or below the pass. Consecutive passes with the same toolpaths below them
// share one band. Deeper bands contain the shallower ones' regions and
// assume them cut, so they must run in order, shallowest first.
struct RestBand {
    double zTop = 0, zBot = 0;   // cut from zTop down to zBot, `stepdown` at a time
    QPainterPath centres;        // tool-centre region still to visit
};

static QList<RestBand> restBands(const QList<Cleared> &cleared, const QPainterPath &mine,
                                 double toolR, double zTop, double zBot, double stepdown)
{
    QList<RestBand> out;
    QList<int> key;                      // the toolpaths above already below this pass
    QPainterPath rest;
    double top = zTop, prevZ = zTop;
    auto close = [&] {
        if (!rest.isEmpty())
            out.append({top, prevZ, rest});
        top = prevZ;
    };
    bool first = true;
    while (prevZ > zBot + 1e-9) {
        const double z = qMax(zBot, prevZ - qMax(0.01, stepdown));
        QList<int> k;
        for (int i = 0; i < cleared.size(); ++i)
            if (cleared.at(i).zBot <= z + 1e-6)
                k << i;
        if (first || k != key) {
            if (!first)
                close();
            first = false;
            key = k;
            QList<QPainterPath> regs;
            for (int i : k)
                regs << cleared.at(i).foot;
            rest = mine;
            if (!regs.isEmpty()) {
                const QPainterPath skip =
                    regionFromRings(insetRings(unionRegions(regs), qMax(0.0, toolR - 0.02)));
                if (!skip.isEmpty())
                    rest = subtractRegion(mine, skip);
            }
        }
        prevZ = z;
    }
    if (!first)
        close();
    return out;
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
    // Absolute, not proportional. A 0.4% band is 0.4 mm on a 100 mm radius,
    // and what gets emitted is a perfect circle of radius |c - poly[0]| - so a
    // bore that wobbles inside the band is machined up to 0.8 mm off on
    // diameter with nothing to show for it. 0.02 mm is the same bound the
    // sagitta test below uses, and vertices of a flattened arc sit on the
    // true circle anyway.
    for (int i = 0; i < n; ++i)
        if (qAbs(QLineF(c, poly.at(i)).length() - r) > 0.02)
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
    // Contour lead-in / lead-out (Fusion's linking arcs): a quarter circle
    // centred at `c` from `a` onto the ring's first point, and another from
    // there off to `b`, both on the air side. Single closed ring jobs only.
    struct Lead {
        bool on = false;
        QPointF a, c, b;
        bool cw = false;
    } lead;
    // Contour finishing (Fusion's multiple finishing passes and spring
    // pass): rings cut once each at the final depth after the roughing
    // ring, nearest the finished edge last, each with its own lead when
    // the rough ring has one.
    QList<QPolygonF> finish;
    QList<Lead> finishLeads;
    // Contours and cutouts: the outline the rings are offset from. Tabs are
    // measured on it, so each holds its width of the part's own edge.
    QList<QPolygonF> edge;
    // Helical entry (pockets): each depth is entered on a helix of radius r
    // about c, sinking at `angle` degrees, instead of plunging.
    struct Helix {
        bool on = false;
        QPointF c;
        double r = 0, angle = 2;
    } helix;
    // Rest bands (see restBands): this job starts at bandTop instead of the
    // toolpath's top, and runs after the toolpath's shallower bands.
    int band = 0;
    double bandTop = std::numeric_limits<double>::quiet_NaN();
    double bandBot = std::numeric_limits<double>::quiet_NaN();
    QPointF start() const { return lead.on ? lead.a : rings.first().first(); }
};

struct CutParams {
    double zTop = 0, zBot = 0, stepdown = 1.0;
    double linkDist = 1e9;   // stay-down link limit between rings
    double rampAngle = 0;    // >0: ramp entries on closed loops (degrees)
    double tabTop = -1e9;    // cutout tabs: lift to this Z inside a tab
    double tabWidth = 0;     // tab span along the loop (0 = no tabs)
    // The bit's radius: the lift runs that much further at each end, or the
    // bit, dropping at the tab's ends, ate the bridge across the kerf from
    // both sides (a 6 mm tab with a 1/4" bit held nothing).
    double tabToolR = 0;
    // A lead's radius, when the loop has one: its arcs sweep that far along
    // the ring either side of the start, so the tabs keep clear of it.
    double tabLeadR = 0;
    // Optional veto for a stay-down link (3D roughing checks it against the
    // relief); unset = any link within linkDist is taken.
    std::function<bool(const QPointF &, const QPointF &)> linkOk;
};

// Ring-fill a centre region: concentric insets every `stepover`, innermost
// first, as one job (a firstDelta of 0 starts on the region's own boundary —
// used for rest regions, already in centre space). Returns false when the
// ring count ran into its bound with region still left to fill. With the
// tool radius given, a stepover past it gets one more ring where the last
// inset left a middle the tool does not reach.
static bool ringFillJob(const QPainterPath &area, double firstDelta, double stepover,
                        QList<Job> &jobs, double toolR = 0)
{
    Job job;
    QList<QList<QPolygonF>> shells;
    double delta = firstDelta;
    // No point of the region lies more than half its narrower side from the
    // boundary, so that many stepovers (and a couple spare) reach the centre
    // of any real pocket. A fixed 500 left the middle of a 600 mm pocket at
    // a 0.5 mm stepover uncut without a word. The hard bound only stops an
    // absurd stepover from running for ever.
    const QRectF bb = area.boundingRect();
    const double reach = 0.5 * qMin(bb.width(), bb.height()) - firstDelta;
    const int cap = int(qBound(2.0, std::ceil(reach / qMax(stepover, 1e-6)) + 3, 20000.0));
    bool full = true;
    while (true) {
        if (shells.size() >= cap) {
            full = insetRings(area, delta).isEmpty();
            break;
        }
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
        if (s.isEmpty()) {
            // The deepest point lies short of `delta`; the last ring reaches
            // its inset plus a radius. A ring at delta - 0.9 radius reaches
            // past it (a 4.5 mm stepover on a 6.35 mm bit left a 2.5 mm rib
            // along a pocket's middle).
            const double last = delta - stepover;
            const double fill = delta - 0.9 * toolR;
            if (toolR > 0 && !shells.isEmpty() && fill > last + 0.05 && last + toolR < delta) {
                const QList<QPolygonF> m = insetRings(area, fill);
                if (!m.isEmpty())
                    shells.append(m);
            }
            break;
        }
        shells.append(s);
        delta += stepover;
    }
    for (int k = shells.size() - 1; k >= 0; --k)
        job.rings.append(shells.at(k));
    // A branch that runs out while the rest goes on (the arm of an L, a
    // channel beside an island) can keep a rib the check above never sees:
    // fill what the rings' sweep leaves of the centre region, as the job's
    // last laps (the stock round it is gone by then, and a helix entry on
    // the job stays where it was). Only a stepover past the radius can
    // leave one: the default (the radius) never pays for the check.
    if (toolR > 0 && stepover > toolR + 1e-6 && !shells.isEmpty()) {
        QPainterPath centre;
        centre.setFillRule(Qt::OddEvenFill);
        for (const QPolygonF &g : shells.first()) {
            centre.addPolygon(g);
            centre.closeSubpath();
        }
        const QPainterPath left = subtractRegion(centre, sweptRegion(job.rings, toolR));
        for (const QPainterPath &comp : components(left)) {
            if (insetRings(comp, 0.02).isEmpty())
                continue;            // a sliver the tool all but reaches
            QList<Job> more;
            full &= ringFillJob(comp, 0.0, stepover, more);
            for (const Job &m : std::as_const(more))
                job.rings.append(m.rings);
        }
    }
    if (!job.rings.isEmpty())
        jobs.append(job);
    return full;
}

// Lead arcs for a closed tool-centre ring: quarter circles of radius
// `radius` (halved until they fit, down to an eighth) tangent to the ring at
// its first point, on whichever side `allowed` accepts every sampled point
// of. The ring's own region decides that: outside a contour the arcs must
// stay out of the outset region, inside one within the inset region, so the
// cutter never touches the finished edge on its way in or out.
// Shortest distance from p to the polyline.
static double distToPoly(const QPointF &p, const QPolygonF &poly)
{
    double best = std::numeric_limits<double>::max();
    for (int i = 0; i + 1 < poly.size(); ++i) {
        const QPointF a = poly.at(i), b = poly.at(i + 1), d = b - a;
        const double l2 = d.x() * d.x() + d.y() * d.y();
        const double t = l2 > 1e-18 ? qBound(0.0, ((p.x() - a.x()) * d.x() + (p.y() - a.y()) * d.y()) / l2, 1.0) : 0.0;
        best = qMin(best, QLineF(p, a + d * t).length());
    }
    return best;
}

// Tool-centre rings of a contour turned to run climb or conventional with a
// clockwise spindle (as orientWalk in adaptive.cpp): climb keeps the
// material on the right of travel. `materialInside`: the part lies inside
// the region the rings bound (an outside contour); otherwise outside it.
// A ring nested in an odd number of the others bounds a hole of that
// region, so its material is on the other side.
static void orientForCut(QList<QPolygonF> &rings, bool materialInside, bool climb)
{
    for (int i = 0; i < rings.size(); ++i) {
        QPolygonF &r = rings[i];
        if (r.size() < 3)
            continue;
        int depth = 0;
        for (int k = 0; k < rings.size(); ++k)
            if (k != i && rings.at(k).containsPoint(r.first(), Qt::OddEvenFill))
                ++depth;
        double area2 = 0;
        for (int k = 0; k < r.size(); ++k) {
            const QPointF &a = r.at(k), &b = r.at((k + 1) % r.size());
            area2 += a.x() * b.y() - b.x() * a.y();
        }
        const bool inLoop = (depth % 2 == 0) == materialInside;
        const bool wantCcw = inLoop != climb;
        if ((area2 > 0) != wantCcw)
            std::reverse(r.begin(), r.end());
    }
}

// Distance along the polyline to the point on it nearest p (and, when
// asked, how far p is from it).
static double arcPosOf(const QPolygonF &poly, const QPointF &p, double *dist = nullptr)
{
    double best = std::numeric_limits<double>::max(), s = 0, at = 0;
    for (int i = 0; i + 1 < poly.size(); ++i) {
        const QPointF a = poly.at(i), d = poly.at(i + 1) - a;
        const double l2 = d.x() * d.x() + d.y() * d.y();
        const double t = l2 > 1e-18 ? qBound(0.0, ((p.x() - a.x()) * d.x() + (p.y() - a.y()) * d.y()) / l2, 1.0) : 0.0;
        const double dd = QLineF(p, a + d * t).length();
        if (dd < best) {
            best = dd;
            at = s + std::sqrt(l2) * t;
        }
        s += std::sqrt(l2);
    }
    if (dist)
        *dist = best;
    return at;
}

// The point `s` along the polyline.
static QPointF pointAtArc(const QPolygonF &poly, double s)
{
    for (int i = 0; i + 1 < poly.size(); ++i) {
        const QLineF seg(poly.at(i), poly.at(i + 1));
        if (s <= seg.length() || i + 2 == poly.size())
            return seg.length() > 1e-12 ? seg.pointAt(qBound(0.0, s / seg.length(), 1.0)) : seg.p1();
        s -= seg.length();
    }
    return poly.isEmpty() ? QPointF() : poly.first();
}

// A closed outline that is a slot (a stadium: every point half the width
// from the segment c1-c2 between the two end centres). Found from its
// farthest pair of points, then checked point by point. Circles and
// anything else are not slots.
static bool asStadium(const QPolygonF &polyIn, QPointF *c1, QPointF *c2, double *width)
{
    QPolygonF poly;
    const int step = qMax(1, int(polyIn.size() / 600));
    for (int i = 0; i < polyIn.size(); i += step)
        poly << polyIn.at(i);
    if (poly.size() < 8)
        return false;
    QPointF a, b;
    double far = -1;
    for (int i = 0; i < poly.size(); ++i)
        for (int k = i + 1; k < poly.size(); ++k) {
            const double d = QLineF(poly.at(i), poly.at(k)).length();
            if (d > far) {
                far = d;
                a = poly.at(i);
                b = poly.at(k);
            }
        }
    if (far < 1e-6)
        return false;
    const QPointF u = (b - a) / far;
    double half = 0;
    for (const QPointF &p : poly)
        half = qMax(half, qAbs((p.x() - a.x()) * u.y() - (p.y() - a.y()) * u.x()));
    if (far - 2 * half < 0.01)
        return false;                   // round: a circle, not a slot
    const QPointF s1 = a + u * half, s2 = b - u * half;
    const double tol = qMax(0.02, 0.002 * 2 * half);
    for (const QPointF &p : polyIn)
        if (qAbs(distToPoly(p, QPolygonF() << s1 << s2) - half) > tol)
            return false;
    *c1 = s1;
    *c2 = s2;
    *width = 2 * half;
    return true;
}

// The closed ring restarted at the point on it nearest to p (inserted there
// as a vertex), so a cut that ends at p carries straight on to it.
static QPolygonF startNearest(const QPolygonF &ring, const QPointF &p)
{
    if (ring.size() < 4 || ring.first() != ring.last())
        return ring;
    int best = 0;
    QPointF at = ring.first();
    double bestD = std::numeric_limits<double>::max();
    for (int i = 0; i + 1 < ring.size(); ++i) {
        const QPointF a = ring.at(i), d = ring.at(i + 1) - a;
        const double l2 = d.x() * d.x() + d.y() * d.y();
        const double t = l2 > 1e-18 ? qBound(0.0, ((p.x() - a.x()) * d.x() + (p.y() - a.y()) * d.y()) / l2, 1.0) : 0.0;
        const QPointF q = a + d * t;
        const double dd = QLineF(p, q).length();
        if (dd < bestD) {
            bestD = dd;
            best = i;
            at = q;
        }
    }
    QPolygonF out;
    out << at;
    const int n = ring.size() - 1;
    for (int k = 1; k <= n; ++k) {
        const QPointF v = ring.at((best + k) % n);
        if (QLineF(v, out.last()).length() > 1e-9)
            out << v;
    }
    if (QLineF(out.last(), at).length() > 1e-9)
        out << at;
    else
        out.last() = at;
    return out;
}

// The ring of `level` that runs `dist` from the finishing ring p all along:
// the same loop offset further, or none when it vanished, merged or split.
// Matching on one point of p took a ring that two parts' offsets had merged
// into: both loops then roughed it, and the step across from it to p went
// straight through the neighbouring part at full depth. So every vertex and
// edge midpoint of the candidate must lie `dist` from p, and p must lie
// along it in turn - or the candidate is what is left of one corner of a
// loop whose narrow arms vanished, and roughing only that corner leaves the
// rest of the loop to the finishing rings at full depth. Only at p's sharp
// corners may p stand further off: there the offset's corner stands back
// dist / sin(angle / 2), over dist / tan(angle / 2) of either edge.
static int parallelRing(const QList<QPolygonF> &level, const QPolygonF &p, double dist, double tol)
{
    const QRectF reach = p.boundingRect().adjusted(-dist - tol, -dist - tol, dist + tol, dist + tol);
    auto runsAlong = [&](const QPolygonF &q) {
        for (int i = 0; i < q.size(); ++i) {
            if (qAbs(distToPoly(q.at(i), p) - dist) > tol)
                return false;
            if (i + 1 < q.size()
                && qAbs(distToPoly((q.at(i) + q.at(i + 1)) / 2, p) - dist) > tol)
                return false;
        }
        return true;
    };
    struct Corner { QPointF at; double reach, off; };
    QVector<Corner> corners;
    const int n = p.size() - (p.size() > 1 && p.first() == p.last() ? 1 : 0);
    // A corner's edges are taken from the first vertices at least `span`
    // along the ring on either side, with spans from tol to twice dist, and
    // the sharpest reading kept: Clipper often rounds a cusp over a couple
    // of micro-segments and a bend of a millimetre or so, and measured
    // vertex to vertex none of them made the whole turn, so the corner was
    // missed and a good ring rejected. The walks follow the ring, and with
    // the corner's own turn they turn less than half a turn in all:
    // measured as straight-line distance a walk went round the tip of a
    // narrow arm and back down its other side, read the arm's two sides as
    // a corner of almost no angle, and let a rough ring that had lost the
    // whole arm stand tens of millimetres off it, so the arm was cut at full
    // depth by the finishing rings alone. A corner also only reaches as far
    // as its edges were walked.
    auto vertex = [&](int k) { return p.at((k % n + n) % n); };
    struct Walk { QPointF at; double len, turn; };
    auto away = [&](int i, int dir, double span, double budget) {
        QPointF prev = p.at(i), heading;
        double len = 0, turn = 0;
        for (int k = 1; k < n; ++k) {
            const QPointF q = vertex(i + dir * k), d = q - prev;
            const double l = std::hypot(d.x(), d.y());
            if (l < 1e-12)
                continue;
            if (!heading.isNull()) {
                const double t = turn + qAbs(std::atan2(heading.x() * d.y() - heading.y() * d.x(),
                                                        heading.x() * d.x() + heading.y() * d.y()));
                if (t >= budget)
                    break;
                turn = t;
            }
            heading = d;
            len += l;
            prev = q;
            if (len >= span)
                break;
        }
        return Walk{prev, len, turn};
    };
    // Both sides' walks, each order of sharing the budget tried.
    auto walks = [&](int i, double span, double budget, bool backFirst) {
        const Walk first = away(i, backFirst ? -1 : 1, span, budget);
        const Walk second = away(i, backFirst ? 1 : -1, span, budget - first.turn);
        return backFirst ? qMakePair(first, second) : qMakePair(second, first);
    };
    for (int i = 0; n >= 3 && i < n; ++i) {
        const QPointF v = p.at(i);
        // The corner's own turn, between its two neighbouring edges.
        QPointF in, out;
        for (int k = 1; k < n && in.isNull(); ++k)
            in = v - vertex(i - k);
        for (int k = 1; k < n && out.isNull(); ++k)
            out = vertex(i + k) - v;
        const double budget = M_PI - 1e-3 - qAbs(std::atan2(in.x() * out.y() - in.y() * out.x(),
                                                            in.x() * out.x() + in.y() * out.y()));
        double half = M_PI / 2;
        bool backFirst = true;
        for (const double span : {0.0, tol, dist, 2 * dist})
            for (const bool order : {true, false}) {
                const auto [a, b] = walks(i, span, budget, order);
                const QPointF u = a.at - v, w = b.at - v;
                const double lu = std::hypot(u.x(), u.y()), lw = std::hypot(w.x(), w.y());
                if (lu < 1e-9 || lw < 1e-9)
                    continue;
                const double h = std::acos(qBound(-1.0, (u.x() * w.x() + u.y() * w.y()) / (lu * lw), 1.0)) / 2;
                if (h > 1e-3 && h < half) {
                    half = h;
                    backFirst = order;
                }
            }
        if (half < M_PI / 2 - 0.02 && half > 1e-3) {
            const double back = dist / std::tan(half);
            const auto [a, b] = walks(i, back, budget, backFirst);
            corners.append({v, qMin(back, qMin(a.len, b.len)) + tol, dist / std::sin(half) + tol});
        }
    }
    auto covers = [&](const QPolygonF &q) {
        auto near = [&](const QPointF &x) {
            const double e = distToPoly(x, q);
            if (e <= dist + tol)
                return true;
            for (const Corner &c : corners)
                if (QLineF(x, c.at).length() <= c.reach && e <= c.off)
                    return true;
            return false;
        };
        for (int i = 0; i < p.size(); ++i)
            if (!near(p.at(i)) || (i + 1 < p.size() && !near((p.at(i) + p.at(i + 1)) / 2)))
                return false;
        return true;
    };
    int found = -1;
    for (int i = 0; i < level.size(); ++i) {
        const QPolygonF &q = level.at(i);
        if (q.isEmpty() || !reach.contains(q.boundingRect())
            || qAbs(distToPoly(q.first(), p) - dist) > tol || !runsAlong(q) || !covers(q))
            continue;
        if (found >= 0)
            return -1;
        found = i;
    }
    return found;
}

static Job::Helix helixIn(const QList<QPolygonF> &rings, double want, double angle);

// Give `job` a helical entry inside the tool-centre region bounded by
// `rings`, and start its rings there. Leave the helix for the ring nearest
// it, at its nearest point: no ring lies across that short move, so it
// never runs over an island.
static void addHelix(Job &job, const QList<QPolygonF> &rings, double want, double angle)
{
    job.helix = helixIn(rings, want, angle);
    if (!job.helix.on || job.rings.isEmpty())
        return;
    const QPointF from = job.helix.c + QPointF(job.helix.r, 0);
    int near = 0;
    double nd = std::numeric_limits<double>::max();
    for (int i = 0; i < job.rings.size(); ++i) {
        const double d = distToPoly(from, job.rings.at(i));
        if (d < nd) {
            nd = d;
            near = i;
        }
    }
    const QPolygonF first = startNearest(job.rings.takeAt(near), from);
    job.rings.prepend(first);
}

// A helix that fits in the tool-centre region bounded by `rings` (even-odd,
// so islands are holes in it): about the point farthest inside it (sampled
// on a grid, then refined around the best), radius `want` or less. Off
// when nothing useful fits.
static Job::Helix helixIn(const QList<QPolygonF> &rings, double want, double angle)
{
    Job::Helix h;
    if (rings.isEmpty() || want <= 0)
        return h;
    const QPainterPath region = regionFromRings(rings);
    auto clearance = [&](const QPointF &p) {
        double d = std::numeric_limits<double>::max();
        for (const QPolygonF &r : rings)
            d = qMin(d, distToPoly(p, r));
        return d;
    };
    const QRectF b = region.boundingRect();
    QPointF best;
    double bestD = -1;
    QPointF lo = b.topLeft();
    QSizeF span = b.size();
    for (int round = 0; round < 3; ++round) {
        const int n = 24;
        const QPointF centre = best;
        if (round > 0) {
            span = span / 6.0;
            lo = centre - QPointF(span.width() / 2, span.height() / 2);
        }
        for (int i = 0; i <= n; ++i)
            for (int k = 0; k <= n; ++k) {
                const QPointF p = lo + QPointF(span.width() * i / n, span.height() * k / n);
                if (!region.contains(p))
                    continue;
                const double d = clearance(p);
                if (d > bestD) {
                    bestD = d;
                    best = p;
                }
            }
        if (bestD < 0)
            return h;
    }
    const double r = qMin(want, 0.95 * bestD);
    if (r < 0.05)
        return h;
    h.on = true;
    h.c = best;
    h.r = r;
    h.angle = angle;
    return h;
}

// The same closed ring started at the middle of its longest edge: a lead
// arc tangent to a straight run has room on the air side, where one at a
// convex corner would swing back across the cut it is about to make.
static QPolygonF startMidLongestEdge(const QPolygonF &ring)
{
    if (ring.size() < 4 || ring.first() != ring.last())
        return ring;
    int best = 0;
    double bestLen = -1;
    for (int i = 0; i + 1 < ring.size(); ++i) {
        const double len = QLineF(ring.at(i), ring.at(i + 1)).length();
        if (len > bestLen) {
            bestLen = len;
            best = i;
        }
    }
    QPolygonF out;
    out << (ring.at(best) + ring.at(best + 1)) / 2;
    for (int k = 1; k < ring.size(); ++k)
        out << ring.at((best + k) % (ring.size() - 1));
    out << out.first();
    return out;
}

// keep > 0 (a tabbed contour cut below the tab top): the lead also stays
// `keep` from the ring away from where it joins, so its arcs and the chord
// back across them cut neither the slug through nor a tab's far side.
static Job::Lead makeLead(const QPolygonF &ring, double radius,
                          const std::function<bool(const QPointF &)> &allowed,
                          double keep = 0)
{
    Job::Lead lead;
    if (ring.size() < 3 || radius <= 0.01)
        return lead;
    QVector<double> cum(ring.size(), 0.0);
    for (int i = 1; i < ring.size(); ++i)
        cum[i] = cum[i - 1] + std::hypot(ring.at(i).x() - ring.at(i - 1).x(),
                                         ring.at(i).y() - ring.at(i - 1).y());
    const double ringLen = cum.last();
    // Clear of the ring outside `skip` either side of the start (along it).
    auto farFromRing = [&](const QPointF &p, double skip) {
        for (int i = 1; i < ring.size(); ++i) {
            if (cum[i] <= skip || cum[i - 1] >= ringLen - skip)
                continue;
            const QPointF a = ring.at(i - 1), d = ring.at(i) - a;
            const double l2 = d.x() * d.x() + d.y() * d.y();
            const double segLen = cum[i] - cum[i - 1];
            if (l2 <= 0 || segLen <= 0)
                continue;
            // Only the part of the segment outside the skipped stretch.
            const double u0 = qMax(0.0, (skip - cum[i - 1]) / segLen);
            const double u1 = qMin(1.0, (ringLen - skip - cum[i - 1]) / segLen);
            if (u0 > u1)
                continue;
            const double u = qBound(u0, QPointF::dotProduct(p - a, d) / l2, u1);
            const QPointF q = a + d * u - p;
            if (q.x() * q.x() + q.y() * q.y() < keep * keep)
                return false;
        }
        return true;
    };
    const QPointF s = ring.first();
    QPointF t;
    for (int i = 1; i < ring.size(); ++i) {
        const QPointF d = ring.at(i) - s;
        const double len = std::hypot(d.x(), d.y());
        if (len > 1e-6) {
            t = d / len;
            break;
        }
    }
    if (t.isNull())
        return lead;
    const QPointF left(-t.y(), t.x());
    for (double r = radius; r >= radius / 8 - 1e-12; r /= 2) {
        for (const int side : {1, -1}) {
            const QPointF n = left * side;
            const QPointF c = s + n * r;
            // Left of travel turns counter-clockwise onto the ring.
            const bool cw = side < 0;
            bool fits = true;
            // Samples no more than 0.25 mm apart: an island's tip fits
            // between 16 of them on a large lead.
            const int half = qMax(8, int(std::ceil(M_PI * r / 2 / 0.25)));
            for (int k = 0; k <= 2 * half && fits; ++k) {
                // Both quarter arcs make up the half circle from a through
                // the tangent point to b (which way round the cutter turns
                // does not change where it goes). The tangent point itself
                // sits on the ring and is skipped.
                if (k == half)
                    continue;
                const double a = (k - half) * (M_PI / 2) / half;
                const QPointF u = -n * std::cos(a) + t * std::sin(a);
                fits = allowed(c + u * r);
            }
            // Between passes the cutter feeds back from b to a straight
            // across the circle, at depth: that chord must stay clear too.
            const int across = qMax(2, int(std::ceil(2 * r / 0.25)));
            for (int k = 1; k < across && fits; ++k)
                fits = allowed(c + t * (r - 2 * r * k / across));
            fits = fits && allowed(c);
            if (fits && keep > 0) {
                // Every arc and chord point, bar the ring near the join
                // (within the lead's reach plus `keep`, along the ring).
                const double skip = 2 * r + keep;
                for (int k = 0; k <= 2 * half && fits; ++k) {
                    const double a = (k - half) * (M_PI / 2) / half;
                    fits = farFromRing(c + (-n * std::cos(a) + t * std::sin(a)) * r, skip);
                }
                for (int k = 0; k <= across && fits; ++k)
                    fits = farFromRing(c + t * (r - 2 * r * k / across), skip);
            }
            if (fits) {
                lead.on = true;
                lead.c = c;
                lead.a = c - t * r;
                lead.b = c + t * r;
                lead.cw = cw;
                return lead;
            }
        }
    }
    return lead;
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
        if (ring.size() < 2) {
            plungeTo(pos(), zTo);
            return;
        }
        const double slope = qTan(qDegreesToRadians(qBound(1.0, angleDeg, 45.0)));
        double z = zFrom;
        int at = 0;                                    // vertex we stand on
        const int maxSteps = 20 * (ring.size() - 1);   // ~20 laps, as before
        for (int st = 0; st < maxSteps && z > zTo + 1e-9; ++st) {
            const int next = at + 1 < ring.size() ? at + 1 : 1;  // last == first
            const double L = QLineF(ring.at(at), ring.at(next)).length();
            z = qMax(zTo, z - L * slope);
            m_ops.append(Op::feedTo(ring.at(next).x(), ring.at(next).y(), z, m_plunge));
            m_x = ring.at(next).x();
            m_y = ring.at(next).y();
            at = next;
        }
        // A ring too short for the drop runs out of laps above zTo (a
        // pocket's innermost ring can be well under a millimetre round):
        // the rest goes straight down at the plunge rate, not as a steep
        // dive at the cutting feed on the next move.
        if (z > zTo + 1e-9)
            plungeTo(pos(), zTo);
        // Carry on around the loop at depth. Going straight back to ring[0]
        // from wherever the ramp ended is a chord across the ring: on an
        // outside contour that cuts into the finished part, by the sagitta of
        // the span it skipped - 0.67 mm on a 3.2 mm round join - once per pass.
        if (at > 0 && at < ring.size() - 1)
            followSpan(ring, at + 1, ring.size() - 1, zTo);
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

    // Evenly spaced tabs on a ring of its own: their centres as distances
    // along it, and their width. The width is bounded by the loop, not just
    // by its own parameter: a 20 mm square has an 80 mm perimeter, and three
    // 30 mm tabs tile it completely - the "cut" never goes below tabTop, the
    // part is never released, and the pass then ends with a full-depth plunge
    // inside a tab. Capping the total at a third of the loop keeps more of it
    // cut than uncut and keeps every tab clear of s = 0. None (false) when
    // the part is too small to tab at all.
    static bool tabSpots(const QPolygonF &poly, const CutParams &cp, QVector<double> *at, double *w)
    {
        if (!(cp.tabWidth > 0.01 && poly.isClosed() && poly.size() > 3))
            return false;
        const double len = perimeter(poly);
        const int nTabs = qBound(3, int(len / 150.0) + 3, 8);
        *w = qMin(cp.tabWidth, len / (3.0 * nTabs));
        for (int t = 0; t < nTabs; ++t)
            at->append(len * (t + 0.5) / nTabs);
        return *w >= 1.0;
    }

    // Tabs for one job, placed once on one of its rings (see runJob) and
    // kept as the XY points where each starts and ends: every ring of the
    // job - each depth, every finishing ring, whatever its own length and
    // start - lifts between those two points projected onto it. Placed per ring, a
    // finishing ring 1.5 mm out on a rounded corner spaced its tabs
    // differently and cut straight through the ones the roughing passes
    // had left; kept as a centre and a width, the longer finishing rings
    // of a hole lifted for less of the wall than the roughing ring had.
    //
    // With the part's outline (`edge`) the tabs are spaced and sized on it
    // (or on the ring where that is the shorter side), not on the
    // tool-centre ring alone: the round corner arc of an outside ring all
    // sits against one corner point of the part, so a tab measured there
    // held nothing.
    void placeTabs(const QPolygonF &ring, const CutParams &cp, const QList<QPolygonF> &edge)
    {
        m_tabsPlaced = true;
        m_tabEnds.clear();
        if (!(cp.tabWidth > 0.01 && ring.isClosed() && ring.size() > 3))
            return;
        // Samples of the ring (s along it) and how much of the outline they
        // have passed so far (h): the foot of each sample on the outline.
        QVector<double> ss, hh;
        {
            const QRectF reach = ring.boundingRect();
            QList<QPolygonF> near;
            QVector<double> nearLen;
            for (const QPolygonF &e : edge)
                if (e.size() > 2 && e.boundingRect().adjusted(-1, -1, 1, 1).intersects(reach)) {
                    near << e;
                    nearLen << perimeter(e);
                }
            int lastRing = -1;
            double lastPos = 0, h = 0, s = 0;
            auto sample = [&](const QPointF &q, double segLen) {
                int bestRing = -1;
                double bestPos = 0, bestD = std::numeric_limits<double>::max();
                for (int k = 0; k < near.size(); ++k) {
                    double d = 0;
                    const double pos = arcPosOf(near.at(k), q, &d);
                    if (d < bestD) {
                        bestD = d;
                        bestRing = k;
                        bestPos = pos;
                    }
                }
                if (!ss.isEmpty()) {
                    // What a tab holds is the narrower side of the kerf it
                    // bridges: the outline, or the ring itself where the
                    // outline is the longer side (round a corner of a hole,
                    // which the tool cannot reach anyway).
                    double dh = segLen;
                    if (bestRing >= 0 && bestRing == lastRing) {
                        const double L = nearLen.at(bestRing);
                        const double moved = qAbs(bestPos - lastPos);
                        dh = qMin(segLen, qMin(moved, L - moved));
                    }
                    h += dh;
                }
                ss << s;
                hh << h;
                lastRing = bestRing;
                lastPos = bestPos;
            };
            if (near.isEmpty())
                return placeTabsOnRing(ring, cp);
            sample(ring.first(), 0);
            for (int i = 1; i < ring.size(); ++i) {
                const QLineF seg(ring.at(i - 1), ring.at(i));
                const int n = qMax(1, int(std::ceil(seg.length() / 0.5)));
                for (int k = 1; k <= n; ++k) {
                    s += seg.length() / n;
                    sample(seg.pointAt(double(k) / n), seg.length() / n);
                }
            }
        }
        const double total = hh.last();
        if (total < 1.0)
            return placeTabsOnRing(ring, cp);
        const int nTabs = qBound(3, int(total / 150.0) + 3, 8);
        const double w = qMin(cp.tabWidth, total / (3.0 * nTabs));
        if (w < 1.0) {
            countThin(cp);
            return;
        }
        // Ring position where the outline has been passed for h: inside a
        // step over which it grows, never on a corner arc where it stands.
        auto ringAt = [&](double h) {
            const int k = int(std::lower_bound(hh.begin(), hh.end(), h) - hh.begin());
            if (k <= 0)
                return ss.first();
            if (k >= hh.size())
                return ss.last();
            const double dh = hh.at(k) - hh.at(k - 1);
            const double t = dh > 1e-12 ? (h - hh.at(k - 1)) / dh : 1.0;
            return ss.at(k - 1) + t * (ss.at(k) - ss.at(k - 1));
        };
        const double ringLen = ss.last();
        for (int t = 0; t < nTabs; ++t) {
            const double c = total * (t + 0.5) / nTabs;
            addLift(ring, ringAt(c - w / 2), ringAt(c + w / 2), ringLen, nTabs, w, cp);
        }
    }
    // One lift, over the tab's own span a..b of the ring. It runs a bit
    // radius past each end, so the bridge left across the cut is the tab's
    // width - measured straight between the points where the bit comes back
    // down: a lift round a corner of the ring (a hole's corner, a star's tip)
    // runs on until they are a tab and a bit apart, or the tab slides along
    // the ring (up to a quarter of the spacing) off the corner. The lifts may
    // take up to 40% of the loop (most of it is still cut through); on a part
    // too small for that the bridge is narrower, and one under 1.5 mm (or
    // none: the slug drops free) is counted for a note.
    void addLift(const QPolygonF &ring, double a, double b, double ringLen, int nTabs, double w, const CutParams &cp)
    {
        const double D = 2 * cp.tabToolR, span = qMax(0.0, b - a);
        const double budget = qMax(span, ringLen / (2.5 * nTabs));
        const double e0 = qMax(0.0, qMin(cp.tabToolR, (budget - span) / 2)), eMax = qMax(e0, (budget - span) / 2);
        // The cut is rounder than the ring's points and the lift is not
        // quite square: a bridge read straight off them comes out a little
        // wide, so a fifth of a millimetre more is asked for.
        constexpr double kTabSlack = 0.2;
        // Arc length to point by binary search: a fine ring (a font, a
        // spline) is asked thousands of times here.
        QVector<double> cum(1, 0.0);
        for (int i = 0; i + 1 < ring.size(); ++i)
            cum << cum.last() + QLineF(ring.at(i), ring.at(i + 1)).length();
        auto pointAt = [&](double t) {
            if (ring.size() < 2)
                return ring.isEmpty() ? QPointF() : ring.first();
            const int i = qBound(1, int(std::lower_bound(cum.begin(), cum.end(), t) - cum.begin()), int(cum.size()) - 1);
            const double seg = cum.at(i) - cum.at(i - 1);
            return seg > 1e-12 ? QLineF(ring.at(i - 1), ring.at(i)).pointAt(qBound(0.0, (t - cum.at(i - 1)) / seg, 1.0))
                               : ring.at(i - 1);
        };
        auto ends = [&](double shift, double e) {
            return QLineF(pointAt(qBound(0.0, a + shift - e, ringLen)), pointAt(qBound(0.0, b + shift + e, ringLen)));
        };
        // What is left across the cut: the nearest the bit comes from one
        // side of the lift to the other, over the stretch of ring each side
        // (in a valley of a star the cut beyond the drop point narrows it),
        // less the bit.
        auto bridge = [&](double shift, double e) {
            const double s0 = qBound(0.0, a + shift - e, ringLen), s1 = qBound(0.0, b + shift + e, ringLen);
            const double reach = w + 2 * D;
            QVector<QPointF> left, right;
            for (double t = 0; t <= reach + 1e-9; t += 0.5) {
                left << pointAt(s0 - t >= 0 ? s0 - t : s0 - t + ringLen);
                right << pointAt(s1 + t <= ringLen ? s1 + t : s1 + t - ringLen);
            }
            // each point of one side against the other side's segments (point
            // pairs alone read up to a quarter step wide)
            auto toSegs = [](const QPointF &p, const QVector<QPointF> &side) {
                double d = std::numeric_limits<double>::max();
                for (int i = 0; i + 1 < side.size(); ++i) {
                    const QPointF u = side.at(i + 1) - side.at(i);
                    const double uu = QPointF::dotProduct(u, u);
                    const double t = uu > 1e-12 ? qBound(0.0, QPointF::dotProduct(p - side.at(i), u) / uu, 1.0) : 0.0;
                    d = qMin(d, QLineF(p, side.at(i) + t * u).length());
                }
                return d;
            };
            double best = std::numeric_limits<double>::max();
            for (const QPointF &p : left)
                best = qMin(best, toSegs(p, right));
            for (const QPointF &q : right)
                best = qMin(best, toSegs(q, left));
            // And the rest of the ring may pass beside the lift (the far side
            // of a slot or an arm narrower than two bits): the tab is only
            // what of the lift's centre line no other pass comes within a
            // bit radius of - the longest such stretch.
            QVector<QPointF> mid;
            QRectF box;
            for (double t = s0; t <= s1 + 1e-9; t += 0.25) {
                mid << pointAt(qMin(t, s1));
                box = box.isNull() ? QRectF(mid.last(), QSizeF(0, 0)) : box.united(QRectF(mid.last(), QSizeF(0, 0)));
            }
            box.adjust(-cp.tabToolR - 0.1, -cp.tabToolR - 0.1, cp.tabToolR + 0.1, cp.tabToolR + 0.1);
            QVector<QLineF> cut;
            for (int i = 0; i + 1 < ring.size(); ++i) {
                const QRectF sb = QRectF(ring.at(i), ring.at(i + 1)).normalized();
                if (!sb.adjusted(-1e-6, -1e-6, 1e-6, 1e-6).intersects(box))
                    continue;
                const double c0 = cum.at(i), c1 = cum.at(i + 1);
                if (c0 < s0)
                    cut << QLineF(ring.at(i), c1 <= s0 ? ring.at(i + 1) : pointAt(s0));
                if (c1 > s1)
                    cut << QLineF(c0 >= s1 ? ring.at(i) : pointAt(s1), ring.at(i + 1));
            }
            int run = 0, longest = 0;
            for (const QPointF &p : mid) {
                double d = std::numeric_limits<double>::max();
                for (const QLineF &c : cut) {
                    const QPointF u = c.p2() - c.p1();
                    const double uu = QPointF::dotProduct(u, u);
                    const double t = uu > 1e-12 ? qBound(0.0, QPointF::dotProduct(p - c.p1(), u) / uu, 1.0) : 0.0;
                    d = qMin(d, QLineF(p, c.p1() + t * u).length());
                }
                run = d >= cp.tabToolR - 1e-9 ? run + 1 : 0;
                longest = qMax(longest, run);
            }
            const double alone = longest > 0 ? (longest - 1) * 0.25 : -D;
            return qMin(best - D, alone);
        };
        // The pass starts at the ring's start (a plunge or ramp, a bit wide):
        // a lift reaching it had that cut through its tab.
        const double clear = qMax(cp.tabToolR, cp.tabLeadR) + 0.5;
        auto room = [&](double shift) { return qMin(eMax, qMin(a + shift - clear, ringLen - clear - (b + shift))); };
        // The shortest lift at this shift that leaves a full bridge, if any.
        auto fit = [&](double shift, QLineF *l) {
            const double top = room(shift);
            for (double e = qMin(e0, top);; e = qMin(e + 0.1, top)) {
                if (bridge(shift, e) >= w + kTabSlack) {
                    *l = ends(shift, e);
                    return true;
                }
                if (e >= top)
                    return false;
            }
        };
        const double maxShift = ringLen / (4.0 * nTabs);
        QLineF l;
        // Where nothing leaves a full bridge (a small part, whose budget is
        // under a tab and a bit), the place that leaves the most: shift 0
        // was the corner the search had been moving away from.
        double bestShift = 0, best = -std::numeric_limits<double>::max();
        for (double k = 0; k <= maxShift + 1e-9; k += 0.5)
            for (double shift : {k, -k}) {
                if (room(shift) < 0)
                    continue;
                // The longest lift here first: when even that leaves no full
                // bridge (a slot, the far side always near) the shorter ones
                // are not tried - on a long ring that was minutes.
                const double br = bridge(shift, room(shift));
                if (br >= w + kTabSlack && fit(shift, &l)) {
                    m_tabEnds.append({l.p1(), l.p2()});
                    return;
                }
                if (br > best + 1e-9) {
                    best = br;
                    bestShift = shift;
                }
            }
        double eBest = qMax(0.0, room(bestShift));
        if (best == -std::numeric_limits<double>::max()) {
            eBest = eMax;   // no room clear of the start anywhere: as before
            best = bridge(0, eMax);
        }
        l = ends(bestShift, eBest);
        if (best < qMin(1.5, w) + kTabSlack)
            countThin(cp);
        m_tabEnds.append({l.p1(), l.p2()});
    }
    // No outline to measure on: tabs spaced and sized along the ring itself.
    void placeTabsOnRing(const QPolygonF &ring, const CutParams &cp)
    {
        QVector<double> at;
        double w = 0;
        if (tabSpots(ring, cp, &at, &w)) {
            const double len = perimeter(ring);
            for (double c : at)
                addLift(ring, c - w / 2, c + w / 2, len, int(at.size()), w, cp);
        } else if (!at.isEmpty()) {
            countThin(cp);   // a loop too short for any tab
        }
    }
    void forgetTabs() { m_tabsPlaced = false; m_tabEnds.clear(); }
    int thinTabs() const { return m_thinTabs; }
    // Only where tabs are cut at all (the contour goes below their top).
    void countThin(const CutParams &cp)
    {
        if (cp.tabWidth > 0.01 && cp.zBot < cp.tabTop - 1e-9)
            ++m_thinTabs;
    }

    // Whether followRing lifts over tabs on this ring at depth z.
    bool hasTabs(const QPolygonF &poly, double z, const CutParams &cp) const
    {
        if (!(z < cp.tabTop - 1e-9 && cp.tabWidth > 0.01 && poly.isClosed() && poly.size() > 3))
            return false;
        if (m_tabsPlaced)
            return !m_tabEnds.isEmpty();
        QVector<double> at;
        double w = 0;
        return tabSpots(poly, cp, &at, &w);
    }

    // Follow one closed/open ring at depth z; arcs for circles; tab lifts when
    // z is below tabTop and tabWidth > 0.
    void followRing(const QPolygonF &poly, double z, const CutParams &cp)
    {
        if (!hasTabs(poly, z, cp)) {
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

        // Tabbed pass: the tabs (see tabSpots / placeTabs) crossed at
        // cp.tabTop instead of z. Each tab is the stretch [from, to] along
        // this ring, wrapping round its start when to < from.
        const double len = perimeter(poly);
        QVector<QPair<double, double>> spans;
        if (m_tabsPlaced) {
            for (const auto &e : m_tabEnds) {
                double from = arcPosOf(poly, e.first), to = arcPosOf(poly, e.second);
                // Projected the wrong way round (a ring that doubles back
                // near the tab): the shorter of the two stretches.
                const double ahead = to >= from ? to - from : len - from + to;
                if (ahead > len / 2)
                    std::swap(from, to);
                // The ring's own lift comes down as far apart as the one the
                // tab was placed on: projected onto a roughing ring inside a
                // corner, its ends came closer and the bit cut the bridge
                // thin there.
                // Never over the ring's start: the pass begins there, and a
                // plunge inside a tab cut it away.
                const double want = QLineF(e.first, e.second).length();
                if (from <= to) {
                    for (double grown = 0; grown < len / (4.0 * m_tabEnds.size())
                                           && QLineF(pointAtArc(poly, from), pointAtArc(poly, to)).length() < want - 1e-6;
                         grown += 0.1) {
                        const bool l = from - 0.1 > 0.5, r = to + 0.1 < len - 0.5;
                        if (!l && !r)
                            break;
                        if (l)
                            from -= 0.1;
                        if (r)
                            to += 0.1;
                    }
                }
                spans.append({from, to});
            }
        } else {
            QVector<double> centres;
            double tabW = 0;
            tabSpots(poly, cp, &centres, &tabW);
            for (const double c : centres)
                spans.append({c - tabW / 2, c + tabW / 2});
        }
        auto inTab = [&](double s) {
            for (const auto &sp : spans) {
                if (sp.first <= sp.second ? s >= sp.first && s <= sp.second
                                          : s >= sp.first || s <= sp.second)
                    return true;
            }
            return false;
        };
        // Each edge split exactly where a tab starts or ends, so the lift
        // and the drop come at the tab's ends, not at the next millimetre.
        QVector<double> ends;
        for (const auto &sp : spans)
            for (const double e : {sp.first, sp.second})
                ends << e - len * std::floor(e / len);
        double s = 0;
        bool up = false;
        for (int i = 1; i < poly.size(); ++i) {
            const QLineF seg(poly.at(i - 1), poly.at(i));
            const double L = seg.length();
            if (L < 1e-12)
                continue;
            QVector<double> cuts;
            for (const double e : ends)
                if (e > s + 1e-9 && e < s + L - 1e-9)
                    cuts << e - s;
            std::sort(cuts.begin(), cuts.end());
            cuts << L;
            double done = 0;
            for (const double t : cuts) {
                const bool tab = inTab(s + (done + t) / 2);
                done = t;
                if (tab != up) {
                    up = tab;
                    m_ops.append(Op::feedTo(m_x, m_y, up ? cp.tabTop : z,
                                            up ? m_feed : m_plunge));
                }
                const QPointF p = seg.pointAt(t / L);
                m_ops.append(Op::feedTo(p.x(), p.y(), up ? cp.tabTop : z, m_feed));
                m_x = p.x();
                m_y = p.y();
            }
            s += L;
        }
        // Deliberately no descent here. The tab spacing above keeps every tab
        // clear of s = 0, so a pass cannot end inside one; if it ever did,
        // dropping to full depth would drill a hole through the tab that is
        // holding the part.
    }

    void retract()
    {
        // Nothing cut yet: the emitter starts at X0 Y0, and a retract there
        // would rapid the bit across to the origin before the first move.
        if (m_ops.isEmpty())
            return;
        m_ops.append(Op::rapid(m_x, m_y, m_safeZ));
    }

    // Rapid to an explicit Z (never above the safe height): the low hops of
    // adaptive clearing, over ground already cut.
    void rapidAt(const QPointF &p, double z)
    {
        m_ops.append(Op::rapid(p.x(), p.y(), qMin(z, m_safeZ)));
        m_x = p.x();
        m_y = p.y();
    }

    // An arc (helical when z differs from the current depth) around ctr.
    void arcTo(const QPointF &to, double z, const QPointF &ctr, bool cw)
    {
        m_ops.append(Op::arcTo(to.x(), to.y(), z, ctr.x() - m_x, ctr.y() - m_y, cw, m_feed));
        m_x = to.x();
        m_y = to.y();
    }

    // A closed contour with lead arcs: each pass drops in at the lead's
    // start (on the air side), arcs onto the ring, goes round, arcs off
    // again, and feeds back across the lead circle to drop for the next
    // pass. Ramping takes the place of the leads (see makeLead's caller).
    void runLeadJob(const Job &job, const CutParams &cp)
    {
        const Job::Lead &l = job.lead;
        const QPolygonF &ring = job.rings.first();
        rapidTo(l.a);
        double z = cp.zTop;
        bool more = true;
        while (more) {
            z = qMax(cp.zBot, z - cp.stepdown);
            more = z > cp.zBot + 1e-9;
            plungeTo(l.a, z);
            arcTo(ring.first(), z, l.c, l.cw);
            followRing(ring, z, cp);
            arcTo(l.b, z, l.c, l.cw);
            if (more)
                feedAt(l.a, z, m_feed);
        }
        for (int i = 0; i < job.finish.size(); ++i) {
            const QPolygonF &fr = job.finish.at(i);
            const Job::Lead &fl = job.finishLeads.value(i);
            if (fl.on) {
                feedAt(fl.a, z, m_feed);
                arcTo(fr.first(), z, fl.c, fl.cw);
                followRing(fr, z, cp);
                arcTo(fl.b, z, fl.c, fl.cw);
            } else {
                feedAt(fr.first(), z, m_feed);
                followRing(fr, z, cp);
            }
        }
        retract();
    }

    // A pocket job entered on a helix at every depth: drop to the floor
    // just cleared over the helix start, spiral down to the new depth, take
    // one flat lap, then cut the rings innermost first. The helix sits in
    // the innermost ring, which is clear of the walls by the tool radius.
    void runHelixJob(const Job &job, const CutParams &cp)
    {
        const Job::Helix &h = job.helix;
        const QPointF p0 = h.c + QPointF(h.r, 0), p1 = h.c - QPointF(h.r, 0);
        // Depth lost per half turn, never so small the helix spins for ever.
        const double drop = qMax(0.02, M_PI * h.r * qTan(qDegreesToRadians(qBound(0.5, h.angle, 45.0))));
        rapidTo(p0);
        double z = cp.zTop;
        bool first = true, more = true;
        while (more) {
            const double zPrev = z;
            z = qMax(cp.zBot, z - cp.stepdown);
            more = z > cp.zBot + 1e-9;
            if (!first) {
                retract();
                rapidTo(p0);
            }
            first = false;
            plungeTo(p0, zPrev);
            double hz = zPrev;
            bool atP0 = true;
            for (int k = 0; k < 100000 && hz > z + 1e-9; ++k) {
                hz = qMax(z, hz - drop);
                arcTo(atP0 ? p1 : p0, hz, h.c, false);
                atP0 = !atP0;
            }
            arcTo(atP0 ? p1 : p0, z, h.c, false);   // flat lap at depth
            arcTo(atP0 ? p0 : p1, z, h.c, false);
            for (int i = 0; i < job.rings.size(); ++i) {
                // Each ring entered where it is nearest: from its own first
                // vertex the next ring was often past the link limit, a
                // retract and a plunge into solid stock at every depth.
                const QPolygonF ring = startNearest(job.rings.at(i), pos());
                const QPointF rs = ring.first();
                if (QLineF(pos(), rs).length() > 1e-6) {
                    // The first ring was chosen as the one nearest the helix
                    // (pocket branch): straight across to it, whatever the
                    // distance, unless that crosses an island.
                    if ((i == 0 || QLineF(pos(), rs).length() <= cp.linkDist)
                        && (!cp.linkOk || cp.linkOk(pos(), rs))) {
                        feedAt(rs, z, m_feed);
                    } else {
                        retract();
                        rapidTo(rs);
                        plungeTo(rs, z);
                    }
                }
                followRing(ring, z, cp);
            }
        }
        retract();
    }

    void runJob(const Job &job, const CutParams &cp)
    {
        // A contour's one roughing ring and its finishing rings share their
        // tabs; separate loops (pocket rings) each keep their own.
        // They are placed on the ring nearest the part, the last finishing
        // ring when there is one: that ring's cut is the one that frees the
        // part, so it is the one the tab widths are measured on.
        if (job.closed && job.rings.size() == 1) {
            CutParams at = cp;
            if (job.lead.on)
                at.tabLeadR = QLineF(job.lead.c, job.lead.a).length();
            placeTabs(job.finish.isEmpty() ? job.rings.first() : job.finish.last(), at, job.edge);
        }
        runJobRings(job, cp);
        forgetTabs();
    }

    void runJobRings(const Job &job, const CutParams &cp)
    {
        if (job.lead.on && job.closed && job.rings.size() == 1) {
            runLeadJob(job, cp);
            return;
        }
        if (job.helix.on && job.closed) {
            runHelixJob(job, cp);
            return;
        }
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

            // Pocket rings are entered where they are nearest (see
            // runHelixJob); a lone ring keeps its start, where its tabs and
            // lead are measured from.
            const bool nearest = job.closed && job.rings.size() > 1;
            const QPolygonF firstRing = nearest && !first ? startNearest(job.rings.at(order.first()), pos())
                                                          : job.rings.at(order.first());
            // A ramp runs the loop at depth from vertex 0 and knows nothing
            // of tabs: on a pass below the tab tops it would cut through every
            // tab before the tabbed lap below lifts over them. Plunge there
            // instead (vertex 0 is never inside a tab).
            const bool ramp = cp.rampAngle > 0 && !hasTabs(firstRing, z, cp);
            if (first) {
                if (ramp && job.closed) {
                    plungeTo(firstRing.first(), cp.zTop);
                    rampDescend(firstRing, cp.zTop, z, cp.rampAngle);
                } else {
                    plungeTo(firstRing.first(), z);
                }
                first = false;
            } else if (job.closed) {
                if (ramp)
                    rampDescend(firstRing, z + cp.stepdown, z, cp.rampAngle);
                else
                    plungeTo(pos(), z);   // in place — we ended here last pass
            } else {
                retract();
                rapidTo(job.start());
                plungeTo(job.start(), z);
            }

            for (int oi = 0; oi < order.size(); ++oi) {
                const QPolygonF ring = oi == 0 ? firstRing
                                     : nearest ? startNearest(job.rings.at(order.at(oi)), pos())
                                               : job.rings.at(order.at(oi));
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
        // Finishing rings at the final depth, each started next to where
        // the last cut ended (see the contour branch).
        for (const QPolygonF &fr : job.finish) {
            feedAt(fr.first(), z, m_feed);
            followRing(fr, z, cp);
        }
        retract();
    }

private:
    QVector<Op> &m_ops;
    double m_safeZ, m_feed, m_plunge;
    double m_x = 0, m_y = 0;
    int m_thinTabs = 0;          // loops whose tabs leave under 1.5 mm across the cut
    bool m_tabsPlaced = false;   // placeTabs: m_tabEnds holds this job's tabs
    QVector<QPair<QPointF, QPointF>> m_tabEnds;   // each tab's start and end on the ring it was placed on
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

// The tool centres' box grown by `pad`. Rest depth counts a later cut as in
// cut air anywhere the earlier cuts cleared (their centres plus the tool
// radius), even across the seam where two of them meet, so a job depends on
// every earlier one whose centres come within a tool radius of its own. The
// pad also gives a straight horizontal or vertical line an area, which
// QRectF needs to count it as overlapping.
static QRectF jobBounds(const Job &j, double pad)
{
    QRectF r;
    for (const QPolygonF &p : j.rings)
        r |= p.boundingRect();
    return r.isNull() ? r : r.adjusted(-pad, -pad, pad, pad);
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
    double reach = 0.01;   // the tool radius (+0.01): how far its cut reaches past the centres
};

// Whether a closed loop of `outer` goes round where `inner` starts: a hole
// in a part, selected with its outline in one toolpath. The outline cut
// through first left the part loose for the hole (moved, or thrown). Only
// within one rest band: a deeper band's rings go round the shallower one's
// start, and it was cut first, plunging through what that band had to clear.
static bool encloses(const Job &outer, const Job &inner)
{
    if (!outer.closed || !inner.closed || inner.rings.isEmpty())
        return false;
    const QPointF p = inner.start();
    for (const QPolygonF &r : outer.rings)
        if (r.size() > 3 && r.boundingRect().contains(p) && r.containsPoint(p, Qt::OddEvenFill))
            return true;
    return false;
}

// Nearest-neighbor over all deferred jobs, with two rules: a job may not run
// while an earlier toolpath still has an overlapping job pending, nor while
// a loop of its own toolpath inside it (a hole in the part) is pending. So at a
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
        // Bounded: loops whose starts each lie inside the other (crossing
        // outlines) must not send this round for ever.
        for (int round = 0, again = 1; again && round <= jobs.size(); ++round) {
            again = 0;
            const QRectF b = jobBounds(jobs.at(best).job, jobs.at(best).reach);
            int pick = -1;
            double pickD = 0;
            for (int i = 0; i < jobs.size(); ++i) {
                const Deferred &a = jobs.at(i), &o = jobs.at(best);
                const bool earlier = a.tp < o.tp || (a.tp == o.tp && a.job.band < o.job.band)
                                     || (a.tp == o.tp && a.job.band == o.job.band && i != best
                                         && encloses(o.job, a.job));
                if (!earlier
                    || !jobBounds(jobs.at(i).job, jobs.at(i).reach).intersects(b))
                    continue;
                const double d = QLineF(from, jobs.at(i).job.start()).length();
                if (pick < 0 || d < pickD) {
                    pick = i;
                    pickD = d;
                }
            }
            if (pick >= 0) {
                best = pick;
                again = 1;
            }
        }
        from = jobs.at(best).job.start();
        out.append(jobs.takeAt(best));
    }
    return out;
}

} // namespace

QString toolReuseWarning(const QVector<Op> &ops)
{
    // By bit (Op::text), so one number used for two bits is two tools.
    QList<QPair<QString, int>> seq;
    for (const Op &o : ops) {
        const QPair<QString, int> t{o.text.isEmpty() ? QString::number(o.ival) : o.text, o.ival};
        if (o.kind == Op::Tool && (seq.isEmpty() || seq.last().first != t.first))
            seq << t;
    }
    QStringList distinct;
    QStringList again;
    for (int i = 0; i < seq.size(); ++i) {
        if (distinct.contains(seq.at(i).first))
            again << QStringLiteral("T%1 after T%2").arg(seq.at(i).second).arg(seq.at(i - 1).second);
        else
            distinct << seq.at(i).first;
    }
    if (again.isEmpty())
        return QString();
    return QStringLiteral("%1 tool change(s) where %2 would do: %3 is picked up again. "
                          "Put the toolpaths that share a tool next to each other.")
        .arg(seq.size() - 1).arg(distinct.size() - 1).arg(again.join(QStringLiteral(", ")));
}

double documentSafeZ(const Document &doc)
{
    const double z = doc.params().value(QStringLiteral("retract"),
                                        QStringLiteral("2.54")).toDouble();
    return (!std::isfinite(z) || z < 0.5) ? 2.54 : z;
}

GcodeResult exportGcode(Document &doc, const ExportWatch *watch,
                        const HeightModel *relief)
{
    GcodeResult res;
    QVector<Op> ops;
    const double safeZ = documentSafeZ(doc);
    QPointF lastPos(0, 0);

    // Tool changes go by the bit, not just its number: two toolpaths that
    // both say T1 but with different bits (an edited diameter, two tools
    // numbered 0) still get the pause to swap them.
    QString lastTool;
    int tpIndex = 0;
    QStringList tpUuids;   // by toolpath index, for doneIds

    // Ring-type toolpaths (pocket / contour / cutout) are not emitted one by
    // one: consecutive ones sharing a tool are pooled and emitted together,
    // ordered by orderDeferred(), so each site is finished before moving on.
    QList<Deferred> pending;
    Op pendingTool = Op::tool(-1);
    auto flush = [&]() {
        if (pending.isEmpty())
            return;
        QVector<Op> body;
        Emitter em(body, safeZ, 0, 0);
        int curTp = -1;
        QStringList names;
        for (const Deferred &d : orderDeferred(pending, lastPos)) {
            if (d.tp != curTp) {
                body.append(Op::comment(d.name));
                // Every toolpath carries its spindle start, even at an
                // unchanged rpm (the post drops the repeat): a tile that keeps
                // this toolpath but not the one before it must still start it.
                body.append(Op::spindle(d.rpm));
                em.setFeeds(d.feed, d.plunge);
                curTp = d.tp;
                if (!names.contains(d.name))
                    names << d.name;
                if (!res.doneIds.contains(tpUuids.value(d.tp)))
                    res.doneIds << tpUuids.value(d.tp);
            }
            const int thinBefore = em.thinTabs();
            em.runJob(d.job, d.cp);
            lastPos = em.pos();
            if (em.thinTabs() > thinBefore) {
                const QString n = QStringLiteral("%1: part too small for its tabs with this bit (under 1.5 mm "
                                                 "left across the cut; it may come free)").arg(d.name);
                if (!res.notes.contains(n))
                    res.notes << n;
            }
        }
        if (pendingTool.text != lastTool) {
            ops.append(pendingTool);
            lastTool = pendingTool.text;
        }
        // No spindle stop here: the next toolpath with the same tool keeps it
        // running; the post stops it before a tool change and at the end.
        ops.append(body);
        res.done << names;
        pending.clear();
        pendingTool = Op::tool(-1);
    };

    QList<Cleared> cleared;   // what the toolpaths so far have removed (rest depth)
    // Cut order: the toolpath list, top to bottom (each toolpath's own jobs
    // still run nearest-first). With params.site_order = "1" consecutive
    // same-tool toolpaths are pooled and every site is finished before the
    // spindle moves on (counterbore, then the through-hole beside it).
    const bool siteOrder = doc.params().value(QStringLiteral("site_order")) == QLatin1String("1");
    const int tpTotal = int(doc.toolpaths().size());
    for (const Toolpath &t : doc.toolpaths()) {
        const QJsonObject j = t.json;
        if (!j.value("enabled").toBool(true))
            continue;
        const QString name = j.value("name").toString();
        const int myIndex = tpIndex++;
        tpUuids << t.uuid;
        if (watch && watch->stop(myIndex, tpTotal, name)) {
            res.cancelled = true;
            break;
        }

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
        const bool face = (t.type == QLatin1String("face_toolpath"));
        const bool bore = (t.type == QLatin1String("bore_toolpath"));
        const bool chamfer = (t.type == QLatin1String("chamfer_toolpath"));
        const bool adaptive = (t.type == QLatin1String("adaptive_toolpath"));
        const bool thread = (t.type == QLatin1String("thread_toolpath"));
        const bool slotTp = (t.type == QLatin1String("slot_toolpath"));
        const bool circular = (t.type == QLatin1String("circular_toolpath"));
        if (!contour && !pocket && !cutout && !drilling && !keyhole && !texture
            && !vcarve && !engrave && !rough3d && !finish3d && !face && !bore && !chamfer
            && !adaptive && !thread && !slotTp && !circular) {
            res.skipped << QStringLiteral("%1 (%2 not supported yet)")
                               .arg(name, t.type);
            continue;
        }

        const QJsonObject speeds = j.value("speeds").toObject();
        const double feed = numOr(speeds.value("feedrate"), 500);
        const double plunge = numOr(speeds.value("plungerate"), 100);
        const int rpm = int(numOr(speeds.value("rpm"), 10000));
        if (!(feed > 0) || !(plunge > 0)) {
            // GRBL refuses F0 or a negative F mid-job, after the spindle
            // is already running.
            res.skipped << QStringLiteral("%1 (feed and plunge rate must be above 0)").arg(name);
            continue;
        }
        if (!(rpm > 0)) {
            // Op::spindle(0) is M05: the toolpath would cut with the spindle
            // stopped (form minimum 0, or a toolpath with no rpm of its own).
            res.skipped << QStringLiteral("%1 (spindle speed must be above 0)").arg(name);
            continue;
        }
        const QJsonObject tool = j.value("tool").toObject();
        const Op toolChange = toolOp(tool);
        // The bit's size, read once and the same way as toolGeomOf() reads it
        // for the tool change and the simulation. A missing, zero or negative
        // diameter is refused: toolGeomOf() stands in 3.175 for it, and the
        // offsets here used to take 6 (missing) or the bad value itself (0
        // ran the tool centre on the vector line), so the cut, the tool key
        // and the simulation each assumed a different bit.
        if (!(numOr(tool.value("diameter"), 0) > 0)) {
            res.skipped << QStringLiteral("%1 (tool diameter must be above 0)").arg(name);
            continue;
        }
        const double toolR = toolGeomOf(tool).radius();
        // An offset is a tool radius off the vectors; a V-bit cuts far
        // narrower than its radius at a shallow depth, so the part came out
        // oversize (a pocket undersize, with strips left between the rings).
        if ((pocket || cutout || adaptive || circular || bore || face
             || (contour && j.value("ofset_dir").toInt(0) != 0))
            && toolGeomOf(tool).kind == ToolGeom::VBit) {
            res.skipped << QStringLiteral("%1 (needs an end mill: a V-bit only follows the line)").arg(name);
            continue;
        }

        // Every depth this toolpath reads, with one flag for whether they were
        // all readable. A toolpath whose depth we cannot read is refused, not
        // guessed at: the wrong guess is a wrong cut.
        bool depthsOk = true;
        auto depth = [&](const QJsonValue &v) {
            bool one = true;
            const double z = depthToZ(v, &one);
            if (!one)
                depthsOk = false;
            return z;
        };

        CutParams cp;
        cp.zTop = depth(j.value("start_depth"));
        // Tabs are a contour feature as much as a cutout one - CC offers them
        // on both, and makeToolpath() creates contours with them switched on.
        // Reading them for cutouts only meant a contour taken to the full
        // stock thickness released the part on its last pass, spindle running.
        // The tab height is measured from `floor`, the cut depth itself: a
        // cutout's break-through goes into the spoilboard under the part,
        // and counting it made every tab thinner by that much.
        auto readTabs = [&](double floor) {
            if (j.value("ignore_tabs").toBool(true)
                || numOr(j.value("tab_height"), 0) <= 0.01)
                return;
            cp.tabTop = floor + numOr(j.value("tab_height"), 0);
            cp.tabWidth = qBound(1.0, numOr(j.value("tab_width"), 6.0), 30.0);
            cp.tabToolR = qMax(0.0, numOr(j.value("tool").toObject().value("diameter"), 3.175) / 2);
        };
        if (cutout) {
            const double floor = depth(j.value("cut_depth"));
            cp.zBot = floor + depth(j.value("break_through"));
            cp.stepdown = qMax(0.05, numOr(j.value("depth_per_pass"), 1.0));
            readTabs(floor);
        } else {
            cp.zBot = depth(j.value("end_depth"));
            cp.stepdown = qMax(0.05, numOr(j.value("stepdown"), 1.0));
            if (contour)
                readTabs(cp.zBot);
        }
        if (j.value("enable_ramping").toBool(false))
            cp.rampAngle = qBound(1.0, numOr(j.value("ramp_angle"), 20.0), 45.0);

        if (!depthsOk) {
            res.skipped << QStringLiteral("%1 (cut depth is not a number)").arg(name);
            continue;
        }
        // A start below the end is no cut either, and each type made
        // something different of it: bore, circular and slot plunged to the
        // start (deeper than the end asked for), a pocket cut at the end,
        // drilling emitted nothing and still reported done. (3D toolpaths
        // go by the relief and max_depth, not these two.)
        if (cp.zTop < cp.zBot - 1e-9 && !rough3d && !finish3d) {
            res.skipped << QStringLiteral("%1 (start depth is below end depth)").arg(name);
            continue;
        }
        // A readable but absurd depth is still not machinable, and the pass
        // loop below steps down by `stepdown` until it reaches the bottom: a
        // depth of 1e15 mm is an export that never returns. No real job is
        // anywhere near this many passes.
        const double passes = (cp.zTop - cp.zBot) / cp.stepdown;
        if (!(passes < 100000.0)) {
            res.skipped << QStringLiteral("%1 (cut depth needs too many passes)").arg(name);
            continue;
        }

        if (rough3d || finish3d) {
            // 3D toolpaths over the modeller's relief (cam3d.cpp). Emitted on
            // their own, never pooled with the 2D ring jobs around them.
            const HeightModel *hm = relief ? relief : heightModelFor(doc);
            if (!hm || !hm->valid()) {
                res.skipped << QStringLiteral("%1 (no 3D model)").arg(name);
                continue;
            }
            bool maxDepthOk = true;
            const Cam3dParams p3 = cam3dParams(j, safeZ, &maxDepthOk);
            if (!maxDepthOk) {
                res.skipped << QStringLiteral("%1 (max depth is not a number)").arg(name);
                continue;
            }
            // The boundary is an area: an open vector taken as closed by its
            // end points bounded a shape that was never drawn. Left out, as
            // for the 2D region types; with only open ones the job is
            // skipped rather than run over the whole model.
            QVector<const Element *> belems;
            const QVector<const Element *> refd = referenced(doc, j);
            for (const Element *e : refd)
                if (vec::isClosed(*e))
                    belems.append(e);
            if (!refd.isEmpty() && belems.isEmpty()) {
                res.skipped << QStringLiteral("%1 (needs closed vectors, open vectors ignored)").arg(name);
                continue;
            }
            if (belems.size() < refd.size())
                res.notes << QStringLiteral("%1: %2 open vector(s) ignored")
                                 .arg(name, QString::number(refd.size() - belems.size()));
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
                bool capped = false;
                for (const RoughLevel &lv : roughLevels(*hm, p3, boundary, watch)) {
                    QList<Job> jobs;
                    for (const QPainterPath &comp : components(lv.region))
                        capped |= !ringFillJob(comp, r + p3.stockToLeave, p3.stepover, jobs, r);
                    CutParams lcp = cp;
                    lcp.zTop = lv.z + p3.stepdown;
                    lcp.zBot = lv.z;
                    lcp.stepdown = p3.stepdown;
                    lcp.linkDist = qMax(2.5 * p3.stepover, 4 * r);
                    // Clear of the relief is not enough: a hole in the
                    // boundary is a keep-out the relief knows nothing of, so
                    // the link must also stay where the tool centre may go.
                    const auto inLevel = linkInside(
                        regionFromRings(insetRings(lv.region, qMax(0.0, r - kLinkLeeway))));
                    lcp.linkOk = [&, inLevel, z = lv.z](const QPointF &a, const QPointF &b) {
                        return inLevel(a, b) && roughLinkClear(*hm, p3, a, b, z);
                    };
                    for (const Job &job : orderJobs(jobs, em.pos()))
                        em.runJob(job, lcp);
                }
                if (capped)
                    res.notes << QStringLiteral("%1: too many clearing rings for the stepover, "
                                                "the middle of a level is left uncut").arg(name);
                lastPos = em.pos();
            } else {
                // Boundary vectors keep the whole tool inside, as for the
                // rough (its rings are inset by the tool radius): a finish
                // with only its centre clipped to them ran a tool radius
                // outside, full depth in stock the rough never cleared.
                // With none, the boundary is the model's edge and the
                // overhang is over the board's edge, in air.
                const QPainterPath inside =
                    belems.isEmpty() ? boundary : cam3dBoundary(*hm, boundary, -p3.tool.radius());
                body = finishOps(*hm, p3, inside, nullptr, watch);
                for (int k = body.size() - 1; k >= 0; --k)
                    if (body.at(k).kind == Op::Feed) {
                        lastPos = QPointF(body.at(k).x, body.at(k).y);
                        break;
                    }
            }
            // A 3D pass can be abandoned part-way, in which case `body` holds
            // only what was finished. Do not emit half a toolpath.
            if (watch && watch->stop(myIndex, tpTotal, name)) {
                res.cancelled = true;
                break;
            }
            if (body.isEmpty()) {
                res.skipped << QStringLiteral("%1 (nothing to cut)").arg(name);
                continue;
            }
            if (toolChange.text != lastTool) {
                ops.append(toolChange);
                lastTool = toolChange.text;
            }
            ops.append(Op::comment(name));
            ops.append(Op::spindle(rpm));
            ops.append(body);
            res.done << name;
            res.doneIds << t.uuid;
            continue;
        }

        QVector<const Element *> elems = referenced(doc, j);
        // Facing works on the stock when it has no vectors of its own.
        const QRectF stock(0, 0, doc.boardWidth(), doc.boardHeight());
        if (elems.isEmpty() && !(face && stock.width() > 0 && stock.height() > 0)) {
            res.skipped << QStringLiteral("%1 (no vectors)").arg(name);
            continue;
        }
        // Region types cut inside or around an area: an open vector has
        // none. Taken as a polygon closed by its first and last points it
        // pocketed, contoured or carved a shape that was never drawn. (A
        // contour on the line follows open vectors as they are.) A texture
        // fills an area too.
        if (pocket || (contour && j.value("ofset_dir").toInt(0) != 0) || cutout || vcarve
            || texture) {
            QVector<const Element *> closedEls;
            for (const Element *e : elems)
                if (vec::isClosed(*e))
                    closedEls.append(e);
            if (closedEls.isEmpty()) {
                res.skipped << QStringLiteral("%1 (needs closed vectors, open vectors ignored)").arg(name);
                continue;
            }
            if (closedEls.size() < elems.size()) {
                res.notes << QStringLiteral("%1: %2 open vector(s) ignored")
                                 .arg(name, QString::number(elems.size() - closedEls.size()));
                elems = closedEls;
            }
        }

        QVector<Op> body;
        Emitter em(body, safeZ, feed, plunge);

        QList<Job> jobs;
        QList<QPainterPath> pocketMine;   // pockets: tool-centre regions, for rest bands
        QList<std::function<bool(const QPointF &, const QPointF &)>> bandLinks;   // per rest band
        double pocketStep = 1.0;
        if (face) {
            // FACE (phobiCCCP type, Fusion's Face): zig-zag passes along X
            // over the stock, or over the bounding box of the vectors given,
            // `stepover` apart, each run carried a tool radius + 1 mm past
            // both ends so the edges come out clean and every plunge is in
            // the air beside the stock.
            QRectF area;
            for (const Element *e : elems)
                area = area.isNull() ? e->painterPath.boundingRect()
                                     : area.united(e->painterPath.boundingRect());
            if (elems.isEmpty())
                area = stock;
            const double stepover = qBound(0.05, numOr(j.value("stepover"), toolR), 2 * toolR);
            double x0 = area.left() - toolR - 1, x1 = area.right() + toolR + 1;
            if (!elems.isEmpty()) {
                // The box around the vectors is the face: the tool stays
                // inside it (it may stand in the middle of the stock, so its
                // plunges are not in the air there).
                const double inX = qMin(toolR, area.width() / 2), inY = qMin(toolR, area.height() / 2);
                area.adjust(0, inY, 0, -inY);
                x0 = area.left() + inX;
                x1 = area.right() - inX;
            }
            // Rows from one edge to the other, the outermost exactly on the
            // edges, evenly spread (never wider than the stepover asked for).
            const int rows = qMax(1, int(std::ceil(area.height() / stepover - 1e-9))) + 1;
            QPolygonF zig;
            for (int k = 0; k < rows; ++k) {
                const double y = rows == 1 ? area.center().y()
                                           : area.top() + area.height() * k / (rows - 1);
                const bool fwd = (k % 2) == 0;
                zig << QPointF(fwd ? x0 : x1, y) << QPointF(fwd ? x1 : x0, y);
            }
            Job job;
            job.rings.append(zig);
            job.closed = false;
            jobs.append(job);
        } else if (bore) {
            // BORE (phobiCCCP type, Fusion's Bore): a helix down the wall of
            // each circle, `stepdown` per turn, one flat lap at the bottom,
            // then out through the center. Climb (counter-clockwise inside a
            // hole) unless climb is false. Only the wall is cut: a blind hole
            // much wider than the tool wants a pocket for its middle.
            const double leave = qMax(0.0, numOr(j.value("stock_to_leave"), 0));
            const bool climb = j.value("climb").toBool(true);
            const double pitch = cp.stepdown;
            int tooSmall = 0;
            QList<Job> holes;
            for (const Element *e : elems) {
                if (e->geometryType != QLatin1String("circle"))
                    continue;
                const QJsonArray c = e->raw.value("center").toArray();
                const QPointF ctr(c.at(0).toDouble(), c.at(1).toDouble());
                const double R = numOr(e->raw.value("radius"), 0);
                if (R - leave - toolR < 0.05) {
                    ++tooSmall;
                    continue;
                }
                Job h;
                h.rings.append(QPolygonF() << ctr << QPointF(R - leave - toolR, 0));
                holes.append(h);
            }
            if (tooSmall)
                res.skipped << QStringLiteral("%1 (%2 circle(s) no wider than the tool)")
                                   .arg(name, QString::number(tooSmall));
            if (holes.isEmpty()) {
                if (!tooSmall)
                    res.skipped << QStringLiteral("%1 (no circles to bore)").arg(name);
                continue;
            }
            for (const Job &h : orderJobs(holes, lastPos)) {
                const QPointF ctr = h.rings.first().at(0);
                const double hr = h.rings.first().at(1).x();
                const QPointF s0 = ctr + QPointF(hr, 0), s1 = ctr - QPointF(hr, 0);
                em.rapidTo(s0);
                em.plungeTo(s0, cp.zTop);
                double z = cp.zTop;
                bool atS0 = true;
                // Half a turn at a time, sinking half the pitch, to the bottom.
                while (z > cp.zBot + 1e-9) {
                    z = qMax(cp.zBot, z - pitch / 2);
                    const QPointF from = atS0 ? s0 : s1, to = atS0 ? s1 : s0;
                    body.append(Op::arcTo(to.x(), to.y(), z, ctr.x() - from.x(),
                                          ctr.y() - from.y(), !climb, feed));
                    atS0 = !atS0;
                }
                for (int half = 0; half < 2; ++half) {   // the flat lap at depth
                    const QPointF from = atS0 ? s0 : s1, to = atS0 ? s1 : s0;
                    body.append(Op::arcTo(to.x(), to.y(), z, ctr.x() - from.x(),
                                          ctr.y() - from.y(), !climb, feed));
                    atS0 = !atS0;
                }
                // Off the wall before lifting. To the centre only when the
                // helix cleared it (tool wider than the helix): otherwise the
                // core is still standing and that is a full-depth slot through
                // it, so step in just clear of the wall, within the band cut.
                const QPointF end = atS0 ? s0 : s1;
                const double in = hr <= toolR + 1e-9 ? hr : qMin(0.5, toolR);
                const QPointF off = end + (ctr - end) * (in / hr);
                em.feedAt(off, cp.zBot, feed);
                em.retract();
                lastPos = off;
            }
        } else if (thread) {
            // THREAD (phobiCCCP type, Fusion's 2D Thread): a thread mill
            // runs a helix of one `pitch` per turn around each circle, which
            // is the thread's major (nominal) diameter. Internal threads are
            // cut out to it from the hole's middle, external ones down to the
            // minor diameter from outside the boss; `thread_depth` is the
            // radial depth (0: ISO's 0.6134 x pitch), split over `passes`.
            // The tool's diameter is the cutter's. Climb (counter-clockwise
            // inside, clockwise outside) unless climb is false; which way the
            // helix climbs follows from that and the hand, so a right-hand
            // internal thread is cut from the bottom up. The helix covers
            // whole turns from end_depth up, carried on into the air above
            // the top rather than past the bottom.
            const double pitch = numOr(j.value("pitch"), 1.5);
            const bool external = j.value("side").toString() == QLatin1String("external");
            const bool climb = j.value("climb").toBool(true);
            const bool leftHand = j.value("left_hand").toBool(false);
            const int passes = qBound(1, int(numOr(j.value("passes"), 1)), 20);
            double td = numOr(j.value("thread_depth"), 0);
            if (td <= 0)
                td = 0.6134 * pitch;
            const double length = cp.zTop - cp.zBot;
            if (!(pitch > 0.05) || !(td < 50) || length <= 1e-6) {
                res.skipped << QStringLiteral("%1 (needs a pitch and a depth)").arg(name);
                continue;
            }
            const int turns = qMax(1, int(std::ceil(length / pitch - 1e-9)));
            if (turns > 10000) {
                res.skipped << QStringLiteral("%1 (too many turns)").arg(name);
                continue;
            }
            const bool ccw = external ? !climb : climb;
            const bool up = (ccw ? 1 : -1) * (leftHand ? -1 : 1) > 0;
            // From the stock top the whole turns run on into the air. A
            // thread that starts below the top (a boss's lower part, a hole
            // under a counterbore) would carry its helix up to a pitch above
            // the start into stock there, so it ends exactly at the start on
            // a part turn instead and leaves the wall radially.
            const bool partial = cp.zTop < -1e-9;
            const double sweep = partial ? 2 * M_PI * length / pitch : 2 * M_PI * turns;
            const double zLow = cp.zBot, zHigh = cp.zBot + sweep / (2 * M_PI) * pitch;
            int tooSmall = 0;
            QList<Job> holes;
            for (const Element *e : elems) {
                if (e->geometryType != QLatin1String("circle"))
                    continue;
                const QJsonArray c = e->raw.value("center").toArray();
                const QPointF ctr(c.at(0).toDouble(), c.at(1).toDouble());
                const double R = numOr(e->raw.value("radius"), 0);
                // The first (smallest-bite) pass radius must leave room.
                const double first = external ? R + toolR - td / passes
                                              : R - toolR - td + td / passes;
                // An internal one plunges at the centre: a cutter wider than
                // the minor diameter would shave every crest.
                if (!external && (first < 0.05 || toolR > R - td - 0.05)) {
                    ++tooSmall;
                    continue;
                }
                if (external && R - td <= 0.05) {
                    ++tooSmall;
                    continue;
                }
                Job h;
                h.rings.append(QPolygonF() << ctr << QPointF(R, 0));
                holes.append(h);
            }
            if (tooSmall)
                res.skipped << QStringLiteral("%1 (%2 circle(s) too small for the thread or tool)")
                                   .arg(name, QString::number(tooSmall));
            if (holes.isEmpty()) {
                if (!tooSmall)
                    res.skipped << QStringLiteral("%1 (no circles to thread)").arg(name);
                continue;
            }
            for (const Job &h : orderJobs(holes, lastPos)) {
                const QPointF ctr = h.rings.first().at(0);
                const double R = h.rings.first().at(1).x();
                // Where each pass starts and ends its radial move: the
                // hole's center, or just clear of the boss.
                const QPointF home = external ? ctr + QPointF(R + 2 * toolR + 1, 0) : ctr;
                em.rapidTo(home);
                for (int k = 1; k <= passes; ++k) {
                    const double pr = external ? R + toolR - td * k / passes
                                               : R - toolR - td + td * k / passes;
                    const QPointF s0 = ctr + QPointF(pr, 0), s1 = ctr - QPointF(pr, 0);
                    double z = up ? zLow : zHigh;
                    em.plungeTo(home, z);
                    em.feedAt(s0, z, feed);
                    // Half turns, the last one cut short to `sweep`.
                    QPointF from = s0;
                    for (int half = 0; half * M_PI < sweep - 1e-9; ++half) {
                        const double step = qMin(M_PI, sweep - half * M_PI);
                        z += (up ? pitch : -pitch) * step / (2 * M_PI);
                        const double a = (ccw ? 1 : -1) * (half * M_PI + step);
                        const QPointF to = step > M_PI - 1e-9 ? (half % 2 ? s0 : s1)
                                                              : ctr + pr * QPointF(std::cos(a), std::sin(a));
                        body.append(Op::arcTo(to.x(), to.y(), z, ctr.x() - from.x(),
                                              ctr.y() - from.y(), !ccw, feed));
                        from = to;
                    }
                    if (from == s0 || !external) {
                        em.feedAt(home, z, feed);   // off the wall before moving on
                    } else {
                        // Out radially to where `home` stands off the boss,
                        // up, and over to home for the next pass.
                        const QPointF out = (from - ctr) / pr;
                        em.feedAt(ctr + out * (R + 2 * toolR + 1), z, feed);
                        if (k < passes) {
                            em.retract();
                            em.rapidTo(home);
                        }
                    }
                }
                em.retract();
                lastPos = em.pos();
            }
        } else if (slotTp) {
            // SLOT (phobiCCCP type, Fusion's 2D Slot): each slot is cut
            // along its centreline, ramping down the length of the slot to
            // the next depth and back along the floor. Closed slots (straight
            // stadiums) as drawn by the Slot tool; open vectors are
            // centrelines cut as they are. A tool narrower than the slot also
            // runs round the wall at every depth, up to three tool diameters
            // wide; wider slots want a pocket.
            const double leave = qMax(0.0, numOr(j.value("stock_to_leave"), 0));
            struct SlotCut { QPolygonF line; QPolygonF wall; };
            QList<SlotCut> cuts;
            int notSlots = 0, tooNarrow = 0, tooWide = 0;
            for (const Element *e : elems) {
                if (!vec::isClosed(*e)) {
                    for (const QPolygonF &p : finePolygons(e->painterPath))
                        if (p.size() >= 2)
                            cuts.append({p, {}});
                    continue;
                }
                const QList<QPolygonF> polys = finePolygons(e->painterPath);
                QPointF c1, c2;
                double w = 0;
                if (polys.size() != 1 || !asStadium(polys.first(), &c1, &c2, &w)) {
                    ++notSlots;
                    continue;
                }
                const double half = w / 2 - leave;
                if (toolR > half + 1e-6) {
                    ++tooNarrow;
                    continue;
                }
                if (half > 3 * toolR + 1e-6) {
                    ++tooWide;
                    continue;
                }
                SlotCut c{QPolygonF() << c1 << c2, {}};
                if (half - toolR > 0.01) {
                    const QList<QPolygonF> wall = insetRings(e->painterPath, toolR + leave);
                    if (!wall.isEmpty())
                        c.wall = startNearest(wall.first(), c1);
                }
                cuts.append(c);
            }
            if (notSlots)
                res.skipped << QStringLiteral("%1 (%2 closed vector(s) are not slots)").arg(name, QString::number(notSlots));
            if (tooNarrow)
                res.skipped << QStringLiteral("%1 (%2 slot(s) narrower than the tool)").arg(name, QString::number(tooNarrow));
            if (tooWide)
                res.skipped << QStringLiteral("%1 (%2 slot(s) over three tool diameters wide: pocket them)")
                                   .arg(name, QString::number(tooWide));
            if (cuts.isEmpty()) {
                if (!notSlots && !tooNarrow && !tooWide)
                    res.skipped << QStringLiteral("%1 (no slots)").arg(name);
                continue;
            }
            QList<Job> order;
            for (int i = 0; i < cuts.size(); ++i) {
                Job k;
                k.rings.append(QPolygonF() << cuts.at(i).line.first() << QPointF(i, 0));
                order.append(k);
            }
            for (const Job &k : orderJobs(order, lastPos)) {
                const SlotCut &c = cuts.at(int(k.rings.first().at(1).x()));
                const QPolygonF &line = c.line;
                double total = 0;
                for (int i = 1; i < line.size(); ++i)
                    total += QLineF(line.at(i - 1), line.at(i)).length();
                em.rapidTo(line.first());
                em.plungeTo(line.first(), cp.zTop);
                double z = cp.zTop;
                while (z > cp.zBot + 1e-9) {
                    const double zNew = qMax(cp.zBot, z - cp.stepdown);
                    // Down along the slot to the far end, then back on the floor.
                    // The ramp never sinks faster than the plunge rate: down
                    // a short slot it is all but a plunge, and at the cutting
                    // feed that drove the bit into the floor.
                    const double dz = z - zNew;
                    const double rampFeed = dz > 1e-9 ? qMin(feed, plunge * std::hypot(total, dz) / dz)
                                                      : feed;
                    double run = 0;
                    for (int i = 1; i < line.size(); ++i) {
                        run += QLineF(line.at(i - 1), line.at(i)).length();
                        const double f = total > 1e-9 ? run / total : 1.0;
                        em.feedAt(line.at(i), z + (zNew - z) * f, rampFeed);
                    }
                    if (total <= 1e-9)
                        em.feedAt(line.first(), zNew, plunge);
                    for (int i = line.size() - 2; i >= 0; --i)
                        em.feedAt(line.at(i), zNew, feed);
                    z = zNew;
                    if (!c.wall.isEmpty()) {
                        em.feedAt(c.wall.first(), z, feed);
                        em.followFitted(c.wall, z);
                        em.feedAt(line.first(), z, feed);
                    }
                }
                em.retract();
                lastPos = line.first();
            }
        } else if (circular) {
            // CIRCULAR (phobiCCCP type, Fusion's Circular): round pockets
            // and bosses on the selected circles. Inside, each depth is
            // entered on a small helix in the middle and cleared outwards in
            // full circles `stepover` apart to the wall; outside, each depth
            // is entered on a helix round the outermost circle,
            // `boss_clearance` out, and cleared inwards to the wall (a
            // plunge beside that circle went straight down into uncut
            // stock). Climb (counter-clockwise in a
            // pocket, clockwise round a boss) unless climb is false.
            const double leave = qMax(0.0, numOr(j.value("stock_to_leave"), 0));
            const bool outside = j.value("side").toString() == QLatin1String("outside");
            const bool climb = j.value("climb").toBool(true);
            const bool ccw = outside ? !climb : climb;
            const double stepover = qBound(0.05, numOr(j.value("stepover"), toolR), 2 * toolR);
            const double clearance = qMax(0.0, numOr(j.value("boss_clearance"), 2 * toolR));
            const double slope = qTan(qDegreesToRadians(qBound(0.5, numOr(j.value("ramp_angle"), 3), 45.0)));
            int tooSmall = 0;
            QList<Job> holes;
            for (const Element *e : elems) {
                if (e->geometryType != QLatin1String("circle"))
                    continue;
                const QJsonArray c = e->raw.value("center").toArray();
                const QPointF ctr(c.at(0).toDouble(), c.at(1).toDouble());
                const double R = numOr(e->raw.value("radius"), 0);
                if (!outside && R - toolR - leave < 0.05) {
                    ++tooSmall;
                    continue;
                }
                Job h;
                h.rings.append(QPolygonF() << ctr << QPointF(R, 0));
                holes.append(h);
            }
            if (tooSmall)
                res.skipped << QStringLiteral("%1 (%2 circle(s) no wider than the tool)").arg(name, QString::number(tooSmall));
            if (holes.isEmpty()) {
                if (!tooSmall)
                    res.skipped << QStringLiteral("%1 (no circles)").arg(name);
                continue;
            }
            auto lap = [&](const QPointF &ctr, double r, double z) {
                const QPointF s0 = ctr + QPointF(r, 0), s1 = ctr - QPointF(r, 0);
                em.arcTo(s1, z, ctr, !ccw);
                em.arcTo(s0, z, ctr, !ccw);
            };
            // Round a boss, the laps stop short of the other selected bosses
            // (their wall plus the tool radius and stock to leave); one the
            // tool cannot pass between its neighbours and itself is skipped.
            auto roomOut = [&](const QPointF &ctr, double R) {
                double room = std::numeric_limits<double>::max();
                for (const Job &o : holes) {
                    const QPointF oc = o.rings.first().at(0);
                    const double oR = o.rings.first().at(1).x(), d = QLineF(ctr, oc).length();
                    if (d + oR <= R + 1e-9)
                        continue;   // itself, or a circle inside this boss
                    room = qMin(room, d - oR - toolR - leave);
                }
                return room;
            };
            int crowded = 0;
            for (const Job &h : orderJobs(holes, lastPos)) {
                const QPointF ctr = h.rings.first().at(0);
                const double R = h.rings.first().at(1).x();
                QVector<double> radii;   // in cutting order
                if (outside) {
                    const double rb = R + toolR + leave, room = roomOut(ctr, R);
                    if (rb > room + 1e-9) {
                        ++crowded;
                        continue;
                    }
                    const double ro = qMin(rb + clearance, room);
                    const int n = qMax(0, int(std::ceil((ro - rb) / stepover - 1e-9)));
                    for (int k = 0; k <= n; ++k)
                        radii << (n == 0 ? rb : ro - (ro - rb) * k / n);
                } else {
                    const double rf = R - toolR - leave;
                    // The first ring inside the tool radius, whatever the
                    // stepover: a helix wider than the tool leaves a pin of
                    // stock standing in the middle.
                    const double r1 = std::min({stepover, rf, 0.9 * toolR});
                    const int n = qMax(0, int(std::ceil((rf - r1) / stepover - 1e-9)));
                    for (int k = 0; k <= n; ++k)
                        radii << (n == 0 ? rf : r1 + (rf - r1) * k / n);
                }
                const QPointF home = ctr + QPointF(radii.first(), 0);
                em.rapidTo(home);
                em.plungeTo(home, cp.zTop);
                double z = cp.zTop;
                while (z > cp.zBot + 1e-9) {
                    const double zNew = qMax(cp.zBot, z - cp.stepdown);
                    // Helix down at the first radius, half a turn at a time.
                    const double drop = qMax(0.02, M_PI * radii.first() * slope);
                    double hz = z;
                    bool atS0 = true;
                    const QPointF s0 = ctr + QPointF(radii.first(), 0), s1 = ctr - QPointF(radii.first(), 0);
                    for (int k = 0; k < 100000 && hz > zNew + 1e-9; ++k) {
                        hz = qMax(zNew, hz - drop);
                        em.arcTo(atS0 ? s1 : s0, hz, ctr, !ccw);
                        atS0 = !atS0;
                    }
                    if (!atS0)
                        em.arcTo(s0, zNew, ctr, !ccw);
                    z = zNew;
                    for (double r : radii) {
                        em.feedAt(ctr + QPointF(r, 0), z, feed);
                        lap(ctr, r, z);
                    }
                    em.feedAt(home, z, feed);
                }
                em.retract();
                lastPos = home;
            }
            if (crowded)
                res.skipped << QStringLiteral("%1 (%2 boss(es) too close to another for the tool)").arg(name, QString::number(crowded));
        } else if (adaptive) {
            // ADAPTIVE (phobiCCCP type, Fusion's 2D Adaptive): the closed
            // vectors cleared without the cutter ever taking much more than
            // `stepover` of radial bite, so `stepdown` can be deep (about a
            // tool diameter) without slotting. adaptive.cpp plans the 2D
            // passes once; every depth level repeats them: a helix down at
            // `ramp_angle` in each part's widest spot, then the passes,
            // joined at depth, by a low hop just over the previous level's
            // floor, or through safe Z.
            QVector<const Element *> closedEls;
            for (const Element *e : elems)
                if (vec::isClosed(*e))
                    closedEls.append(e);
            if (closedEls.isEmpty()) {
                res.skipped << QStringLiteral("%1 (needs closed vectors)").arg(name);
                continue;
            }
            QVector<QPolygonF> rings;
            for (QPolygonF p : regionOf(closedEls).toSubpathPolygons())
                if (p.size() > 2) {
                    closeLoop(p);
                    rings.append(p);
                }
            adaptive::Params ap;
            ap.toolR = toolR;
            ap.stepover = qBound(0.02 * toolR, numOr(j.value("stepover"), 0.2 * toolR), toolR);
            ap.leave = qMax(0.0, numOr(j.value("stock_to_leave"), 0));
            ap.climb = j.value("climb").toBool(true);
            const adaptive::Plan plan = adaptive::plan(rings, ap, [&] {
                return watch && watch->stop(myIndex, tpTotal, name);
            });
            if (watch && watch->stop(myIndex, tpTotal, name)) {
                res.cancelled = true;
                break;
            }
            if (!plan.available) {
                res.skipped << QStringLiteral("%1 (adaptive needs Clipper2)").arg(name);
                continue;
            }
            if (plan.unreached)
                res.skipped << QStringLiteral("%1 (%2 area(s) narrower than the tool)")
                                   .arg(name, QString::number(plan.unreached));
            if (plan.capped)
                res.notes << QStringLiteral("%1: adaptive plan stopped at %2 moves, part of the area left uncut")
                                 .arg(name, QString::number(adaptive::kMaxMoves));
            if (plan.moves.isEmpty()) {
                if (!plan.unreached)
                    res.skipped << QStringLiteral("%1 (nothing the tool fits in)").arg(name);
                continue;
            }
            const double ramp = qBound(0.5, numOr(j.value("ramp_angle"), 2.0), 15.0);
            const double clear = 1.0;   // hops and plunges start this far above the last floor
            // Tool-centre lines of a plan, for what it sweeps.
            auto planCentres = [](const adaptive::Plan &pl) {
                QList<QPolygonF> out;
                for (const adaptive::Move &m : pl.moves) {
                    if (m.kind == adaptive::Move::Helix) {
                        QPolygonF ring;
                        for (int k = 0; k <= 16; ++k) {
                            const double a = k * M_PI / 8;
                            ring << m.center + m.radius * QPointF(qCos(a), qSin(a));
                        }
                        out << ring;
                    } else if (m.path.size() >= 2) {
                        out << m.path;
                    }
                }
                return out;
            };
            // (top of the cut, plan): one run, or one per rest band.
            QList<QPair<QPair<double, double>, adaptive::Plan>> runs;
            // Not a rest band: nothing says the stock above Start is gone (a
            // Start of 3 mm hopped at Z-2 through wood), so the first level
            // hops and enters from just above the stock top.
            bool fromTop = false;
            {
                QList<QPolygonF> centres = planCentres(plan);
                QVector<QPointF> pts;
                const double along = qBound(0.1, toolR / 4, 0.5);
                for (const QPolygonF &c : centres)
                    addAlong(pts, c, along, false);
                bool skipIt = false;
                if (j.value("rest_auto").toBool(false)) {
                    const double overlap = qBound(0.0, numOr(j.value("rest_overlap"), 1.0), 50.0);
                    double floor = 0;
                    switch (restAdjust(cleared, pts, toolR, overlap, &cp.zTop, cp.zBot, &floor)) {
                    case RestVerdict::Done:
                        res.skipped << QStringLiteral("%1 (already cut to %2 mm by the toolpaths above)")
                                           .arg(name, QString::number(-floor, 'f', 2));
                        skipIt = true;
                        break;
                    case RestVerdict::Raised:
                        res.notes << QStringLiteral("%1: starts at %2 mm, the toolpaths above cut to %3 mm")
                                         .arg(name, QString::number(-cp.zTop, 'f', 2), QString::number(-floor, 'f', 2));
                        break;
                    case RestVerdict::Partial: {
                        // Only what the toolpaths above left, band by band: each
                        // band is planned inside the pocket around its rest centres.
                        const QPainterPath region = regionFromRings(QList<QPolygonF>(rings.begin(), rings.end()));
                        const QList<RestBand> bands = restBands(
                            cleared, regionFromRings(insetRings(region, toolR + ap.leave)),
                            toolR, cp.zTop, cp.zBot, cp.stepdown);
                        centres.clear();
                        for (const RestBand &b : bands) {
                            const QPainterPath area = intersectRegion(
                                regionFromRings(outsetRings(b.centres, toolR + ap.leave)), region);
                            QVector<QPolygonF> bandRings;
                            for (QPolygonF p : area.toSubpathPolygons())
                                if (p.size() > 2) {
                                    closeLoop(p);
                                    bandRings.append(p);
                                }
                            const adaptive::Plan bp = adaptive::plan(bandRings, ap, [&] {
                                return watch && watch->stop(myIndex, tpTotal, name);
                            });
                            if (bp.capped)
                                res.notes << QStringLiteral("%1: adaptive plan stopped at %2 moves, part of the area left uncut")
                                                 .arg(name, QString::number(adaptive::kMaxMoves));
                            if (bp.moves.isEmpty())
                                continue;
                            centres << planCentres(bp);
                            runs.append({{b.zTop, b.zBot}, bp});
                        }
                        if (runs.isEmpty()) {
                            res.skipped << QStringLiteral("%1 (already cut by the toolpaths above)").arg(name);
                            skipIt = true;
                        } else {
                            res.notes << QStringLiteral("%1: cuts only what the toolpaths above left (%2 depth band(s))")
                                             .arg(name, QString::number(runs.size()));
                        }
                        break;
                    }
                    case RestVerdict::Untouched:
                        break;
                    }
                }
                // Only a flat end mill leaves a flat floor at zBot everywhere it went.
                if (!centres.isEmpty() && toolGeomOf(tool).kind == ToolGeom::Flat)
                    cleared.append({sweptRegion(centres, toolR), cp.zBot});
                if (skipIt)
                    continue;
                if (runs.isEmpty()) {
                    runs.append({{cp.zTop, cp.zBot}, plan});
                    fromTop = true;
                }
            }
            if (watch && watch->stop(myIndex, tpTotal, name)) {
                res.cancelled = true;
                break;
            }
            for (const auto &run : runs) {
                const adaptive::Plan &plan = run.second;
                double prevZ = run.first.first;
                const double runBot = run.first.second;
                // A rest band opening at Start has the stock above it too.
                bool firstLevel = fromTop || qAbs(run.first.first - cp.zTop) < 1e-9;
                while (prevZ > runBot + 1e-9) {
                    const double z = qMax(runBot, prevZ - cp.stepdown);
                    // Where hops and entries start: above the last floor, or
                    // above the stock top on the first level from Start.
                    const double hopZ = (firstLevel ? qMax(prevZ, 0.0) : prevZ) + clear;
                    firstLevel = false;
                    for (const adaptive::Move &m : plan.moves) {
                        if (m.kind == adaptive::Move::Helix) {
                            const QPointF ctr = m.center;
                            const QPointF s0 = ctr + QPointF(m.radius, 0), s1 = ctr - QPointF(m.radius, 0);
                            const double pitch = qMax(0.02, 2 * M_PI * m.radius
                                                                * qTan(qDegreesToRadians(ramp)));
                            em.retract();
                            em.rapidTo(s0);
                            em.rapidAt(s0, hopZ);
                            double hz = hopZ;
                            bool atS0 = true;
                            while (hz > z + 1e-9) {   // half a turn at a time
                                hz = qMax(z, hz - pitch / 2);
                                em.arcTo(atS0 ? s1 : s0, hz, ctr, !ap.climb);
                                atS0 = !atS0;
                            }
                            for (int half = 0; half < 2; ++half) {   // the flat lap at depth
                                em.arcTo(atS0 ? s1 : s0, z, ctr, !ap.climb);
                                atS0 = !atS0;
                            }
                            if (!atS0)   // odd number of half turns: round to s0
                                em.arcTo(s0, z, ctr, !ap.climb);
                            continue;
                        }
                        const QPointF start = m.path.first();
                        switch (m.link) {
                        case adaptive::Move::StayDown:
                            em.feedAt(start, z, feed);
                            break;
                        case adaptive::Move::Lift:
                            // Everything above the previous level is gone over
                            // the whole pocket, so a hop just above it is clear.
                            em.rapidAt(em.pos(), hopZ);
                            for (const QPointF &v : m.via)
                                em.rapidAt(v, hopZ);
                            em.rapidAt(start, hopZ);
                            em.plungeTo(start, z);
                            break;
                        case adaptive::Move::Retract:
                            em.retract();
                            em.rapidTo(start);
                            em.rapidAt(start, hopZ);
                            em.plungeTo(start, z);
                            break;
                        }
                        em.followFitted(m.path, z);
                    }
                    prevZ = z;
                }
            }
            em.retract();
            lastPos = em.pos();
        } else if (chamfer) {
            // 2D CHAMFER (phobiCCCP type, Fusion's 2D Chamfer): a V-bit runs
            // `tip_offset` off the vectors on the air side, deep enough that
            // the cone cuts a bevel `chamfer_width` wide into the edge.
            // side "outside" bevels a part's outer edge (tip outside the
            // closed vectors), "inside" a hole's or pocket's rim. Open
            // vectors are traced as they are.
            const ToolGeom vg = toolGeomOf(tool);
            if (vg.kind != ToolGeom::VBit) {
                res.skipped << QStringLiteral("%1 (needs a V-bit)").arg(name);
                continue;
            }
            const double halfTan = qTan(qDegreesToRadians(qBound(10.0, vg.angle, 170.0) / 2));
            const double width = qMax(0.0, numOr(j.value("chamfer_width"), 1.0));
            const double tip = qMax(0.0, numOr(j.value("tip_offset"), 0.5));
            // Deeper than the cone, the bevel would be cut by the shank.
            if (width + tip > vg.radius() + 1e-6) {
                res.skipped << QStringLiteral("%1 (chamfer width + tip offset %2 mm is more than the V-bit's radius %3 mm)")
                                   .arg(name, QString::number(width + tip, 'f', 2), QString::number(vg.radius(), 'f', 2));
                continue;
            }
            const bool inside = j.value("side").toString() == QLatin1String("inside");
            cp.zBot = cp.zTop - (width + tip) / halfTan;
            QVector<const Element *> closedEls;
            for (const Element *e : elems) {
                if (vec::isClosed(*e)) {
                    closedEls.append(e);
                    continue;
                }
                for (const QPolygonF &p : finePolygons(e->painterPath))
                    if (p.size() >= 2) {
                        Job job;
                        job.rings.append(p);
                        job.closed = false;
                        jobs.append(job);
                    }
            }
            if (!closedEls.isEmpty()) {
                const QPainterPath region = regionOf(closedEls);
                QList<QPolygonF> rings;
                if (tip < 1e-6) {
                    for (QPolygonF p : region.toSubpathPolygons())
                        if (p.size() > 2) {
                            closeLoop(p);
                            rings.append(p);
                        }
                } else {
                    rings = inside ? insetRings(region, tip) : outsetRings(region, tip);
                }
                for (const QPolygonF &p : rings) {
                    Job job;
                    job.rings.append(p);
                    jobs.append(job);
                }
            }
        } else if (drilling || keyhole) {
            const double peck = numOr(j.value("peck_distance"), 0);
            const double slotLen = numOr(j.value("length"), 12.7);
            const double slotAng = qDegreesToRadians(numOr(j.value("angle"), 90));
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
            // Drilling cycles (phobiCCCP keys): `dwell` seconds at the bottom
            // of each hole, and `chip_break` pecks that back off only
            // `chip_retract` mm between pecks instead of leaving the hole.
            const double dwell = drilling ? qBound(0.0, numOr(j.value("dwell"), 0), 60.0) : 0.0;
            const bool chipBreak = drilling && j.value("chip_break").toBool(false);
            const double chipBack = qBound(0.05, numOr(j.value("chip_retract"), 0.5), 10.0);
            for (const Job &h : orderJobs(pts, lastPos)) {
                const QPointF ctr = h.start();
                em.rapidTo(ctr);
                if (drilling && peck > 0.01) {
                    double z = cp.zTop;
                    while (z > cp.zBot + 1e-9) {
                        z = qMax(cp.zBot, z - peck);
                        em.plungeTo(ctr, z);
                        if (z <= cp.zBot + 1e-9 && dwell > 0)
                            body.append(Op::dwell(dwell));
                        if (chipBreak && z > cp.zBot + 1e-9)
                            em.rapidAt(ctr, z + chipBack);
                        else
                            em.retract();
                    }
                } else {
                    em.plungeTo(ctr, cp.zBot);
                    if (dwell > 0)
                        body.append(Op::dwell(dwell));
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
            const double ang = qDegreesToRadians(numOr(j.value("angle"), 0));
            const QPointF dir(qCos(ang), qSin(ang));
            const QPointF nrm(-dir.y(), dir.x());
            const double stepover = qMax(0.2, numOr(j.value("stepover"), 3));
            const double sVar = qBound(0.0, numOr(j.value("stepover_variation"), 0), 1.0);
            const double minL = qMax(1.0, numOr(j.value("min_length"), 15));
            const double maxL = qMax(minL, numOr(j.value("max_length"), 30));
            const double zMin = depth(j.value("min_depth"));
            const double zMax = depth(j.value("max_depth"));
            if (!depthsOk) {
                res.skipped << QStringLiteral("%1 (cut depth is not a number)").arg(name);
                continue;
            }
            Lcg rng(qHash(t.uuid));

            // Where each hatch line enters and leaves the region, computed the
            // way the engrave hatch does it: crossings against the boundary
            // edges, sorted, taken in pairs.
            //
            // This used to sample region.contains() every 0.5 mm along the full
            // diagonal, for every hatch line -- diag^2 / (0.5 * stepover) calls
            // to one of the most expensive predicates Qt has. On a 1220 mm
            // Shapeoko XXL sheet that is about three million of them: measured
            // 25 s, on the interface thread, to emit a few hundred strokes.
            // The cost grew with the square of the sheet. Crossings are exact
            // as well as cheap, so the runs are no longer quantised to 0.5 mm.
            QList<QPolygonF> edges;
            for (QPolygonF r : region.toSubpathPolygons()) {
                if (r.size() > 2 && r.first() != r.last())
                    r.append(r.first());
                if (r.size() > 3)
                    edges.append(r);
            }

            const double diag = QLineF(bb.topLeft(), bb.bottomRight()).length();
            const QPointF mid = bb.center();
            int guard = 0;
            QVector<double> hits;
            for (double off = -diag / 2; off < diag / 2 && guard < 20000; off +=
                 stepover * (1.0 + sVar * (rng.next() - 0.5))) {
                const QPointF base = mid + nrm * off;
                hits.clear();
                for (const QPolygonF &ring : edges) {
                    for (int i = 0; i + 1 < ring.size(); ++i) {
                        const QPointF &P = ring.at(i), &Q = ring.at(i + 1);
                        const double fP = (P.x() - base.x()) * nrm.x()
                                          + (P.y() - base.y()) * nrm.y();
                        const double fQ = (Q.x() - base.x()) * nrm.x()
                                          + (Q.y() - base.y()) * nrm.y();
                        // half-open: a vertex exactly on the line counts once
                        if ((fP <= 0 && fQ > 0) || (fQ <= 0 && fP > 0)) {
                            const double t = fP / (fP - fQ);
                            const QPointF p = P + t * (Q - P);
                            hits.append((p.x() - base.x()) * dir.x()
                                        + (p.y() - base.y()) * dir.y());
                        }
                    }
                }
                std::sort(hits.begin(), hits.end());
                for (int h = 0; h + 1 < hits.size() && guard < 20000; h += 2) {
                    double a = hits.at(h);
                    const double runEnd = hits.at(h + 1);
                    while (a < runEnd - 1.0 && guard < 20000) {
                        ++guard;
                        const double L = qMin(runEnd - a, rng.in(minL, maxL));
                        const QPointF sp = base + dir * a;
                        const QPointF ep = base + dir * (a + L);
                        const double z = rng.in(qMin(zMin, zMax), qMax(zMin, zMax));
                        em.rapidTo(sp);
                        em.plungeTo(sp, z);
                        // Through the emitter, so it knows the stroke ended
                        // at ep: appended behind its back, the retract went
                        // up over the stroke's start instead, and the next
                        // rapid travelled back from there.
                        em.feedAt(ep, z, feed);
                        em.retract();
                        a += qMax(1.0, L * 0.7 + rng.in(0, L * 0.3));
                    }
                }
            }
        } else if (vcarve) {
            // A flat or ball bit cuts straight down past the walls the depth
            // follows; only a V-bit's cone fits them. A tool with no type
            // given is taken as a 60-degree V-bit, as before.
            const ToolGeom vg = toolGeomOf(tool);
            if (tool.contains(QLatin1String("type")) && vg.kind != ToolGeom::VBit) {
                res.skipped << QStringLiteral("%1 (needs a V-bit)").arg(name);
                continue;
            }
            const double bitAngle = vg.kind == ToolGeom::VBit ? vg.angle : 60.0;
            const double halfTan = qTan(qDegreesToRadians(qBound(10.0, bitAngle, 179.0) / 2));
            const double stepover = qMax(0.05, numOr(j.value("stepover"), 0.2));
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
            int tooDeep = 0;
            for (const QPainterPath &comp : comps) {
                bool medialDone = false;
                if (medialAxisAvailable()) {
                    // True v-carve: ride the medial axis, the bit tip sinking
                    // to clearance/tan(halfAngle) so the cone kisses both
                    // walls — sharp corners come from the spurs, where the
                    // clearance (and the bit) runs out to zero.
                    QVector<QVector<VPoint>> pend =
                        medialAxis(comp.toSubpathPolygons());
                    // Where the carve goes deeper than the cone (a shape
                    // wider than the bit, end depth past the cone), the shank
                    // rubs and leaves a lip along the walls: that shape is
                    // left out, with a note. Narrow letters never get there.
                    // The cone's width where it leaves the stock: an inlay
                    // male starts below the top, and the cone above its start
                    // depth still has to cut up through the stock. Where a
                    // flat floor above already covers the shape (a letter on
                    // a pocket's floor), the stock starts at that floor.
                    auto zOf = [&](const VPoint &p) {
                        return qMax(cp.zBot, cp.zTop - p.d / halfTan);
                    };
                    double top = 0;
                    {
                        QPainterPath core = regionFromRings(insetRings(comp, 0.05));
                        if (core.isEmpty())
                            core = comp;
                        for (const Cleared &c : cleared) {
                            const double cand = qMax(c.zBot, cp.zTop);
                            if (!(cand < top))
                                continue;
                            // The cone (or the shank, at most) widens past the
                            // shape between that floor and Z0: that ring must be air too.
                            // It meets the shape's edge at the start depth, so it is
                            // that much wider at Z0 however high the floor is.
                            const double grow = qMin(vg.radius(), -cp.zTop * halfTan);
                            const QPainterPath need = grow > 0.05
                                ? regionFromRings(outsetRings(core, grow)) : core;
                            QList<QPainterPath> regs;
                            for (const Cleared &o : cleared)
                                if (o.zBot <= c.zBot + 1e-6)
                                    regs << o.foot;
                            // Clipper, not QPainterPath::contains(): that missed
                            // an uncut island standing wholly inside the letter.
                            double left = 0;
                            for (const QPolygonF &g : subtractRegion(need, unionRegions(regs)).toSubpathPolygons())
                                left += std::fabs(vec::ringArea(g));
                            if (left < 0.01)
                                top = cand;
                        }
                    }
                    double reach = 0;
                    for (const QVector<VPoint> &ch : pend)
                        for (const VPoint &v : ch)
                            reach = qMax(reach, (top - zOf(v)) * halfTan);
                    if (reach > vg.radius() + 1e-6) {
                        ++tooDeep;
                        continue;
                    }
                    medialDone = !pend.isEmpty();
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
                        // Per pass from the stock top: deeper than one pass
                        // (a wide letter, an inlay male that starts below the
                        // top) the chain is ridden again at each level, the
                        // last at its own depth.
                        double deepest = 0;
                        for (const VPoint &v : ch)
                            deepest = qMin(deepest, zOf(v));
                        // No Per pass on file (0): one pass, as before.
                        const double step = numOr(j.value("stepdown"), 1.0) > 1e-6 ? cp.stepdown : 1e9;
                        double level = top;
                        do {
                            level = qMax(deepest, level - step);
                            if (level < deepest + 1e-9)
                                level = deepest;
                            em.rapidTo(s);
                            em.plungeTo(s, qMax(level, zOf(ch.first())));
                            for (int k = 1; k < ch.size(); ++k)
                                em.feedAt(QPointF(ch.at(k).x, ch.at(k).y),
                                          qMax(level, zOf(ch.at(k))), feed);
                            em.retract();
                        } while (level > deepest + 1e-9);
                        cur = em.pos();
                    }
                    // Flat-bottom clearing where the shape is wider than the
                    // bit's reach at full depth.
                    // No Per pass on file (0): one pass, as for the medial chains.
                    const double step = numOr(j.value("stepdown"), 1.0) > 1e-6 ? cp.stepdown : 1e9;
                    if (medialDone && dMax > 1e-9) {
                        double delta = dMax;
                        int guard = 0;
                        while (guard++ < 2000) {
                            const QList<QPolygonF> rings = insetRings(comp, delta);
                            if (rings.isEmpty())
                                break;
                            for (const QPolygonF &ring : rings) {
                                // Full depth in one pass here is virgin stock:
                                // the medial pass only carved the skeleton, so
                                // the mid-edge stretches of this ring are
                                // untouched. Plunging straight to zBot and
                                // taking the whole depth in one lap breaks
                                // bits. Step down like every other ring job.
                                // From the stock top, not the start depth: an
                                // inlay male starts well below the top, and the
                                // stock above it is still there.
                                em.rapidTo(ring.first());
                                double zr = top;
                                bool deeper = true;
                                while (deeper) {
                                    zr = qMax(cp.zBot, zr - step);
                                    deeper = zr > cp.zBot + 1e-9;
                                    em.plungeTo(ring.first(), zr);
                                    em.followRing(ring, zr, flat);
                                }
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
            if (tooDeep > 0) {
                const QString why = QStringLiteral("deeper than the V-bit's cone (%1 mm): lower the end depth "
                                                   "or use a wider bit").arg(vg.radius() / halfTan, 0, 'f', 2);
                if (tooDeep == comps.size()) {
                    res.skipped << QStringLiteral("%1 (%2)").arg(name, why);
                    continue;
                }
                res.notes << QStringLiteral("%1: %2 shape(s) left uncut, %3").arg(name, QString::number(tooDeep), why);
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
            cp.stepdown = qMax(0.02, numOr(j.value("stepdown"), cp.zTop - cp.zBot));
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
                const double bitAngle = numOr(tool.value("angle"), 0);
                double effR = toolR;
                if (bitAngle > 0)
                    effR = qMin(toolR, (cp.zTop - cp.zBot)
                                           * qTan(qDegreesToRadians(qBound(1.0, bitAngle, 179.0) / 2)));
                effR = qMax(0.01, effR);
                const double spacing = qMax(0.05, numOr(j.value("line_spacing"), 1.0));
                const double angle = numOr(j.value("angle"), 45);
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
            int openTabless = 0;
            for (const Element *e : elems)
                for (const QPolygonF &p : finePolygons(e->painterPath))
                    if (p.size() >= 2) {
                        Job job;
                        job.rings.append(p);
                        job.closed = p.isClosed();
                        jobs.append(job);
                        openTabless += job.closed ? 0 : 1;
                    }
            // Tabs go on closed loops only: an outline whose ends miss by a
            // hair (a DXF gap) was cut through at full depth with no word.
            if (openTabless > 0 && cp.tabWidth > 0.01 && cp.zBot < cp.tabTop - 1e-9)
                res.notes << QStringLiteral("%1: %2 open vector(s) cut without tabs (tabs go on closed vectors only)")
                                 .arg(name).arg(openTabless);
        } else if (contour || cutout) {
            const QPainterPath region = regionOf(elems);
            const bool inside = contour ? j.value("ofset_dir").toInt(0) < 0
                                        : j.value("flip_inside_outside").toBool(false);
            // stock_to_leave keeps the cut that much further from the vector
            // on the material side (cutouts never leave stock).
            const double leave = contour ? qMax(0.0, numOr(j.value("stock_to_leave"), 0)) : 0.0;
            // Cut direction: `climb` as CC writes it (false on the contours it
            // and makeToolpath() create, absent on cutouts: conventional).
            // The offsets came out in whatever order the boolean left them,
            // so an outside contour ran conventional and an inside one climb,
            // whatever the switch said.
            const bool climb = j.value("climb").toBool(false);
            auto ringsAt = [&](double d) {
                QList<QPolygonF> rs = inside ? insetRings(region, d) : outsetRings(region, d);
                orientForCut(rs, !inside, climb);
                return rs;
            };
            const QList<QPolygonF> rings = ringsAt(toolR + leave);
            // Finishing passes (phobiCCCP keys, contours only): the roughing
            // ring stays finish_passes x finish_stepover off the final one;
            // then, at the final depth, one ring per finish_stepover closer,
            // and spring_pass repeats the last. A loop whose offsets vanish
            // or merge with a neighbour's is just cut as before.
            const int nFin = contour ? qBound(0, int(numOr(j.value("finish_passes"), 0)), 10) : 0;
            const double fs = qBound(0.01, numOr(j.value("finish_stepover"), 0.25), 2 * toolR);
            const bool spring = contour && j.value("spring_pass").toBool(false);
            QVector<QList<QPolygonF>> levels;   // levels[k - 1]: k x fs further out
            for (int k = 1; k <= nFin; ++k)
                levels.append(ringsAt(toolR + leave + k * fs));
            // Lead-in / lead-out arcs (phobiCCCP key, contours only). Ramping
            // is an entry of its own, so it wins when both are asked for.
            const double leadR = contour && cp.rampAngle <= 0
                                     ? qMax(0.0, numOr(j.value("lead_radius"), 0)) : 0.0;
            const QPainterPath centres = leadR > 0 ? regionFromRings(rings) : QPainterPath();
            auto allowed = [&](const QPointF &p) { return centres.contains(p) == inside; };
            // With tabs below the cut, a lead leaves 1.5 mm of material
            // between its kerf and the ring's away from the join (the bridge
            // a tab note warns under), and is no wider than the bit: a larger
            // half circle, closed by the feed across it onto a finishing
            // pass, cuts a chip loose.
            const double leadKeep = cp.tabWidth > 0.01 && cp.zBot < cp.tabTop - 1e-9 ? 2 * toolR + 1.5 : 0.0;
            const double leadUse = leadKeep > 0 ? qMin(leadR, 2 * toolR) : leadR;
            QList<QPolygonF> edge;
            if (cp.tabWidth > 0.01)
                for (QPolygonF e : region.toSubpathPolygons())
                    if (e.size() > 2) {
                        closeLoop(e);
                        edge << e;
                    }
            int finSkipped = 0;
            for (const QPolygonF &p : rings) {
                Job job;
                job.edge = edge;
                QPolygonF rough = p;
                QList<QPolygonF> finish;
                if (nFin > 0) {
                    const double tol = 0.3 * fs + 0.02;
                    const int r = parallelRing(levels.at(nFin - 1), p, nFin * fs, tol);
                    QList<QPolygonF> mids;
                    bool ok = r >= 0;
                    for (int k = nFin - 1; ok && k >= 1; --k) {
                        const int m = parallelRing(levels.at(k - 1), p, k * fs, tol);
                        ok = m >= 0;
                        if (ok)
                            mids << levels.at(k - 1).at(m);
                    }
                    if (ok) {
                        rough = levels.at(nFin - 1).at(r);
                        finish = mids;
                        finish << p;
                    } else {
                        ++finSkipped;
                    }
                }
                if (spring)
                    finish << (finish.isEmpty() ? rough : finish.last());
                job.rings.append(rough);
                if (leadR > 0) {
                    const QPolygonF moved = startMidLongestEdge(rough);
                    job.lead = makeLead(moved, leadUse, allowed, leadKeep);
                    if (job.lead.on)
                        job.rings.first() = moved;
                }
                // Each finishing ring starts beside where the cut before it
                // ended, so stepping across is a short radial move.
                QPointF at = job.rings.first().first();
                for (const QPolygonF &f : finish) {
                    const QPolygonF fr = startNearest(f, at);
                    job.finish << fr;
                    if (job.lead.on)
                        job.finishLeads << makeLead(fr, leadUse, allowed, leadKeep);
                    at = fr.first();
                }
                jobs.append(job);
            }
            if (finSkipped)
                res.notes << QStringLiteral("%1: finishing passes skipped on %2 loop(s), "
                                            "their offsets do not stay parallel")
                                 .arg(name, QString::number(finSkipped));
        } else { // pocket
            const double stepover = qMax(0.1, numOr(j.value("stepover"), toolR));
            cp.linkDist = qMax(2.5 * stepover, 4 * toolR);
            const double leave = qMax(0.0, numOr(j.value("stock_to_leave"), 0));
            // Rest machining: with enable_rest and a rest_diameter larger than
            // this tool, only ring-fill what that previous cutter left behind.
            const double prevR = numOr(j.value("rest_diameter"), 0) / 2.0;
            const bool useRest = j.value("enable_rest").toBool(false) && prevR > toolR + 1e-6;

            // Helical entry (phobiCCCP keys): helix_entry, helix_diameter (of
            // the tool centre's circle) and helix_angle. Rest regions and
            // pockets too narrow for a helix keep the plunge or ramp.
            const bool helix = j.value("helix_entry").toBool(false) && !useRest;
            const double helixWant = qMax(0.0, numOr(j.value("helix_diameter"), 2 * toolR)) / 2;
            const double helixAngle = numOr(j.value("helix_angle"), 2);
            bool capped = false;
            pocketStep = stepover;
            for (const QPainterPath &comp : components(regionOf(elems))) {
                if (useRest) {
                    for (const QPainterPath &rc : restRegions(comp, toolR, prevR, leave)) {
                        capped |= !ringFillJob(rc, 0.0, stepover, jobs, toolR);
                        pocketMine << rc;
                    }
                } else {
                    const int before = jobs.size();
                    capped |= !ringFillJob(comp, toolR + leave, stepover, jobs, toolR);
                    pocketMine << regionFromRings(insetRings(comp, toolR + leave));
                    if (helix && jobs.size() > before)
                        addHelix(jobs[before], insetRings(comp, toolR + leave), helixWant, helixAngle);
                }
            }
            if (capped)
                res.notes << QStringLiteral("%1: too many rings for the stepover, "
                                            "the middle of the pocket is left uncut").arg(name);
            // Links (ring to ring, helix to ring) stay down only inside the
            // tool-centre region: rings either side of an island are often
            // closer than linkDist.
            cp.linkOk = linkInside(unionRegions(pocketMine));
        }

        // A face with a flat end mill leaves a flat floor too (a V-carve on a
        // faced panel counts its stock from there).
        if (face && !jobs.isEmpty() && toolGeomOf(tool).kind == ToolGeom::Flat)
            cleared.append({sweptRegion(jobs.first().rings, toolR), cp.zBot});
        if (!jobs.isEmpty() && (pocket || contour || cutout)) {
            // Rest depth: start where the toolpaths above left off.
            QList<QPolygonF> centres;
            QVector<QPointF> pts;
            const double along = qBound(0.1, toolR / 4, 0.5);
            // A lead's half circle from a through the ring's start to b, and
            // the step back across it between passes: the first pass drops
            // in at a, so it must be in cut air as much as the ring is.
            auto addLead = [&](const Job::Lead &l, const QPointF &s) {
                if (!l.on)
                    return;
                const double r = QLineF(l.c, s).length();
                if (r < 1e-9)
                    return;
                const QPointF u0 = (l.a - l.c) / r, u1 = (s - l.c) / r;
                const int n = qMax(8, int(std::ceil(M_PI * r / along)));
                QPolygonF arc;
                for (int k = 0; k <= n; ++k) {
                    const double th = M_PI * k / n;
                    arc << l.c + r * (u0 * qCos(th) + u1 * qSin(th));
                }
                addAlong(pts, arc, along, false);
                addAlong(pts, QPolygonF() << l.b << l.a, along, false);
            };
            for (const Job &job : jobs) {
                for (const QPolygonF &r : job.rings) { centres << r; addAlong(pts, r, along, pocket); }
                for (const QPolygonF &r : job.finish) { centres << r; addAlong(pts, r, along, pocket); }
                if (!job.rings.isEmpty())
                    addLead(job.lead, job.rings.first().first());
                for (int k = 0; k < job.finishLeads.size() && k < job.finish.size(); ++k)
                    addLead(job.finishLeads.at(k), job.finish.at(k).first());
                if (job.helix.on)
                    for (int k = 0; k < 8; ++k) {
                        const double a = k * M_PI / 4;
                        const QPointF hp = job.helix.c + job.helix.r * QPointF(qCos(a), qSin(a));
                        pts << hp;
                    }
            }
            if (j.value("rest_auto").toBool(false)) {
                const double overlap = qBound(0.0, numOr(j.value("rest_overlap"), 1.0), 50.0);
                double floor = 0;
                switch (restAdjust(cleared, pts, toolR, overlap, &cp.zTop, cp.zBot, &floor)) {
                case RestVerdict::Done:
                    res.skipped << QStringLiteral("%1 (already cut to %2 mm by the toolpaths above)")
                                       .arg(name, QString::number(-floor, 'f', 2));
                    continue;
                case RestVerdict::Raised:
                    res.notes << QStringLiteral("%1: starts at %2 mm, the toolpaths above cut to %3 mm")
                                     .arg(name, QString::number(-cp.zTop, 'f', 2), QString::number(-floor, 'f', 2));
                    break;
                case RestVerdict::Partial:
                    if (pocket) {
                        // Only what the toolpaths above left, band by band.
                        QList<Job> banded;
                        const QList<RestBand> bands =
                            restBands(cleared, unionRegions(pocketMine), toolR, cp.zTop, cp.zBot, cp.stepdown);
                        // Each band's entry keeps the pocket's helix, if it has one.
                        const bool helix = j.value("helix_entry").toBool(false)
                                           && !(j.value("enable_rest").toBool(false)
                                                && numOr(j.value("rest_diameter"), 0) / 2.0 > toolR + 1e-6);
                        const double helixWant = qMax(0.0, numOr(j.value("helix_diameter"), 2 * toolR)) / 2;
                        const double helixAngle = numOr(j.value("helix_angle"), 2);
                        for (int b = 0; b < bands.size(); ++b) {
                            // A band's links stay inside that band's region.
                            bandLinks << linkInside(bands.at(b).centres);
                            for (const QPainterPath &rc : components(bands.at(b).centres)) {
                                const int before = banded.size();
                                ringFillJob(rc, 0.0, pocketStep, banded, toolR);
                                if (helix && banded.size() > before)
                                    addHelix(banded[before], insetRings(rc, 0.0), helixWant, helixAngle);
                                for (int k = before; k < banded.size(); ++k) {
                                    banded[k].band = b;
                                    banded[k].bandTop = bands.at(b).zTop;
                                    banded[k].bandBot = bands.at(b).zBot;
                                }
                            }
                        }
                        jobs = banded;
                        centres.clear();
                        for (const Job &job : jobs)
                            centres << job.rings;
                        res.notes << QStringLiteral("%1: cuts only what the toolpaths above left (%2 depth band(s))")
                                         .arg(name, QString::number(bands.size()));
                    } else {
                        res.notes << QStringLiteral("%1: only partly inside what the toolpaths above cut, full depth kept")
                                         .arg(name);
                    }
                    break;
                case RestVerdict::Untouched:
                    break;
                }
            }
            // Only a flat end mill leaves a flat floor at zBot everywhere it
            // went: a V-bit or ball leaves a groove, and tabs stand up to
            // tabTop, so those floors are recorded no deeper than is sure.
            if (!centres.isEmpty() && toolGeomOf(tool).kind == ToolGeom::Flat)
                cleared.append({sweptRegion(centres, toolR), qMax(cp.zBot, cp.tabTop)});
            if (jobs.isEmpty()) {
                res.skipped << QStringLiteral("%1 (already cut by the toolpaths above)").arg(name);
                continue;
            }
        }

        if (!jobs.isEmpty()) {
            if (pendingTool.text != toolChange.text)
                flush();                      // different tool: close the pool
            pendingTool = toolChange;
            for (const Job &job : jobs) {
                CutParams jcp = cp;
                if (!std::isnan(job.bandTop)) {
                    jcp.zTop = job.bandTop;
                    jcp.zBot = job.bandBot;
                    if (job.band >= 0 && job.band < bandLinks.size())
                        jcp.linkOk = bandLinks.at(job.band);
                }
                pending.append({job, jcp, myIndex, name, feed, plunge, rpm, toolR + 0.01});
            }
            if (!siteOrder)
                flush();                      // list order: this toolpath, then the next
            continue;
        }

        if (body.isEmpty()) {
            res.skipped << QStringLiteral("%1 (empty)").arg(name);
            continue;
        }
        flush();   // direct-emission toolpath: keep document order around it
        if (toolChange.text != lastTool) {
            ops.append(toolChange);
            lastTool = toolChange.text;
        }
        ops.append(Op::comment(name));
        ops.append(Op::spindle(rpm));
        ops.append(body);
        res.done << name;
        res.doneIds << t.uuid;
    }
    flush();

    res.gcode = GrblPost(true).generate(ops);
    res.ops = ops;
    if (const QString w = toolReuseWarning(ops); !w.isEmpty())
        res.notes << w;
    return res;
}

// CC tool `type`: 0 = flat end mill, 1 = ball, 2 = V-bit. Be lenient with
// files whose type is missing or inconsistent: a positive angle means a V-bit,
// a corner radius equal to the tool radius means a ball.
static ToolGeom toolGeomOf(const QJsonObject &tool)
{
    ToolGeom g;
    g.number = int(numOr(tool.value("number"), 0));
    g.diameter = numOr(tool.value("diameter"), 3.175);
    if (!(g.diameter > 0) || !std::isfinite(g.diameter))
        g.diameter = 3.175;
    g.angle = numOr(tool.value("angle"), 0);
    g.cornerRadius = qMax(0.0, numOr(tool.value("corner_radius"), 0));
    g.name = tool.value("name").toString();
    const int type = int(numOr(tool.value("type"), 0));
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
