#include "element.h"
#include <QFont>
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
    const QJsonArray rendered = obj.value("rendered").toArray();
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
    // Render glyph outlines in local Y-up space with the baseline at y = 0,
    // sized so the ascent equals heightMm, then flatten to CC's contour list.
    const int px = qMax(4, int(qRound(heightMm * 4)));   // oversample 4x for smooth curves
    QFont font(family);
    font.setPixelSize(px);
    QPainterPath gp;
    gp.addText(0, 0, font, text);
    const double scale = heightMm / double(px);
    QTransform local;
    local.scale(scale, -scale);          // Qt text is Y-down; CC is Y-up
    const QPainterPath up = local.map(gp);

    QJsonArray rendered;
    const auto polys = up.toSubpathPolygons();
    for (const QPolygonF &poly : polys) {
        QJsonArray contour;
        for (const QPointF &v : poly)
            contour.append(xy(v));
        rendered.append(contour);
    }

    QJsonObject o;
    o.insert("alignment", 0);
    o.insert("arc_angle_offset", 0);
    o.insert("arc_center", xy(up.boundingRect().center() + pos));
    o.insert("arc_enabled", false);
    o.insert("arc_radius", 25.4);
    o.insert("arc_text_on_bottom", false);
    o.insert("font", family);
    o.insert("font_height", heightMm);
    o.insert("geometryType", QStringLiteral("text"));
    o.insert("group_id", QJsonArray());
    o.insert("id", QUuid::createUuid().toString());
    o.insert("layer", layer);
    o.insert("position", xy(QPointF(0, 0)));
    o.insert("qtfont", font.toString());
    o.insert("rendered", rendered);
    o.insert("spacing", 1);
    o.insert("tabs", QJsonArray());
    o.insert("text", text);
    // Row-major 3x3, translation in [6],[7] — places the baseline-left at pos.
    o.insert("transform", QJsonArray{1, 0, 0, 0, 1, 0, pos.x(), pos.y(), 1});
    return fromJson(o);
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
