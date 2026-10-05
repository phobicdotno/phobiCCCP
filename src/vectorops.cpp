#include "vectorops.h"
#include <QHash>
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
    if (e.geometryType != QLatin1String("path"))
        return true;
    for (const QJsonValue &v : e.raw.value("point_type").toArray())
        if (v.toInt() == 4)
            return true;
    return false;
}

#ifdef HAVE_CLIPPER2
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

QVector<QPolygonF> offsetRings(const QVector<QPainterPath> &closed,
                               const QVector<QPainterPath> &open, double delta, double tol)
{
    const double arcTol = tol * kScale;
    C2::Paths64 out;
    if (!closed.isEmpty()) {
        const C2::Paths64 region = regionOf(closed, tol);
        if (!region.empty())
            out = C2::InflatePaths(region, delta * kScale, C2::JoinType::Round,
                                   C2::EndType::Polygon, 2.0, arcTol);
    }
    if (!open.isEmpty() && !qFuzzyIsNull(delta)) {
        C2::Paths64 lines;
        for (const QPainterPath &pp : open)
            for (const QPolygonF &poly : finePolygons(pp, tol))
                if (poly.size() > 1)
                    lines.push_back(toPath64(poly));
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
    QRectF all;
    for (const QRectF &b : boxes)
        all = all.isNull() ? b : all.united(b);
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
        const QTransform n = xf * t;
        o.insert("transform", QJsonArray{n.m11(), n.m12(), 0, n.m21(), n.m22(), 0,
                                         n.dx(), n.dy(), 1});
        return Element::fromJson(o);
    }

    const bool shape = e.geometryType == QLatin1String("circle")
                    || e.geometryType == QLatin1String("rectangle")
                    || e.geometryType == QLatin1String("regular_polygon");
    if (shape && e.raw.contains("center")) {
        const QPointF c = jsonPt(e.raw.value("center"));
        const QPointF nc = t.map(c);
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

} // namespace vec
} // namespace c2d
