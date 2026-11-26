#include "vectorops.h"
#include <QHash>
#include <QSet>
#include <functional>
#include <QJsonArray>
#include <QLineF>
#include <QTransform>
#include <QUuid>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <numeric>

#ifdef HAVE_CLIPPER2
#include <clipper2/clipper.h>
namespace C2 = Clipper2Lib;
#endif

namespace c2d {
namespace vec {

// Flatten curves at `tol` mm (same trick as gcodeexport: Qt flattens at a
// fixed 0.5 path-unit chord error, so scale up first).
static QList<QPolygonF> finePolygons(const QPainterPath &path, double tol)
{
    // tol is a public parameter; 0 would scale every coordinate to inf and
    // silently return no rings at all rather than an error.
    const double k = 0.5 / (tol > 1e-9 ? tol : 0.005);
    QList<QPolygonF> out = path.toSubpathPolygons(QTransform::fromScale(k, k));
    for (QPolygonF &poly : out)
        for (QPointF &pt : poly)
            pt /= k;
    return out;
}

// Clipper returns pieces that only touch at a corner (a circle inscribed in
// a square leaves four corner pieces) as one ring that visits that corner
// twice. Split such a ring into its simple loops.
static QVector<QPolygonF> splitAtTouches(QPolygonF ring)
{
    if (ring.size() > 1 && ring.first() == ring.last())
        ring.removeLast();
    auto same = [](const QPointF &a, const QPointF &b) {
        return std::fabs(a.x() - b.x()) < 1e-4 && std::fabs(a.y() - b.y()) < 1e-4;
    };
    QVector<QPolygonF> out;
    QPolygonF cur;
    for (const QPointF &p : ring) {
        int k = -1;
        for (int i = 0; i + 1 < cur.size(); ++i)   // not the point just before
            if (same(cur.at(i), p)) {
                k = i;
                break;
            }
        if (k >= 0) {
            QPolygonF loop = cur.mid(k);
            if (loop.size() >= 3)
                out << loop;
            cur.resize(k + 1);
        } else {
            cur << p;
        }
    }
    if (cur.size() >= 3)
        out << cur;
    return out;
}

QVector<QPolygonF> fillFace(const QVector<QPainterPath> &closed, const QPointF &at, double tol)
{
    QVector<QPainterPath> in, out;
    for (const QPainterPath &p : closed) {
        QPainterPath eo = p;
        eo.setFillRule(Qt::OddEvenFill);
        (eo.contains(at) ? in : out) << eo;
    }
    if (in.isEmpty())
        return {};
    auto pathOf = [](const QVector<QPolygonF> &rings) {
        QPainterPath r;
        r.setFillRule(Qt::OddEvenFill);
        for (const QPolygonF &g : rings) {
            r.addPolygon(g);
            r.closeSubpath();
        }
        return r;
    };
    QVector<QPolygonF> face = in.size() == 1 ? booleanRings(in, {}, BoolOp::Union, tol)
                                             : booleanRings(in, {}, BoolOp::Intersect, tol);
    if (!out.isEmpty() && !face.isEmpty())
        face = booleanRings({pathOf(face)}, out, BoolOp::Subtract, tol);
    {
        QVector<QPolygonF> loops;
        for (const QPolygonF &r : face)
            loops += splitAtTouches(r);
        face = loops;
    }
    // Flattened, a circle inscribed in a square no longer touches it: the
    // corner pieces hang together by slits a few microns wide. Shrinking and
    // regrowing by twice the flattening tolerance cuts those (corners come
    // back rounded by that much, far below anything a bit can cut).
    if (!face.isEmpty()) {
        const double d = 2 * (tol > 1e-9 ? tol : 0.005);
        const QVector<QPolygonF> shrunk = offsetRings({pathOf(face)}, {}, -d, tol);
        const QVector<QPolygonF> opened = shrunk.isEmpty() ? shrunk : offsetRings({pathOf(shrunk)}, {}, d, tol);
        if (!opened.isEmpty())
            face = opened;
    }
    // The piece holding `at`: the smallest ring around it is its outline;
    // its holes are the rings inside that outline not inside another of them.
    int outer = -1;
    double best = 0;
    for (int i = 0; i < face.size(); ++i)
        if (face.at(i).containsPoint(at, Qt::OddEvenFill)) {
            const double a = std::fabs(ringArea(face.at(i)));
            if (outer < 0 || a < best) {
                outer = i;
                best = a;
            }
        }
    if (outer < 0)
        return {};
    const QPolygonF &o = face.at(outer);
    auto within = [](const QPolygonF &inner, const QPolygonF &around) {
        for (const QPointF &q : inner)
            if (!around.containsPoint(q, Qt::OddEvenFill))
                return false;
        return true;
    };
    QVector<int> holes;
    for (int i = 0; i < face.size(); ++i)
        if (i != outer && !face.at(i).containsPoint(at, Qt::OddEvenFill) && within(face.at(i), o)
            && std::fabs(ringArea(face.at(i))) < best)
            holes << i;
    QVector<QPolygonF> res{o};
    for (int i : holes) {
        bool nested = false;
        for (int k : holes)
            if (k != i && std::fabs(ringArea(face.at(k))) > std::fabs(ringArea(face.at(i)))
                && within(face.at(i), face.at(k)))
                nested = true;
        if (!nested)
            res << face.at(i);
    }
    return res;
}

double ringArea(const QPolygonF &ring)
{
    double a = 0;
    const int n = ring.size();
    for (int i = 0; i < n; ++i) {
        const QPointF &p = ring.at(i);
        const QPointF &q = ring.at((i + 1) % n);
        a += p.x() * q.y() - q.x() * p.y();
    }
    return a / 2.0;
}

bool isClosed(const Element &e)
{
    if (e.geometryType == QLatin1String("text"))
        return !Element::isStrokeText(e.raw);
    if (e.geometryType != QLatin1String("path"))
        return true;
    for (const QJsonValue &v : e.raw.value("point_type").toArray())
        if (v.toInt() == 4)
            return true;
    return false;
}

#ifdef HAVE_CLIPPER2
static const double kScale = 1000.0;   // integer µm

static C2::Path64 toPath64(const QPolygonF &poly, bool trimClosed = true)
{
    C2::Path64 p;
    const int n = trimClosed && poly.isClosed() ? poly.size() - 1 : poly.size();
    p.reserve(size_t(n));
    // Clamped: a coordinate past ~1e15 units overflows Clipper's integer maths
    // and shapes vanish silently. Infinity clamps by its sign; NaN becomes 0.
    auto q = [](double v) {
        const double s = v * kScale;
        return std::isnan(s) ? qint64(0) : qRound64(qBound(-1e15, s, 1e15));
    };
    for (int i = 0; i < n; ++i)
        p.push_back(C2::Point64(q(poly.at(i).x()), q(poly.at(i).y())));
    return p;
}

// Even-odd union of every closed subpath of `paths`: nested contours become
// holes — how CC fills a single element (text counters, a ring) and how its
// toolpaths treat a set of nested vectors.
static C2::Paths64 regionOf(const QVector<QPainterPath> &paths, double tol)
{
    C2::Paths64 subj;
    for (const QPainterPath &pp : paths)
        for (const QPolygonF &poly : finePolygons(pp, tol))
            if (poly.size() > 2)
                subj.push_back(toPath64(poly));
    if (subj.empty())
        return subj;
    return C2::Union(subj, C2::FillRule::EvenOdd);
}

// Fill union across *elements*: each element is its own even-odd region
// (oriented outer+/hole- by Clipper), and those regions weld non-zero, so
// two overlapping squares merge and a shape fully inside another vanishes
// into it — CC's Boolean model.
static C2::Paths64 fillUnion(const QVector<QPainterPath> &paths, double tol)
{
    C2::Paths64 all;
    for (const QPainterPath &pp : paths) {
        const C2::Paths64 r = regionOf({pp}, tol);
        all.insert(all.end(), r.begin(), r.end());
    }
    if (all.empty())
        return all;
    return C2::Union(all, C2::FillRule::NonZero);
}

static QVector<QPolygonF> rings(const C2::Paths64 &paths, double tol)
{
    // Drop the collinear vertices the flattening left behind (within tol).
    const C2::Paths64 simple = C2::SimplifyPaths(paths, tol * kScale, false);
    QVector<QPolygonF> out;
    for (const C2::Path64 &p : simple) {
        if (p.size() < 3)
            continue;
        QPolygonF ring;
        ring.reserve(int(p.size()));
        for (const C2::Point64 &pt : p)
            ring.append(QPointF(pt.x / kScale, pt.y / kScale));
        if (std::fabs(ringArea(ring)) < tol * tol)
            continue;
        out.append(ring);
    }
    return out;
}

QVector<QPolygonF> booleanRings(const QVector<QPainterPath> &subjects,
                                const QVector<QPainterPath> &clips, BoolOp op, double tol)
{
    if (subjects.isEmpty())
        return {};
    C2::Paths64 result;
    switch (op) {
    case BoolOp::Union:
        result = fillUnion(subjects, tol);
        break;
    case BoolOp::Subtract: {
        const C2::Paths64 s = fillUnion(subjects, tol);
        const C2::Paths64 c = fillUnion(clips, tol);
        result = c.empty() ? s : C2::Difference(s, c, C2::FillRule::NonZero);
        break;
    }
    case BoolOp::Intersect: {
        result = regionOf({subjects.first()}, tol);
        for (int i = 1; i < subjects.size() && !result.empty(); ++i)
            result = C2::Intersect(result, regionOf({subjects.at(i)}, tol),
                                   C2::FillRule::NonZero);
        break;
    }
    }
    return rings(result, tol);
}

// The region a set of elements encloses, for an offset: an element wholly
// inside an odd number of others is a hole (a ring drawn as two circles),
// while elements that only overlap weld together. One even-odd pass over
// everything would turn an overlap into a hole.
static C2::Paths64 nestedRegion(const QVector<QPainterPath> &paths, double tol)
{
    QVector<C2::Paths64> regions;
    for (const QPainterPath &pp : paths) {
        C2::Paths64 r = regionOf({pp}, tol);
        if (!r.empty())
            regions.append(std::move(r));
    }
    if (regions.size() > 200)    // containment is pairwise; keep it bounded
        return regionOf(paths, tol);
    QVector<int> depth(regions.size(), 0);
    int deepest = 0;
    for (int i = 0; i < regions.size(); ++i) {
        for (int j = 0; j < regions.size(); ++j)
            if (i != j && C2::Difference(regions.at(i), regions.at(j), C2::FillRule::NonZero).empty()
                && !(C2::Difference(regions.at(j), regions.at(i), C2::FillRule::NonZero).empty() && j > i))
                ++depth[i];
        deepest = qMax(deepest, depth.at(i));
    }
    C2::Paths64 out;
    for (int d = 0; d <= deepest; ++d) {
        C2::Paths64 level;
        for (int i = 0; i < regions.size(); ++i)
            if (depth.at(i) == d)
                level.insert(level.end(), regions.at(i).begin(), regions.at(i).end());
        if (level.empty())
            continue;
        out = d % 2 == 0 ? C2::Union(out, level, C2::FillRule::NonZero)
                         : C2::Difference(out, level, C2::FillRule::NonZero);
    }
    return out;
}

QVector<QPolygonF> offsetRings(const QVector<QPainterPath> &closed,
                               const QVector<QPainterPath> &open, double delta, double tol)
{
    const double arcTol = tol * kScale;
    C2::Paths64 out;
    if (!closed.isEmpty()) {
        const C2::Paths64 region = nestedRegion(closed, tol);
        if (!region.empty())
            out = C2::InflatePaths(region, delta * kScale, C2::JoinType::Round,
                                   C2::EndType::Polygon, 2.0, arcTol);
    }
    if (!open.isEmpty() && !qFuzzyIsNull(delta)) {
        C2::Paths64 lines;
        for (const QPainterPath &pp : open)
            for (const QPolygonF &poly : finePolygons(pp, tol))
                if (poly.size() > 1)   // an open path ending on its start keeps that last side
                    lines.push_back(toPath64(poly, false));
        const C2::Paths64 stroke = C2::InflatePaths(lines, std::fabs(delta) * kScale,
                                                    C2::JoinType::Round, C2::EndType::Round,
                                                    2.0, arcTol);
        out.insert(out.end(), stroke.begin(), stroke.end());
    }
    // A single union pass normalizes orientation and welds overlapping
    // stroke outlines with the region offsets.
    if (out.empty())
        return {};
    return rings(C2::Union(out, C2::FillRule::NonZero), tol);
}

#else // !HAVE_CLIPPER2 — approximate Qt path booleans, no offsetting.

static QPainterPath regionPath(const QVector<QPainterPath> &paths, double tol)
{
    QPainterPath r;
    r.setFillRule(Qt::OddEvenFill);
    for (const QPainterPath &pp : paths)
        for (const QPolygonF &poly : finePolygons(pp, tol))
            if (poly.size() > 2)
                r.addPolygon(poly);
    return r.simplified();
}

static QVector<QPolygonF> ringsOf(const QPainterPath &p, double tol)
{
    QVector<QPolygonF> out;
    for (QPolygonF poly : p.toSubpathPolygons()) {
        if (poly.isClosed())
            poly.removeLast();
        if (poly.size() > 2 && std::fabs(ringArea(poly)) >= tol * tol)
            out.append(poly);
    }
    return out;
}

QVector<QPolygonF> booleanRings(const QVector<QPainterPath> &subjects,
                                const QVector<QPainterPath> &clips, BoolOp op, double tol)
{
    if (subjects.isEmpty())
        return {};
    QPainterPath r = regionPath({subjects.first()}, tol);
    if (op == BoolOp::Union)
        for (int i = 1; i < subjects.size(); ++i)
            r = r.united(regionPath({subjects.at(i)}, tol));
    if (op == BoolOp::Subtract)
        for (const QPainterPath &c : clips)
            r = r.subtracted(regionPath({c}, tol));
    else if (op == BoolOp::Intersect)
        for (int i = 1; i < subjects.size(); ++i)
            r = r.intersected(regionPath({subjects.at(i)}, tol));
    return ringsOf(r, tol);
}

QVector<QPolygonF> offsetRings(const QVector<QPainterPath> &, const QVector<QPainterPath> &,
                               double, double)
{
    return {};
}
#endif

QVector<Element> elementsFromRings(const QVector<QPolygonF> &rings, const QJsonObject &layer)
{
    QVector<Element> out;
    for (const QPolygonF &ring : rings) {
        QVector<QPointF> pts;
        pts.reserve(ring.size());
        for (const QPointF &p : ring)
            pts.append(p);
        out.append(Element::makePath(pts, true, layer));
    }
    return out;
}

static QJsonObject layerOf(const QVector<Element> &inputs)
{
    return inputs.isEmpty() ? QJsonObject() : inputs.first().raw.value("layer").toObject();
}

QVector<Element> booleanElements(const QVector<Element> &inputs, BoolOp op, double tol)
{
    QVector<QPainterPath> subjects, clips;
    for (int i = 0; i < inputs.size(); ++i) {
        if (!isClosed(inputs.at(i)))
            continue;
        if (op == BoolOp::Subtract && i > 0)
            clips.append(inputs.at(i).painterPath);
        else
            subjects.append(inputs.at(i).painterPath);
    }
    return elementsFromRings(booleanRings(subjects, clips, op, tol), layerOf(inputs));
}

QVector<Element> offsetElements(const QVector<Element> &inputs, double delta, double tol)
{
    QVector<QPainterPath> closed, open;
    for (const Element &e : inputs)
        (isClosed(e) ? closed : open).append(e.painterPath);
    return elementsFromRings(offsetRings(closed, open, delta, tol), layerOf(inputs));
}

// ---- alignment ----------------------------------------------------------
// QRectF in CC's Y-up space: top() is the *minimum* y (bottom edge on the
// board) and bottom() the maximum. Spell it out to keep the code readable.
static double minX(const QRectF &r) { return r.left(); }
static double maxX(const QRectF &r) { return r.right(); }
static double minY(const QRectF &r) { return r.top(); }
static double maxY(const QRectF &r) { return r.bottom(); }

QVector<QPointF> alignDeltas(const QVector<QRectF> &boxes, Align mode, const QRectF &ref)
{
    QVector<QPointF> d;
    d.reserve(boxes.size());
    for (const QRectF &b : boxes) {
        switch (mode) {
        case Align::Left:    d.append({minX(ref) - minX(b), 0}); break;
        case Align::Right:   d.append({maxX(ref) - maxX(b), 0}); break;
        case Align::HCenter: d.append({ref.center().x() - b.center().x(), 0}); break;
        case Align::Bottom:  d.append({0, minY(ref) - minY(b)}); break;
        case Align::Top:     d.append({0, maxY(ref) - maxY(b)}); break;
        case Align::VCenter: d.append({0, ref.center().y() - b.center().y()}); break;
        }
    }
    return d;
}

QVector<QPointF> centerDeltas(const QVector<QRectF> &boxes, Center mode, const QRectF &stock)
{
    // By hand: isNull() and united() both take a box of no size (a zero-length
    // line) for "no box", which left such a vector out of the group. Only the
    // box of an empty path, QRectF(), is no box.
    QRectF all;
    bool have = false;
    for (const QRectF &b : boxes) {
        if (b == QRectF())
            continue;
        if (!have) {
            all = b;
            have = true;
            continue;
        }
        all = QRectF(QPointF(qMin(all.left(), b.left()), qMin(all.top(), b.top())),
                     QPointF(qMax(all.right(), b.right()), qMax(all.bottom(), b.bottom())));
    }
    const QPointF shift = stock.center() - all.center();
    const QPointF d(mode == Center::Vertical ? 0.0 : shift.x(),
                    mode == Center::Horizontal ? 0.0 : shift.y());
    return QVector<QPointF>(boxes.size(), d);
}

QVector<QPointF> distributeDeltas(const QVector<QRectF> &boxes, Axis axis)
{
    const int n = boxes.size();
    QVector<QPointF> d(n, QPointF(0, 0));
    if (n < 3)
        return d;
    auto coord = [axis](const QRectF &b) {
        return axis == Axis::Horizontal ? b.center().x() : b.center().y();
    };
    QVector<int> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return coord(boxes.at(a)) < coord(boxes.at(b)); });
    const double first = coord(boxes.at(order.first()));
    const double last = coord(boxes.at(order.last()));
    const double step = (last - first) / (n - 1);
    for (int rank = 0; rank < n; ++rank) {
        const int i = order.at(rank);
        const double shift = first + rank * step - coord(boxes.at(i));
        d[i] = axis == Axis::Horizontal ? QPointF(shift, 0) : QPointF(0, shift);
    }
    return d;
}

// ---- mirror and array ------------------------------------------------------

static QPointF jsonPt(const QJsonValue &v)
{
    const QJsonArray a = v.toArray();
    return a.size() == 2 ? QPointF(a.at(0).toDouble(), a.at(1).toDouble()) : QPointF();
}

static QJsonArray jsonXY(const QPointF &p)
{
    return QJsonArray{p.x(), p.y()};
}

// Every anchor and handle of a model, as one list.
static QVector<QPointF> modelPoints(const PathModel &m)
{
    QVector<QPointF> out;
    for (const SubPath &s : m.subs)
        for (const PathNode &n : s.nodes)
            out << n.p << n.in << n.out;
    return out;
}

// Same outline up to node order and direction: the anchors and handles of
// the two models match one to one. Enough to tell that a rectangle or a
// regular polygon came out of a transform as the same shape.
static bool sameOutline(const PathModel &a, const PathModel &b)
{
    const QVector<QPointF> pa = modelPoints(a), pb = modelPoints(b);
    if (pa.size() != pb.size() || pa.isEmpty())
        return false;
    QVector<bool> used(pb.size(), false);
    for (const QPointF &p : pa) {
        bool found = false;
        for (int i = 0; i < pb.size(); ++i) {
            if (!used.at(i) && QLineF(p, pb.at(i)).length() < 1e-6) {
                used[i] = true;
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

static PathModel mapModel(const PathModel &m, const QTransform &t)
{
    PathModel out = m;
    const bool flip = t.determinant() < 0;
    for (SubPath &s : out.subs) {
        for (PathNode &n : s.nodes) {
            n.p = t.map(n.p);
            n.in = t.map(n.in);
            n.out = t.map(n.out);
        }
        if (!flip || s.nodes.size() < 2)
            continue;
        // A reflection turns CCW into CW. Walk the subpath backwards (a closed
        // one keeps its start node) so the winding the file had is kept; the
        // handles swap sides along with the direction.
        QVector<PathNode> r;
        const int n = s.nodes.size();
        if (s.closed) {
            r << s.nodes.first();
            for (int i = n - 1; i >= 1; --i)
                r << s.nodes.at(i);
        } else {
            for (int i = n - 1; i >= 0; --i)
                r << s.nodes.at(i);
        }
        for (PathNode &node : r)
            std::swap(node.in, node.out);
        s.nodes = r;
    }
    return out;
}

Element transformElement(const Element &e, const QTransform &t)
{
    if (t.isIdentity())
        return e;
    if (t.type() <= QTransform::TxTranslate) {
        Element c = e;
        c.translate(t.dx(), t.dy());
        return c;
    }

    if (e.geometryType == QLatin1String("text")) {
        // Text is placed by its own row-major 3x3; compose ours after it.
        QJsonObject o = e.raw;
        const QJsonArray a = o.value("transform").toArray();
        QTransform xf;
        if (a.size() == 9)
            xf = QTransform(a.at(0).toDouble(), a.at(1).toDouble(),
                            a.at(3).toDouble(), a.at(4).toDouble(),
                            a.at(6).toDouble(), a.at(7).toDouble());
        QTransform n = xf * t;
        // A uniform scale goes into the text's own sizes (font height, arc
        // radius and centre, and the outlines, which are scaled as they are
        // rather than laid out again), so Height in Properties stays the
        // drawn size. Only rotation / reflection is left in the transform.
        // A stretch cannot be a font size and stays in the transform.
        const double sx = std::hypot(t.m11(), t.m12()), sy = std::hypot(t.m21(), t.m22());
        const bool similar = std::fabs(sx - sy) < 1e-9 * qMax(1.0, sx)
                          && std::fabs(t.m11() * t.m21() + t.m12() * t.m22()) < 1e-9 * qMax(1.0, sx * sy);
        if (similar && sx > 1e-12 && std::fabs(sx - 1.0) > 1e-12) {
            const double k = sx;
            QTransform shrink;
            shrink.scale(1.0 / k, 1.0 / k);
            n = shrink * n;
            o.insert("font_height", o.value("font_height").toDouble(10.0) * k);
            if (o.contains("arc_radius"))
                o.insert("arc_radius", o.value("arc_radius").toDouble() * k);
            if (o.value("arc_center").toArray().size() == 2)
                o.insert("arc_center", jsonXY(jsonPt(o.value("arc_center")) * k));
            QJsonArray rendered;
            for (const QJsonValue &cv : o.value("rendered").toArray()) {
                QJsonArray contour;
                for (const QJsonValue &pv : cv.toArray())
                    contour.append(jsonXY(jsonPt(pv) * k));
                rendered.append(contour);
            }
            o.insert("rendered", rendered);
        }
        // The fold leaves (1/k)*k rounding noise (0.9999999999999999) and
        // a turn there and back leaves ~1e-17 off-diagonals: snap them, so a
        // plain text reads as plain again.
        auto snap = [](double v) {
            for (double to : {0.0, 1.0, -1.0})
                if (std::fabs(v - to) < 1e-12)
                    return to;
            return v;
        };
        o.insert("transform", QJsonArray{snap(n.m11()), snap(n.m12()), 0, snap(n.m21()),
                                         snap(n.m22()), 0, n.dx(), n.dy(), 1});
        return Element::fromJson(o);
    }

    const bool shape = e.geometryType == QLatin1String("circle")
                    || e.geometryType == QLatin1String("rectangle")
                    || e.geometryType == QLatin1String("regular_polygon");
    if (shape && e.raw.contains("center")) {
        const QPointF c = jsonPt(e.raw.value("center"));
        const QPointF nc = t.map(c);
        if (Element::isPoint(e)) {
            // A sketch point has no size: it only moves, never stretches.
            Element moved = e;
            moved.translate(nc.x() - c.x(), nc.y() - c.y());
            return moved;
        }
        // How the transform scales: lengths along x and y, and whether it
        // keeps angles (rotation / reflection / uniform scale).
        const double sx = std::hypot(t.m11(), t.m12()), sy = std::hypot(t.m21(), t.m22());
        const bool similar = std::fabs(sx - sy) < 1e-9 * qMax(1.0, sx)
                          && std::fabs(t.m11() * t.m21() + t.m12() * t.m22()) < 1e-9 * qMax(1.0, sx * sy);
        const bool rigid = similar && std::fabs(sx - 1.0) < 1e-9;
        Element cand;
        if (rigid) {
            cand = e;
            const QPointF moved = nc - c;
            cand.translate(moved.x(), moved.y());
            // A circle is the same circle under any rotation or reflection.
            if (e.geometryType == QLatin1String("circle"))
                return cand;
        } else {
            QHash<QString, double> p;
            p.insert("cx", nc.x());
            p.insert("cy", nc.y());
            if (e.geometryType == QLatin1String("circle")) {
                if (!similar)   // stretched: an ellipse, which only a path can be
                    return Element::withPathModel(e, mapModel(Element::pathModel(e), t));
                p.insert("radius", e.raw.value("radius").toDouble() * sx);
                return Element::regen(e, p);
            }
            if (e.geometryType == QLatin1String("rectangle")) {
                if (similar) {
                    p.insert("width", e.raw.value("width").toDouble() * sx);
                    p.insert("height", e.raw.value("height").toDouble() * sx);
                } else if (std::fabs(t.m12()) < 1e-12 && std::fabs(t.m21()) < 1e-12) {
                    p.insert("width", e.raw.value("width").toDouble() * std::fabs(t.m11()));
                    p.insert("height", e.raw.value("height").toDouble() * std::fabs(t.m22()));
                }
            } else if (similar) {
                p.insert("radius", e.raw.value("radius").toDouble() * sx);
            }
            cand = Element::regen(e, p);
        }
        const PathModel target = mapModel(Element::pathModel(e), t);
        if (sameOutline(Element::pathModel(cand), target))
            return cand;   // symmetric under t: a rectangle flipped, a hexagon turned by 60 deg
        if (similar && e.geometryType == QLatin1String("regular_polygon") && !target.isEmpty()
            && !target.subs.first().nodes.isEmpty()) {
            // Still a regular polygon, just turned: the rotation key is the
            // angle of its first vertex about the center.
            const QPointF v = target.subs.first().nodes.first().p - nc;
            QHash<QString, double> p;
            p.insert("cx", nc.x());
            p.insert("cy", nc.y());
            p.insert("radius", e.raw.value("radius").toDouble() * sx);
            p.insert("rotation", qRadiansToDegrees(std::atan2(v.y(), v.x())));
            const Element poly = Element::regen(e, p);
            if (sameOutline(Element::pathModel(poly), target))
                return poly;
        }
        return Element::withPathModel(e, target);
    }

    return Element::withPathModel(e, mapModel(Element::pathModel(e), t));
}

QTransform mirrorTransform(const QRectF &box, Axis axis)
{
    const QPointF c = box.center();
    QTransform t;
    t.translate(c.x(), c.y());
    if (axis == Axis::Horizontal)
        t.scale(-1, 1);
    else
        t.scale(1, -1);
    t.translate(-c.x(), -c.y());
    return t;
}

QVector<QTransform> gridTransforms(const QRectF &box, int cols, int rows,
                                   double gapX, double gapY)
{
    QVector<QTransform> out;
    if (cols < 1 || rows < 1)
        return out;
    const double stepX = box.width() + gapX, stepY = box.height() + gapY;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c)
            if (r || c)
                out.append(QTransform::fromTranslate(c * stepX, r * stepY));
    return out;
}

QVector<QTransform> circularTransforms(const QRectF &box, QPointF center, int count,
                                       double spanDeg, bool rotate)
{
    QVector<QTransform> out;
    if (count < 2)
        return out;
    const double step = std::fabs(spanDeg) >= 360.0 ? (spanDeg < 0 ? -360.0 : 360.0) / count
                                                    : spanDeg / (count - 1);
    const QPointF b = box.center();
    for (int i = 1; i < count; ++i) {
        const double a = i * step;
        QTransform turn;
        turn.translate(center.x(), center.y());
        turn.rotate(a);
        turn.translate(-center.x(), -center.y());
        if (rotate) {
            out.append(turn);
        } else {
            const QPointF d = turn.map(b) - b;
            out.append(QTransform::fromTranslate(d.x(), d.y()));
        }
    }
    return out;
}

QVector<QTransform> pathTransforms(const QPainterPath &path, int count, double spacing,
                                   bool orient)
{
    QVector<QTransform> out;
    if (count < 2)
        return out;
    // The path's first subpath, flattened finely (Qt's own flattening is
    // coarse at millimetre scale).
    const double k = 100.0;
    QList<QPolygonF> polys = path.toSubpathPolygons(QTransform::fromScale(k, k));
    if (polys.isEmpty() || polys.first().size() < 2)
        return out;
    QPolygonF poly = polys.first();
    for (QPointF &p : poly)
        p /= k;
    QVector<double> acc{0.0};
    for (int i = 1; i < poly.size(); ++i)
        acc.append(acc.last() + QLineF(poly.at(i - 1), poly.at(i)).length());
    const double total = acc.last();
    if (total < 1e-9)
        return out;
    const bool closed = QLineF(poly.first(), poly.last()).length() < 1e-6;
    // Point and direction (degrees) at arc length s.
    auto at = [&](double s, QPointF *pt, double *deg) {
        // On a vertex, the segment leaving it (as for the original at 0): a
        // copy on a square's corner turned one edge too little.
        int i = int(std::upper_bound(acc.begin(), acc.end(), s + 1e-6) - acc.begin());
        i = qBound(1, i, int(poly.size()) - 1);
        while (i < poly.size() - 1 && acc.at(i) - acc.at(i - 1) < 1e-12)
            ++i;
        const QLineF seg(poly.at(i - 1), poly.at(i));
        const double len = acc.at(i) - acc.at(i - 1);
        *pt = len > 1e-12 ? seg.pointAt(qBound(0.0, (s - acc.at(i - 1)) / len, 1.0)) : seg.p2();
        *deg = std::atan2(seg.dy(), seg.dx()) * 180.0 / M_PI;
    };
    QPointF p0;
    double d0 = 0;
    at(0, &p0, &d0);
    for (int i = 1; i < count; ++i) {
        const double s = spacing > 0 ? i * spacing
                       : closed ? total * i / count : total * i / (count - 1);
        if (s > total + 1e-9)
            break;
        // Round a closed path back at the start: a copy there would sit on
        // the original (stacked vectors cancel in a pocket).
        if (closed && s > total - 0.01)
            break;
        QPointF p;
        double d = 0;
        at(s, &p, &d);
        // A copy that lands on the start (a figure-eight crossing it, a tiny
        // circle) is skipped; the path goes on past it.
        if (closed && QLineF(p, p0).length() < 0.05)
            continue;
        QTransform t;
        t.translate(p.x(), p.y());
        if (orient)
            t.rotate(d - d0);
        t.translate(-p0.x(), -p0.y());
        out.append(t);
    }
    return out;
}

QVector<Element> copyElements(const QVector<Element> &els, const QTransform &t)
{
    QHash<QString, QString> groups;
    QVector<Element> out;
    for (const Element &e : els) {
        QJsonObject o = transformElement(e, t).raw;
        o.insert("id", QUuid::createUuid().toString());
        QJsonArray g;
        for (const QJsonValue &v : o.value("group_id").toArray()) {
            if (!v.isString()) { g.append(v); continue; }
            QString &mapped = groups[v.toString()];
            if (mapped.isEmpty())
                mapped = QUuid::createUuid().toString();
            g.append(mapped);
        }
        o.insert("group_id", g);
        out.append(Element::fromJson(o));
    }
    return out;
}

// ---- rotate / scale --------------------------------------------------------

QTransform rotateTransform(QPointF center, double deg)
{
    QTransform t;
    t.translate(center.x(), center.y());
    t.rotate(deg);
    t.translate(-center.x(), -center.y());
    return t;
}

QTransform scaleTransform(QPointF anchor, double sx, double sy)
{
    QTransform t;
    t.translate(anchor.x(), anchor.y());
    t.scale(sx, sy);
    t.translate(-anchor.x(), -anchor.y());
    return t;
}

// ---- fillet / chamfer -------------------------------------------------------

namespace {

QPointF unit(const QPointF &v)
{
    const double l = std::hypot(v.x(), v.y());
    return l > 0 ? v / l : QPointF();
}

double len(const QPointF &v) { return std::hypot(v.x(), v.y()); }

// The segment arriving at / leaving node i is a straight line.
bool straightIn(const SubPath &s, int i)
{
    const int n = s.nodes.size();
    if (!s.closed && i == 0)
        return false;
    const PathNode &prev = s.nodes.at((i - 1 + n) % n);
    return !prev.hasOut() && !s.nodes.at(i).hasIn();
}

bool straightOut(const SubPath &s, int i)
{
    const int n = s.nodes.size();
    if (!s.closed && i == n - 1)
        return false;
    const PathNode &next = s.nodes.at((i + 1) % n);
    return !s.nodes.at(i).hasOut() && !next.hasIn();
}

// A sharp corner between two straight segments, turning by more than 1°
// (anything less is a flattened curve, not a corner).
bool isCorner(const SubPath &s, int i)
{
    const int n = s.nodes.size();
    if (n < 3 && s.closed)
        return false;
    if (!straightIn(s, i) || !straightOut(s, i))
        return false;
    const QPointF p = s.nodes.at(i).p;
    const QPointF a = s.nodes.at((i - 1 + n) % n).p - p;
    const QPointF b = s.nodes.at((i + 1) % n).p - p;
    if (len(a) < 1e-9 || len(b) < 1e-9)
        return false;
    const double cosA = QPointF::dotProduct(unit(a), unit(b));
    return cosA > -std::cos(qDegreesToRadians(1.0));
}

} // namespace

PathModel cornerModel(const PathModel &m, CornerStyle style, double size, int *corners)
{
    PathModel out;
    int count = 0;
    for (const SubPath &s : m.subs) {
        const int n = s.nodes.size();
        QVector<bool> corner(n, false);
        for (int i = 0; i < n; ++i)
            corner[i] = isCorner(s, i);
        SubPath r;
        r.closed = s.closed;
        for (int i = 0; i < n; ++i) {
            const PathNode &node = s.nodes.at(i);
            if (!corner.at(i) || size <= 0) {
                r.nodes.append(node);
                continue;
            }
            const int ip = (i - 1 + n) % n, in = (i + 1) % n;
            const QPointF p = node.p;
            const QPointF a = s.nodes.at(ip).p - p, b = s.nodes.at(in).p - p;
            const QPointF u = unit(a), v = unit(b);
            const double phi = std::acos(qBound(-1.0, QPointF::dotProduct(u, v), 1.0));
            // How far along each edge the cut starts: a neighbouring corner
            // that is being cut too gets the other half of the shared edge.
            const double room = qMin(len(a) * (corner.at(ip) ? 0.5 : 1.0),
                                     len(b) * (corner.at(in) ? 0.5 : 1.0));
            double d = style == CornerStyle::Fillet ? size / std::tan(phi / 2) : size;
            d = qMin(d, room);
            if (d < 1e-9) {
                r.nodes.append(node);
                continue;
            }
            PathNode t1, t2;
            t1.p = t1.in = t1.out = p + u * d;
            t2.p = t2.in = t2.out = p + v * d;
            if (style == CornerStyle::Fillet) {
                const double radius = d * std::tan(phi / 2);
                const double sweep = M_PI - phi;
                const double h = 4.0 / 3.0 * std::tan(sweep / 4) * radius;
                t1.out = t1.p - u * h;
                t2.in = t2.p - v * h;
            }
            r.nodes << t1 << t2;
            ++count;
        }
        // Two cuts that met in the middle of an edge leave two nodes on one
        // spot: fuse them (the first's incoming handle, the second's outgoing).
        for (int i = 0; r.nodes.size() > 2 && i < r.nodes.size();) {
            const int j = i + 1 < r.nodes.size() ? i + 1 : (r.closed ? 0 : -1);
            if (j < 0 || j == i || len(r.nodes.at(i).p - r.nodes.at(j).p) > 1e-9) {
                ++i;
                continue;
            }
            PathNode f = r.nodes.at(i);
            f.out = r.nodes.at(j).out;
            if (f.hasIn() && f.hasOut())
                f.kind = PathNode::Smooth;
            r.nodes[i] = f;
            r.nodes.removeAt(j);
            if (j < i)
                --i;
        }
        out.subs.append(r);
    }
    if (corners)
        *corners = count;
    return out;
}

Element cornerElement(const Element &e, CornerStyle style, double size, int *corners)
{
    if (corners)
        *corners = 0;
    if (e.geometryType == QLatin1String("text") || e.geometryType == QLatin1String("circle"))
        return e;
    int k = 0;
    const PathModel m = cornerModel(Element::pathModel(e), style, size, &k);
    if (corners)
        *corners = k;
    return k ? Element::withPathModel(e, m) : e;
}

// ---- join / check ----------------------------------------------------------

static SubPath reversedSub(const SubPath &s)
{
    SubPath r;
    r.closed = s.closed;
    for (int i = s.nodes.size() - 1; i >= 0; --i) {
        PathNode n = s.nodes.at(i);
        std::swap(n.in, n.out);
        r.nodes.append(n);
    }
    return r;
}

static double ptDist(QPointF a, QPointF b)
{
    return QLineF(a, b).length();
}

// Append `b` to `a`, b's first node landing on a's last (its leaving handle
// moved along, so the first segment of b keeps its shape).
static void appendSub(SubPath &a, const SubPath &b)
{
    PathNode &end = a.nodes.last();
    const PathNode &head = b.nodes.first();
    end.out = head.out + (end.p - head.p);
    end.kind = PathNode::Corner;
    for (int i = 1; i < b.nodes.size(); ++i)
        a.nodes.append(b.nodes.at(i));
}

static bool zeroLengthSub(const SubPath &s)
{
    for (const PathNode &n : s.nodes)
        if (ptDist(n.p, s.nodes.first().p) > 1e-9 || n.hasIn() || n.hasOut())
            return false;
    return true;
}

static bool joinCandidate(const Element &e)
{
    return e.geometryType == QLatin1String("path") && !Element::isPoint(e)
        && !Element::isConstruction(e);
}

JoinResult joinElements(const QVector<Element> &els, double gap)
{
    JoinResult out;
    gap = std::max(0.0, gap) + 1e-9;
    struct Piece { SubPath sub; int el; };
    QVector<PathModel> models(els.size());
    QVector<Piece> pieces;
    for (int k = 0; k < els.size(); ++k) {
        if (!joinCandidate(els.at(k)))
            continue;
        models[k] = Element::pathModel(els.at(k));
        for (const SubPath &s : models.at(k).subs)
            if (!s.closed && s.nodes.size() >= 2 && !zeroLengthSub(s))
                pieces.append({s, k});
    }
    if (pieces.isEmpty())
        return out;

    // Union-find over elements linked by a changed chain.
    QVector<int> parent(els.size());
    std::iota(parent.begin(), parent.end(), 0);
    std::function<int(int)> root = [&](int i) {
        return parent[i] == i ? i : parent[i] = root(parent[i]);
    };
    struct Chain { SubPath sub; int el; bool changed; };
    QVector<Chain> chains;
    QVector<bool> used(pieces.size(), false);
    for (int i = 0; i < pieces.size(); ++i) {
        if (used.at(i))
            continue;
        used[i] = true;
        SubPath chain = pieces.at(i).sub;
        const int el = pieces.at(i).el;
        bool changed = false;
        for (bool grew = true; grew;) {
            grew = false;
            for (int side = 0; side < 2 && !grew; ++side) {
                // side 0: grow at the chain's end, side 1: at its start.
                const QPointF at = side == 0 ? chain.nodes.last().p : chain.nodes.first().p;
                int best = -1;
                bool bestFlip = false;
                double bestD = gap;
                for (int j = 0; j < pieces.size(); ++j) {
                    if (used.at(j))
                        continue;
                    const SubPath &s = pieces.at(j).sub;
                    const double dFirst = ptDist(at, s.nodes.first().p);
                    const double dLast = ptDist(at, s.nodes.last().p);
                    // At the end a piece should start there, at the start end there.
                    const double dKeep = side == 0 ? dFirst : dLast;
                    const double dFlip = side == 0 ? dLast : dFirst;
                    if (dKeep <= bestD) { best = j; bestFlip = false; bestD = dKeep; }
                    if (dFlip < bestD) { best = j; bestFlip = true; bestD = dFlip; }
                }
                if (best < 0)
                    continue;
                used[best] = true;
                const SubPath piece = bestFlip ? reversedSub(pieces.at(best).sub)
                                               : pieces.at(best).sub;
                if (side == 0) {
                    appendSub(chain, piece);
                } else {
                    SubPath front = piece;
                    appendSub(front, chain);
                    chain = front;
                }
                parent[root(pieces.at(best).el)] = root(el);
                ++out.joints;
                changed = grew = true;
            }
        }
        if (chain.nodes.size() >= 2
            && ptDist(chain.nodes.first().p, chain.nodes.last().p) <= gap) {
            SubPath closed = chain;
            const PathNode last = closed.nodes.takeLast();
            PathNode &first = closed.nodes.first();
            first.in = last.in + (first.p - last.p);
            first.kind = PathNode::Corner;
            closed.closed = true;
            // Two straight lines back and forth enclose nothing.
            bool curved = false;
            for (const PathNode &n : closed.nodes)
                curved = curved || n.hasIn() || n.hasOut();
            if (closed.nodes.size() >= 3 || (closed.nodes.size() == 2 && curved)) {
                chain = closed;
                ++out.closed;
                changed = true;
            }
        }
        chains.append({chain, el, changed});
    }

    QSet<int> consumedRoots;
    for (const Chain &c : chains)
        if (c.changed)
            consumedRoots.insert(root(c.el));
    if (consumedRoots.isEmpty())
        return out;
    // One result per linked group, in the order of its first element.
    for (int k = 0; k < els.size(); ++k) {
        if (!consumedRoots.contains(k) || root(k) != k)
            continue;
        QVector<int> members;
        for (int m = 0; m < els.size(); ++m)
            if (joinCandidate(els.at(m)) && root(m) == k)
                members.append(m);
        PathModel model;
        for (int m : members) {
            out.consumed.append(els.at(m));
            for (const SubPath &s : models.at(m).subs)
                if (s.closed)
                    model.subs.append(s);
        }
        for (const Chain &c : chains)
            if (root(c.el) == k)
                model.subs.append(c.sub);
        const Element &first = els.at(members.first());
        out.results.append(members.size() == 1 ? Element::withPathModel(first, model)
                                               : Element::pieceOf(first, model));
        for (int m : members)
            out.resultOf.insert(els.at(m).id, out.results.last().id);
    }
    return out;
}

QStringList VectorCheck::allIds() const
{
    QStringList ids;
    for (const QStringList *l : {&open, &duplicates, &zeroLength, &selfCrossing})
        for (const QString &id : *l)
            if (!ids.contains(id))
                ids << id;
    return ids;
}

static double crossZ(QPointF a, QPointF b, QPointF c)
{
    return (b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x());
}

// Two edges cross each other strictly (touching or overlapping does not count).
static bool edgesCross(QPointF p1, QPointF p2, QPointF q1, QPointF q2)
{
    const double eps = 1e-12;
    const double d1 = crossZ(q1, q2, p1), d2 = crossZ(q1, q2, p2);
    const double d3 = crossZ(p1, p2, q1), d4 = crossZ(p1, p2, q2);
    return ((d1 > eps && d2 < -eps) || (d1 < -eps && d2 > eps))
        && ((d3 > eps && d4 < -eps) || (d3 < -eps && d4 > eps));
}

// 1 crosses, 0 does not, -1 too many points to check.
static int ringCrossesItself(QPolygonF ring)
{
    QPolygonF r;
    for (const QPointF &p : ring)
        if (r.isEmpty() || ptDist(r.last(), p) > 1e-9)
            r.append(p);
    while (r.size() > 1 && ptDist(r.first(), r.last()) <= 1e-9)
        r.removeLast();
    const int n = r.size();
    if (n < 4)
        return 0;
    if (n > 20000)
        return -1;
    QVector<QRectF> box(n);
    for (int i = 0; i < n; ++i)
        box[i] = QRectF(r.at(i), r.at((i + 1) % n)).normalized();
    for (int i = 0; i < n; ++i)
        for (int j = i + 2; j < n; ++j) {
            if (i == 0 && j == n - 1)
                continue;   // neighbours across the start
            if (box.at(i).right() < box.at(j).left() || box.at(j).right() < box.at(i).left()
                || box.at(i).bottom() < box.at(j).top() || box.at(j).bottom() < box.at(i).top())
                continue;
            if (edgesCross(r.at(i), r.at((i + 1) % n), r.at(j), r.at((j + 1) % n)))
                return 1;
        }
    return 0;
}

VectorCheck checkElements(const QVector<Element> &els)
{
    VectorCheck out;
    QHash<QString, QVector<int>> seen;   // rough key -> earlier elements
    for (int k = 0; k < els.size(); ++k) {
        const Element &e = els.at(k);
        const QPainterPath &pp = e.painterPath;
        if (pp.length() < 1e-6) {
            out.zeroLength << e.id;
            continue;
        }
        const QRectF bb = pp.boundingRect();
        const QString key = QStringLiteral("%1/%2/%3/%4/%5")
                                .arg(qRound64(bb.left() * 1000)).arg(qRound64(bb.top() * 1000))
                                .arg(qRound64(bb.right() * 1000)).arg(qRound64(bb.bottom() * 1000))
                                .arg(pp.elementCount());
        bool dup = false;
        for (int m : seen.value(key))
            if (els.at(m).painterPath == pp) { dup = true; break; }
        if (dup)
            out.duplicates << e.id;
        else
            seen[key].append(k);
        if (e.geometryType != QLatin1String("path") || Element::isPoint(e))
            continue;
        const PathModel model = Element::pathModel(e);
        bool open = false, crosses = false, unchecked = false;
        for (const SubPath &s : model.subs) {
            if (!s.closed) {
                open = open || !Element::isConstruction(e);
                continue;
            }
            if (crosses || s.nodes.size() < 2)
                continue;
            PathModel one;
            one.subs.append(s);
            const QList<QPolygonF> polys = one.painterPath().toSubpathPolygons();
            const int x = polys.isEmpty() ? 0 : ringCrossesItself(polys.first());
            crosses = x > 0;
            unchecked = unchecked || x < 0;
        }
        if (unchecked && !crosses)
            out.notChecked << e.id;
        if (open)
            out.open << e.id;
        if (crosses)
            out.selfCrossing << e.id;
    }
    return out;
}

// ---- text on path -------------------------------------------------------------

QPainterPath textOnPathOutline(const QJsonObject &textObj, const QPainterPath &path,
                               const TextOnPathOptions &opt, QVector<GlyphPlace> *places)
{
    QPainterPath out;
    if (places)
        places->clear();
    double width = 0;
    QVector<TextGlyph> glyphs = Element::textGlyphs(textObj, &width);
    if (glyphs.isEmpty())
        return out;
    // A stretch kept in the text's transform (a uniform scale is in its
    // height already); its turn and place do not matter on the path.
    const QJsonArray tf = textObj.value("transform").toArray();
    if (tf.size() == 9) {
        const double sx = std::hypot(tf.at(0).toDouble(), tf.at(1).toDouble());
        const double sy = std::hypot(tf.at(3).toDouble(), tf.at(4).toDouble());
        if (sx > 1e-12 && sy > 1e-12 && (std::fabs(sx - 1) > 1e-9 || std::fabs(sy - 1) > 1e-9)) {
            const QTransform st = QTransform::fromScale(sx, sy);
            for (TextGlyph &g : glyphs) {
                g.path = st.map(g.path);
                g.cluster *= sx;
                g.advance *= sx;
            }
            width *= sx;
        }
    }
    // The first subpath, flattened finely so curves are measured by their
    // true length (Qt's own flattening is coarse at millimetre scale).
    const double k = 100.0;
    const QList<QPolygonF> polys = path.toSubpathPolygons(QTransform::fromScale(k, k));
    if (polys.isEmpty() || polys.first().size() < 2)
        return out;
    QPolygonF poly;
    for (const QPointF &p : polys.first())
        if (poly.isEmpty() || QLineF(poly.last(), p / k).length() > 1e-9)
            poly.append(p / k);
    if (poly.size() < 2)
        return out;
    if (opt.flip)
        std::reverse(poly.begin(), poly.end());
    QVector<double> acc{0.0};
    for (int i = 1; i < poly.size(); ++i)
        acc.append(acc.last() + QLineF(poly.at(i - 1), poly.at(i)).length());
    const double total = acc.last();
    if (total < 1e-9)
        return out;
    const bool closed = opt.closed != 0 && path.elementCount() > 0
                        && QLineF(poly.first(), poly.last()).length() < 1e-6;
    // Point at arc length s: wrapped on a closed path, carried on along the
    // end direction past an open path's ends.
    auto pointAt = [&](double s) -> QPointF {
        if (closed) {
            s = std::fmod(s, total);
            if (s < 0)
                s += total;
        } else if (s < 0) {
            const QLineF a(poly.at(0), poly.at(1));
            return poly.at(0) + (a.p2() - a.p1()) * (s / a.length());
        } else if (s > total) {
            const QLineF b(poly.at(poly.size() - 2), poly.last());
            return poly.last() + (b.p2() - b.p1()) * ((s - total) / b.length());
        }
        int i = int(std::upper_bound(acc.begin(), acc.end(), s) - acc.begin());
        i = qBound(1, i, int(poly.size()) - 1);
        const double len = acc.at(i) - acc.at(i - 1);
        return QLineF(poly.at(i - 1), poly.at(i)).pointAt(len > 0 ? (s - acc.at(i - 1)) / len : 0);
    };
    const double s0 = opt.startPercent < 0 ? (total - width) / 2.0
                                           : total * opt.startPercent / 100.0;
    // Direction from a short chord either side (smooth on a flattened curve).
    const double h = qMin(0.05, total / 4);
    for (const TextGlyph &g : glyphs) {
        const double mid = g.cluster + g.advance / 2;
        const double s = s0 + mid;
        const QPointF p = pointAt(s);
        const QLineF chord(pointAt(s - h), pointAt(s + h));
        const double ang = std::atan2(chord.dy(), chord.dx());
        const QPointF normal(-std::sin(ang), std::cos(ang));   // left of travel = letters' top
        const QPointF base = p + normal * opt.offset;
        QTransform t;
        t.translate(base.x(), base.y());
        t.rotateRadians(ang);
        t.translate(-mid, 0);
        out.addPath(t.map(g.path));
        if (places)
            places->append({s0 + g.cluster, t.map(QPointF(g.cluster, 0)), qRadiansToDegrees(ang)});
    }
    return out;
}

QVector<Element> textOnPathElements(const Element &text, const Element &path,
                                    const TextOnPathOptions &opt)
{
    if (text.geometryType != QLatin1String("text"))
        return {};
    TextOnPathOptions o2 = opt;
    const PathModel guide = Element::pathModel(path);
    if (o2.closed < 0 && !guide.isEmpty())
        o2.closed = guide.subs.first().closed ? 1 : 0;
    const QPainterPath placed = textOnPathOutline(text.raw, path.painterPath, o2);
    if (placed.isEmpty())
        return {};
    // A text whose outlines are already where they belong (identity
    // transform, no arc), turned into paths the same way Convert to path
    // does, so ids, groups, layer and tabs follow the same rules.
    QJsonArray contours;
    const double k = 20.0;   // flatten in a scaled space: ~0.025 mm tolerance
    const bool stroke = Element::isStrokeText(text.raw);   // single-line strokes: 2 points do
    for (const QPolygonF &poly : placed.toSubpathPolygons(QTransform::fromScale(k, k))) {
        QJsonArray c;
        for (const QPointF &q : poly)
            c.append(QJsonArray{q.x() / k, q.y() / k});
        if (c.size() >= (stroke ? 2 : 3))
            contours.append(c);
    }
    QJsonObject o = text.raw;
    o.insert("rendered", contours);
    o.insert("arc_enabled", false);
    o.insert("transform", QJsonArray{1, 0, 0, 0, 1, 0, 0, 0, 1});
    return Element::toPaths(Element::fromJson(o));
}

} // namespace vec
} // namespace c2d
