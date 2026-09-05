#include "element.h"
#include <QFont>
#include <QFontMetricsF>
#include <QLineF>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointF>
#include <QPolygonF>
#include <QTransform>
#include <QUuid>
#include <QtMath>

namespace c2d {

static QPointF pt(const QJsonArray &a) {
    return QPointF(a.at(0).toDouble(), a.at(1).toDouble());
}

static QJsonArray xy(const QPointF &p) {
    return QJsonArray{p.x(), p.y()};
}

static QJsonArray xyList(const QVector<QPointF> &pts) {
    QJsonArray a;
    for (const QPointF &p : pts)
        a.append(xy(p));
    return a;
}

// The bezier circle constant CC uses (cp offset = kappa * radius).
static const double kKappa = 0.5522847498307936;

// Point types (per anchor): 0 = first, 1 = line segment, 3 = cubic bezier
// segment, 4 = closing point. cp1[i]/cp2[i] are the two control points for the
// segment *arriving at* anchor i. Closed shapes (circle/rect/polygon) store
// coordinates relative to `center`; a `path` stores absolute coordinates with
// position == [0,0]. Either way, absolute = position + point.
QPainterPath Element::buildPointModel(const QJsonObject &obj)
{
    QPainterPath path;
    const QJsonArray points = obj.value("points").toArray();
    const QJsonArray cp1    = obj.value("cp1").toArray();
    const QJsonArray cp2    = obj.value("cp2").toArray();
    const QJsonArray ptype  = obj.value("point_type").toArray();
    const QJsonArray posA   = obj.value("position").toArray();
    const QPointF origin    = posA.size() == 2 ? pt(posA) : QPointF(0, 0);

    if (points.isEmpty())
        return path;

    for (int i = 0; i < points.size(); ++i) {
        const int t = ptype.at(i).toInt();
        const QPointF p = origin + pt(points.at(i).toArray());
        if (t == 0) {
            path.moveTo(p);
        } else if (t == 4) {
            path.closeSubpath();
        } else if (t == 3) {
            const QPointF c1 = origin + pt(cp1.at(i).toArray());
            const QPointF c2 = origin + pt(cp2.at(i).toArray());
            path.cubicTo(c1, c2, p);
        } else { // t == 1 (line) and any fallback
            path.lineTo(p);
        }
    }
    return path;
}

// Text ships pre-flattened glyph outlines in `rendered` (array of contours,
// each an array of [x,y] points in local space), positioned by a row-major 3x3
// `transform` (translation in elements 6,7). No font machinery needed.
QPainterPath Element::buildText(const QJsonObject &obj)
{
    QPainterPath path;
    QJsonArray rendered = obj.value("rendered").toArray();
    if (rendered.isEmpty() && !obj.value("text").toString().isEmpty()) {
        // No outlines shipped (hand-written JSON): lay the text out ourselves,
        // honouring the arc_* keys.
        QJsonObject tmp = obj;
        renderText(tmp);
        rendered = tmp.value("rendered").toArray();
    }
    const QJsonArray t = obj.value("transform").toArray();

    QTransform xf;
    if (t.size() == 9) {
        xf = QTransform(t.at(0).toDouble(), t.at(1).toDouble(),
                        t.at(3).toDouble(), t.at(4).toDouble(),
                        t.at(6).toDouble(), t.at(7).toDouble());
    }

    for (const QJsonValue &contourV : rendered) {
        const QJsonArray contour = contourV.toArray();
        for (int i = 0; i < contour.size(); ++i) {
            const QPointF p = xf.map(pt(contour.at(i).toArray()));
            if (i == 0)
                path.moveTo(p);
            else
                path.lineTo(p);
        }
        path.closeSubpath();
    }
    return path;
}

Element Element::fromJson(const QJsonObject &obj)
{
    Element e;
    e.raw = obj;
    e.id = obj.value("id").toString();
    e.geometryType = obj.value("geometryType").toString();

    if (e.geometryType == "text") {
        e.behavior = Text;
        e.painterPath = buildText(obj);
    } else {
        e.behavior = static_cast<Behavior>(obj.value("behavior").toInt());
        e.painterPath = buildPointModel(obj);
    }
    return e;
}

// Shared boilerplate for the closed-shape factories: keys every CC element
// carries, with coordinates relative to `center` and position == center.
static QJsonObject shapeCommon(const QString &geometryType, int behavior,
                               QPointF center, const QJsonObject &layer)
{
    QJsonObject o;
    o.insert("behavior", behavior);
    o.insert("center", xy(center));
    o.insert("geometryType", geometryType);
    o.insert("group_id", QJsonArray());
    o.insert("id", QUuid::createUuid().toString());   // "{...}" braces, CC style
    o.insert("layer", layer);
    o.insert("position", xy(center));
    o.insert("tabs", QJsonArray());
    return o;
}

// Line-segment shapes (rectangle, polygon): cp1[i] = points[i-1], cp2[i] =
// points[i] (row 0 uses points[0] for both) — exactly what CC writes.
static void fillLinearRows(QJsonObject &o, const QVector<QPointF> &rows)
{
    QVector<QPointF> cp1, cp2;
    QJsonArray ptype, smooth;
    const int n = rows.size();
    for (int i = 0; i < n; ++i) {
        cp1.append(rows.at(qMax(0, i - 1)));
        cp2.append(rows.at(i));
        ptype.append(i == 0 ? 0 : (i == n - 1 ? 4 : 1));
        smooth.append((i == 0 || i == n - 1) ? 1 : 0);
    }
    o.insert("points", xyList(rows));
    o.insert("cp1", xyList(cp1));
    o.insert("cp2", xyList(cp2));
    o.insert("point_type", ptype);
    o.insert("smooth", smooth);
}

Element Element::makeCircle(QPointF center, double r, const QJsonObject &layer)
{
    QJsonObject o = shapeCommon(QStringLiteral("circle"), 3, center, layer);
    o.insert("radius", r);
    const double k = kKappa * r;
    // Anchors start at (-r,0), CCW through the four quadrant points, then a
    // (0,0) close row; cp rows follow CC's specimen exactly.
    o.insert("points", xyList({{-r, 0}, {0, r}, {r, 0}, {0, -r}, {-r, 0}, {0, 0}}));
    o.insert("cp1",    xyList({{0, 0}, {-r, k}, {k, r}, {r, -k}, {-k, -r}, {0, 0}}));
    o.insert("cp2",    xyList({{0, 0}, {-k, r}, {r, k}, {k, -r}, {-r, -k}, {0, 0}}));
    o.insert("point_type", QJsonArray{0, 3, 3, 3, 3, 4});
    o.insert("smooth",     QJsonArray{1, 1, 1, 1, 1, 1});
    return fromJson(o);
}

Element Element::makeRectangle(QPointF center, double w, double h,
                               const QJsonObject &layer)
{
    QJsonObject o = shapeCommon(QStringLiteral("rectangle"), 1, center, layer);
    o.insert("width", w);
    o.insert("height", h);
    o.insert("corner_type", 0);          // square corners
    o.insert("radius", 6.35);            // CC's default corner radius (unused at type 0)
    const double x = w / 2.0, y = h / 2.0;
    // 4 corners CW from top-right, back to start, then the close row.
    fillLinearRows(o, {{x, y}, {x, -y}, {-x, -y}, {-x, y}, {x, y}, {x, y}});
    return fromJson(o);
}

Element Element::makePolygon(QPointF center, double r, int numSides,
                             const QJsonObject &layer, double rotationDeg)
{
    numSides = qMax(3, numSides);
    QJsonObject o = shapeCommon(QStringLiteral("regular_polygon"), 2, center, layer);
    o.insert("radius", r);
    o.insert("num_sides", numSides);
    o.insert("rotation", rotationDeg);
    const double rot = qDegreesToRadians(rotationDeg);
    QVector<QPointF> rows;
    for (int i = 0; i < numSides; ++i) {
        const double a = 2.0 * M_PI * i / numSides + rot;   // vertex 0 at rotation, CCW
        rows.append(QPointF(r * qCos(a), r * qSin(a)));
    }
    rows.append(rows.first());   // line back to start
    rows.append(rows.first());   // close row
    fillLinearRows(o, rows);
    return fromJson(o);
}

Element Element::makePath(const QVector<QPointF> &vertices, bool closed,
                          const QJsonObject &layer)
{
    // Paths carry no center/radius keys: absolute coords, position [0,0].
    QJsonObject o;
    o.insert("behavior", 0);
    o.insert("geometryType", QStringLiteral("path"));
    o.insert("group_id", QJsonArray());
    o.insert("id", QUuid::createUuid().toString());
    o.insert("layer", layer);
    o.insert("position", xy(QPointF(0, 0)));
    o.insert("tabs", QJsonArray());

    QVector<QPointF> rows = vertices;
    if (closed && !vertices.isEmpty()) {
        rows.append(vertices.first());   // line back to start
        rows.append(vertices.first());   // close row
    }
    QVector<QPointF> cp1, cp2;
    QJsonArray ptype, smooth;
    const int n = rows.size();
    for (int i = 0; i < n; ++i) {
        cp1.append(rows.at(qMax(0, i - 1)));
        cp2.append(rows.at(i));
        ptype.append(i == 0 ? 0 : (closed && i == n - 1 ? 4 : 1));
        smooth.append(closed && i == n - 1 ? 1 : 0);
    }
    o.insert("points", xyList(rows));
    o.insert("cp1", xyList(cp1));
    o.insert("cp2", xyList(cp2));
    o.insert("point_type", ptype);
    o.insert("smooth", smooth);
    return fromJson(o);
}

Element Element::makeText(const QString &text, QPointF pos, double heightMm,
                          const QString &family, const QJsonObject &layer)
{
    QFont font(family);
    font.setPointSize(100);              // CC stores qtfont at 100 pt
    QJsonObject o;
    o.insert("alignment", 0);
    o.insert("arc_angle_offset", 0);
    o.insert("arc_center", xy(QPointF(0, -25.4)));   // fixed up below
    o.insert("arc_enabled", false);
    o.insert("arc_radius", 25.4);
    o.insert("arc_text_on_bottom", false);
    o.insert("font", family + QStringLiteral("-Regular"));
    o.insert("font_height", heightMm);
    o.insert("geometryType", QStringLiteral("text"));
    o.insert("group_id", QJsonArray());
    o.insert("id", QUuid::createUuid().toString());
    o.insert("layer", layer);
    o.insert("position", xy(QPointF(0, 0)));
    o.insert("qtfont", font.toString());
    o.insert("spacing", 1);
    o.insert("tabs", QJsonArray());
    o.insert("text", text);
    // Row-major 3x3, translation in [6],[7] — places the baseline-left at pos.
    o.insert("transform", QJsonArray{1, 0, 0, 0, 1, 0, pos.x(), pos.y(), 1});
    renderText(o);
    // Default arc centre: below the middle of the straight baseline by the
    // radius, so enabling the arc lifts the text into a bow around its old
    // baseline midpoint.
    const double w = o.value("straight_width").toDouble();
    o.remove("straight_width");
    o.insert("arc_center", xy(QPointF(w / 2.0, -o.value("arc_radius").toDouble())));
    return fromJson(o);
}

// ---- text rendering --------------------------------------------------------

QFont Element::textFont(const QJsonObject &o)
{
    QFont f;
    const QString qt = o.value("qtfont").toString();
    if (qt.isEmpty() || !f.fromString(qt)) {
        // "Helvetica-Regular" / "Arial-Bold" style face names.
        QString face = o.value("font").toString();
        const int dash = face.lastIndexOf(QChar('-'));
        QString style;
        if (dash > 0) { style = face.mid(dash + 1); face = face.left(dash); }
        f = QFont(face.isEmpty() ? QStringLiteral("Helvetica") : face);
        f.setBold(style.contains(QLatin1String("Bold"), Qt::CaseInsensitive));
        f.setItalic(style.contains(QLatin1String("Italic"), Qt::CaseInsensitive)
                    || style.contains(QLatin1String("Oblique"), Qt::CaseInsensitive));
    }
    f.setPointSize(100);
    return f;
}

static QString faceName(const QFont &f)
{
    QString style = f.bold() ? (f.italic() ? QStringLiteral("BoldItalic") : QStringLiteral("Bold"))
                             : (f.italic() ? QStringLiteral("Italic") : QStringLiteral("Regular"));
    return f.family() + QChar('-') + style;
}

// Flatten a path into CC's contour list. The flattening runs in a 20x scaled
// space so the ~0.5-unit bezier tolerance becomes 0.025 mm.
static QJsonArray contoursOf(const QPainterPath &mm)
{
    QTransform up;
    up.scale(20, 20);
    QJsonArray rendered;
    const auto polys = up.map(mm).toSubpathPolygons();
    for (const QPolygonF &poly : polys) {
        if (poly.size() < 3)
            continue;
        QJsonArray contour;
        for (int i = 0; i < poly.size(); ++i) {
            // toSubpathPolygons repeats the first point at the end; CC does not.
            if (i == poly.size() - 1 && poly.at(i) == poly.first())
                break;
            contour.append(xy(poly.at(i) / 20.0));
        }
        rendered.append(contour);
    }
    return rendered;
}

// Rebuild `rendered` from the element's own keys. Layout is in local Y-up
// space with the straight baseline on y = 0 starting at x = 0 (what CC
// writes: the first glyph's left bearing is the smallest x). font_height is
// the font's ascent in mm (CC's "P" tops out at 0.93 * font_height for
// Helvetica, i.e. the Windows ascent). Arc mode places each glyph with its
// advance midpoint on the circle |q - arc_center| = arc_radius, baseline
// tangent to it, the whole string centred on the top point (90 deg) or the
// bottom point (270 deg, glyphs upright, extending inwards) rotated by
// arc_angle_offset degrees (counter-clockwise positive). Glyph advances are
// measured along the arc, so the letters keep their straight spacing.
void Element::renderText(QJsonObject &o)
{
    const QString text = o.value("text").toString();
    const double h = o.value("font_height").toDouble(10.0);
    const double spacing = o.contains("spacing") ? o.value("spacing").toDouble(1.0) : 1.0;
    QFont font = textFont(o);
    const int px = 256;                    // render size; scaled to mm below
    font.setPixelSize(px);
    font.setStyleStrategy(QFont::PreferOutline);
    const QFontMetricsF fm(font);
    const double ascent = fm.ascent() > 0 ? fm.ascent() : px * 0.9;
    const double scale = h / ascent;
    QTransform toMm;
    toMm.scale(scale, -scale);            // Qt text is Y-down; CC is Y-up

    // Per-glyph outlines and advances (mm, Y-up, pen at the origin).
    struct Glyph { QPainterPath path; double advance; };
    QVector<Glyph> glyphs;
    double width = 0;
    for (const QChar &ch : text) {
        QPainterPath gp;
        gp.addText(0, 0, font, QString(ch));
        const double adv = fm.horizontalAdvance(ch) * scale * spacing;
        glyphs.append({toMm.map(gp), adv});
        width += adv;
    }
    o.insert("straight_width", width);

    QPainterPath out;
    const bool arc = o.value("arc_enabled").toBool();
    const double R = o.value("arc_radius").toDouble();
    if (!arc || R <= 1e-6) {
        double x = 0;
        for (const Glyph &g : glyphs) {
            QTransform t;
            t.translate(x, 0);
            out.addPath(t.map(g.path));
            x += g.advance;
        }
    } else {
        const QJsonArray ca = o.value("arc_center").toArray();
        const QPointF C = ca.size() == 2 ? pt(ca) : QPointF(width / 2, -R);
        const bool bottom = o.value("arc_text_on_bottom").toBool();
        const double offset = qDegreesToRadians(o.value("arc_angle_offset").toDouble());
        const double mid = (bottom ? 1.5 * M_PI : 0.5 * M_PI) + offset;
        double x = 0;
        for (const Glyph &g : glyphs) {
            const double c = x + g.advance / 2;        // glyph midpoint on the baseline
            // Top text reads clockwise (angle decreases with x); bottom text
            // reads counter-clockwise so it stays upright for the reader.
            const double theta = bottom ? mid - (width / 2 - c) / R
                                        : mid + (width / 2 - c) / R;
            QTransform t;
            t.translate(C.x() + R * qCos(theta), C.y() + R * qSin(theta));
            t.rotateRadians(bottom ? theta + 0.5 * M_PI : theta - 0.5 * M_PI);
            t.translate(-g.advance / 2, 0);   // glyph outlines start at their own origin
            out.addPath(t.map(g.path));
            x += g.advance;
        }
    }
    o.insert("rendered", contoursOf(out));
}

Element Element::regenText(const Element &src, const QJsonObject &changes)
{
    QJsonObject o = src.raw;
    QFont f = textFont(o);
    bool fontChanged = false;
    for (auto it = changes.begin(); it != changes.end(); ++it) {
        const QString k = it.key();
        if (k == QLatin1String("family")) { f.setFamily(it.value().toString()); fontChanged = true; }
        else if (k == QLatin1String("bold")) { f.setBold(it.value().toBool()); fontChanged = true; }
        else if (k == QLatin1String("italic")) { f.setItalic(it.value().toBool()); fontChanged = true; }
        else o.insert(k, it.value());
    }
    if (fontChanged) {
        f.setPointSize(100);
        o.insert("qtfont", f.toString());
        o.insert("font", faceName(f));
    }
    renderText(o);
    o.remove("straight_width");
    return fromJson(o);
}

// ---- node model --------------------------------------------------------------

static bool samePt(const QPointF &a, const QPointF &b)
{
    return qAbs(a.x() - b.x()) < 1e-9 && qAbs(a.y() - b.y()) < 1e-9;
}

bool PathNode::hasIn() const { return !samePt(in, p); }
bool PathNode::hasOut() const { return !samePt(out, p); }

int SubPath::segmentCount() const
{
    const int n = nodes.size();
    return n < 2 ? 0 : (closed ? n : n - 1);
}

void SubPath::segment(int i, const PathNode **a, const PathNode **b) const
{
    *a = &nodes.at(i);
    *b = &nodes.at((i + 1) % nodes.size());
}

static void segmentTo(QPainterPath &path, const PathNode &a, const PathNode &b)
{
    if (!a.hasOut() && !b.hasIn())
        path.lineTo(b.p);
    else
        path.cubicTo(a.out, b.in, b.p);
}

static QPointF lerp(const QPointF &a, const QPointF &b, double t) { return a + (b - a) * t; }

QPointF SubPath::pointAt(int seg, double t) const
{
    const PathNode *a, *b;
    segment(seg, &a, &b);
    if (!a->hasOut() && !b->hasIn())
        return lerp(a->p, b->p, t);
    const QPointF p01 = lerp(a->p, a->out, t), p12 = lerp(a->out, b->in, t),
                  p23 = lerp(b->in, b->p, t);
    return lerp(lerp(p01, p12, t), lerp(p12, p23, t), t);
}

double SubPath::closest(const QPointF &pt, int *seg, double *t) const
{
    double best = 1e300;
    *seg = -1; *t = 0;
    for (int s = 0; s < segmentCount(); ++s) {
        const int N = 32;
        for (int k = 0; k <= N; ++k) {
            const double tt = double(k) / N;
            const double d = QLineF(pt, pointAt(s, tt)).length();
            if (d < best) { best = d; *seg = s; *t = tt; }
        }
    }
    if (*seg >= 0) {   // refine around the sampled minimum
        double lo = qMax(0.0, *t - 1.0 / 32), hi = qMin(1.0, *t + 1.0 / 32);
        for (int it = 0; it < 24; ++it) {
            const double m1 = lo + (hi - lo) / 3, m2 = hi - (hi - lo) / 3;
            if (QLineF(pt, pointAt(*seg, m1)).length() < QLineF(pt, pointAt(*seg, m2)).length())
                hi = m2;
            else
                lo = m1;
        }
        *t = (lo + hi) / 2;
        best = QLineF(pt, pointAt(*seg, *t)).length();
    }
    return best;
}

int SubPath::insertNode(int seg, double t)
{
    if (seg < 0 || seg >= segmentCount())
        return -1;
    const int ia = seg, ib = (seg + 1) % nodes.size();
    PathNode &a = nodes[ia];
    PathNode &b = nodes[ib];
    PathNode n;
    if (!a.hasOut() && !b.hasIn()) {
        n.p = n.in = n.out = lerp(a.p, b.p, t);
        n.kind = PathNode::Corner;
    } else {
        // de Casteljau split: the two halves trace the original curve.
        const QPointF p01 = lerp(a.p, a.out, t), p12 = lerp(a.out, b.in, t),
                      p23 = lerp(b.in, b.p, t);
        const QPointF p012 = lerp(p01, p12, t), p123 = lerp(p12, p23, t);
        n.p = lerp(p012, p123, t);
        n.in = p012;
        n.out = p123;
        n.kind = PathNode::Smooth;
        a.out = p01;
        b.in = p23;
    }
    nodes.insert(ia + 1, n);
    return ia + 1;
}

bool SubPath::removeNode(int i)
{
    if (i < 0 || i >= nodes.size() || nodes.size() <= 2)
        return false;
    nodes.removeAt(i);
    return true;
}

void SubPath::retract(int i)
{
    PathNode &n = nodes[i];
    n.in = n.out = n.p;
    n.kind = PathNode::Corner;
}

void SubPath::setKind(int i, PathNode::Kind k)
{
    PathNode &n = nodes[i];
    n.kind = k;
    if (k == PathNode::Corner)
        return;
    const int cnt = nodes.size();
    const PathNode *prev = (i > 0) ? &nodes.at(i - 1) : (closed && cnt > 1 ? &nodes.at(cnt - 1) : nullptr);
    const PathNode *next = (i + 1 < cnt) ? &nodes.at(i + 1) : (closed && cnt > 1 ? &nodes.at(0) : nullptr);
    double lin = QLineF(n.p, n.in).length(), lout = QLineF(n.p, n.out).length();
    if (lin < 1e-9 && prev)  lin = QLineF(n.p, prev->p).length() / 3.0;
    if (lout < 1e-9 && next) lout = QLineF(n.p, next->p).length() / 3.0;
    QPointF d = n.out - n.in;                        // current tangent
    if (QLineF(QPointF(), d).length() < 1e-9) {
        if (prev && next)      d = next->p - prev->p;
        else if (next)         d = next->p - n.p;
        else if (prev)         d = n.p - prev->p;
    }
    const double len = QLineF(QPointF(), d).length();
    if (len < 1e-9)
        return;
    d /= len;
    if (k == PathNode::Symmetric)
        lin = lout = (lin + lout) / 2.0;
    n.in = prev ? n.p - d * lin : n.p;
    n.out = next ? n.p + d * lout : n.p;
}

void SubPath::moveHandle(int i, bool outHandle, const QPointF &to, bool breakSymmetry)
{
    PathNode &n = nodes[i];
    QPointF &moved = outHandle ? n.out : n.in;
    QPointF &other = outHandle ? n.in : n.out;
    moved = to;
    if (breakSymmetry) { n.kind = PathNode::Corner; return; }
    if (n.kind == PathNode::Corner)
        return;
    const QPointF v = to - n.p;
    const double len = QLineF(QPointF(), v).length();
    if (len < 1e-9)
        return;
    const double otherLen = n.kind == PathNode::Symmetric ? len : QLineF(n.p, other).length();
    if (otherLen < 1e-9 && n.kind == PathNode::Smooth)
        return;                                       // nothing to align
    other = n.p - v / len * otherLen;
}

QPainterPath PathModel::painterPath() const
{
    QPainterPath path;
    for (const SubPath &s : subs) {
        if (s.nodes.isEmpty())
            continue;
        path.moveTo(s.nodes.first().p);
        for (int i = 1; i < s.nodes.size(); ++i)
            segmentTo(path, s.nodes.at(i - 1), s.nodes.at(i));
        if (s.closed && s.nodes.size() > 1) {
            segmentTo(path, s.nodes.last(), s.nodes.first());
            path.closeSubpath();
        }
    }
    return path;
}

PathModel Element::pathModel(const Element &e)
{
    PathModel m;
    if (e.geometryType == QLatin1String("text"))
        return m;
    const QJsonObject &o = e.raw;
    const QJsonArray points = o.value("points").toArray();
    const QJsonArray cp1 = o.value("cp1").toArray();
    const QJsonArray cp2 = o.value("cp2").toArray();
    const QJsonArray ptype = o.value("point_type").toArray();
    const QJsonArray smooth = o.value("smooth").toArray();
    const QJsonArray posA = o.value("position").toArray();
    const QPointF origin = posA.size() == 2 ? pt(posA) : QPointF(0, 0);

    SubPath cur;
    auto flush = [&]() {
        if (cur.nodes.isEmpty())
            return;
        if (cur.closed && cur.nodes.size() >= 2
            && samePt(cur.nodes.last().p, cur.nodes.first().p)) {
            // The return-to-start row carries the closing segment's handles.
            cur.nodes.first().in = cur.nodes.last().in;
            cur.nodes.removeLast();
        } else if (cur.closed && !cur.nodes.isEmpty()) {
            cur.nodes.last().out = cur.nodes.last().p;   // straight close
            cur.nodes.first().in = cur.nodes.first().p;
        }
        for (PathNode &n : cur.nodes) {
            if (!n.hasIn() && !n.hasOut())
                n.kind = PathNode::Corner;
            else if (n.kind != PathNode::Corner
                     && samePt(n.in - n.p, -(n.out - n.p)))
                n.kind = PathNode::Symmetric;
        }
        m.subs.append(cur);
        cur = SubPath();
    };

    for (int i = 0; i < points.size(); ++i) {
        const int t = ptype.at(i).toInt();
        const QPointF p = origin + pt(points.at(i).toArray());
        const bool sm = smooth.size() > i && smooth.at(i).toInt() != 0;
        if (t == 0) {
            flush();
            PathNode n;
            n.p = n.in = n.out = p;
            n.kind = sm ? PathNode::Smooth : PathNode::Corner;
            cur.nodes.append(n);
        } else if (t == 4) {
            cur.closed = true;
        } else {
            PathNode n;
            n.p = n.out = p;
            n.in = (t == 3 && cp2.size() > i) ? origin + pt(cp2.at(i).toArray()) : p;
            n.kind = sm ? PathNode::Smooth : PathNode::Corner;
            if (!cur.nodes.isEmpty()) {
                cur.nodes.last().out = (t == 3 && cp1.size() > i)
                                           ? origin + pt(cp1.at(i).toArray())
                                           : cur.nodes.last().p;
            } else {
                n.in = p;   // stray row without a first point: start here
            }
            cur.nodes.append(n);
        }
    }
    flush();
    return m;
}

// Encode the node model into CC's parallel arrays (absolute coordinates,
// position [0,0]). See pathModel() for the row layout it mirrors.
static void fillBezierRows(QJsonObject &o, const PathModel &model)
{
    QVector<QPointF> rows, cp1, cp2;
    QJsonArray ptype, smooth;
    auto row = [&](const QPointF &p, const QPointF &c1, const QPointF &c2, int t, int sm) {
        rows.append(p); cp1.append(c1); cp2.append(c2); ptype.append(t); smooth.append(sm);
    };
    auto segRow = [&](const PathNode &a, const PathNode &b, int sm) {
        if (!a.hasOut() && !b.hasIn())
            row(b.p, a.p, b.p, 1, sm);
        else
            row(b.p, a.out, b.in, 3, sm);
    };
    for (const SubPath &s : model.subs) {
        const int n = s.nodes.size();
        if (n == 0)
            continue;
        const PathNode &first = s.nodes.first();
        row(first.p, first.p, first.p, 0, first.kind != PathNode::Corner ? 1 : 0);
        for (int i = 1; i < n; ++i)
            segRow(s.nodes.at(i - 1), s.nodes.at(i), s.nodes.at(i).kind != PathNode::Corner ? 1 : 0);
        if (s.closed && n >= 2) {
            segRow(s.nodes.last(), first, first.kind != PathNode::Corner ? 1 : 0);
            row(first.p, first.p, first.p, 4, 1);
        }
    }
    o.insert("points", xyList(rows));
    o.insert("cp1", xyList(cp1));
    o.insert("cp2", xyList(cp2));
    o.insert("point_type", ptype);
    o.insert("smooth", smooth);
}

Element Element::makeBezierPath(const PathModel &model, const QJsonObject &layer)
{
    QJsonObject o;
    o.insert("behavior", 0);
    o.insert("geometryType", QStringLiteral("path"));
    o.insert("group_id", QJsonArray());
    o.insert("id", QUuid::createUuid().toString());
    o.insert("layer", layer);
    o.insert("position", xy(QPointF(0, 0)));
    o.insert("tabs", QJsonArray());
    fillBezierRows(o, model);
    return fromJson(o);
}

Element Element::withPathModel(const Element &src, const PathModel &model)
{
    Element e = makeBezierPath(model, src.raw.value("layer").toObject());
    QJsonObject o = e.raw;
    o.insert("id", src.raw.value("id"));
    o.insert("group_id", src.raw.value("group_id"));
    o.insert("tabs", src.raw.value("tabs"));
    return fromJson(o);
}

QVector<Element> Element::toPaths(const Element &src)
{
    QVector<Element> out;
    if (src.geometryType == QLatin1String("path")) {
        out.append(src);
        return out;
    }
    if (src.geometryType != QLatin1String("text")) {
        out.append(withPathModel(src, pathModel(src)));
        return out;
    }
    // Text: each rendered contour (already in absolute coordinates on the
    // painter path) becomes a closed polygon path; the first keeps the id.
    const auto polys = src.painterPath.toSubpathPolygons();
    bool first = true;
    for (const QPolygonF &poly : polys) {
        PathModel m;
        SubPath s;
        s.closed = true;
        for (int i = 0; i < poly.size(); ++i) {
            if (i == poly.size() - 1 && samePt(poly.at(i), poly.first()))
                break;
            PathNode n;
            n.p = n.in = n.out = poly.at(i);
            s.nodes.append(n);
        }
        if (s.nodes.size() < 3)
            continue;
        m.subs.append(s);
        Element e = makeBezierPath(m, src.raw.value("layer").toObject());
        if (first) {
            QJsonObject o = e.raw;
            o.insert("id", src.raw.value("id"));
            o.insert("group_id", src.raw.value("group_id"));
            o.insert("tabs", src.raw.value("tabs"));
            e = fromJson(o);
            first = false;
        }
        out.append(e);
    }
    return out;
}

Element Element::regen(const Element &src, const QHash<QString, double> &p)
{
    const QJsonObject &r = src.raw;
    const QJsonArray c = r.value("center").toArray();
    const double cx = p.value("cx", c.size() == 2 ? c.at(0).toDouble() : 0);
    const double cy = p.value("cy", c.size() == 2 ? c.at(1).toDouble() : 0);
    const QJsonObject layer = r.value("layer").toObject();

    Element e;
    if (src.geometryType == QLatin1String("circle")) {
        e = makeCircle({cx, cy}, p.value("radius", r.value("radius").toDouble()), layer);
    } else if (src.geometryType == QLatin1String("rectangle")) {
        e = makeRectangle({cx, cy},
                          p.value("width", r.value("width").toDouble()),
                          p.value("height", r.value("height").toDouble()), layer);
    } else if (src.geometryType == QLatin1String("regular_polygon")) {
        e = makePolygon({cx, cy},
                        p.value("radius", r.value("radius").toDouble()),
                        int(p.value("num_sides", r.value("num_sides").toDouble())), layer,
                        p.value("rotation", r.value("rotation").toDouble()));
    } else {
        return src;   // path/text: no parametric regen
    }

    // Keep the original identity so toolpath references and undo stay stable.
    QJsonObject o = e.raw;
    o.insert("id", r.value("id"));
    o.insert("group_id", r.value("group_id"));
    o.insert("tabs", r.value("tabs"));
    return fromJson(o);
}

void Element::translate(double dx, double dy)
{
    if (geometryType == QLatin1String("text")) {
        QJsonArray t = raw.value("transform").toArray();
        if (t.size() == 9) {
            t[6] = t.at(6).toDouble() + dx;
            t[7] = t.at(7).toDouble() + dy;
            raw.insert("transform", t);
        }
    } else {
        // absolute = position + point, so shifting position moves everything;
        // center mirrors position on the closed shapes.
        for (const char *key : {"position", "center"}) {
            if (raw.contains(QLatin1String(key))) {
                QPointF p = pt(raw.value(QLatin1String(key)).toArray());
                raw.insert(QLatin1String(key), xy(p + QPointF(dx, dy)));
            }
        }
    }
    *this = fromJson(raw);
}

QByteArray Element::toJson() const
{
    return QJsonDocument(raw).toJson(QJsonDocument::Indented);
}

} // namespace c2d
