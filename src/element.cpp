#include "element.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointF>
#include <QTransform>
#include <QUuid>

namespace c2d {

static QPointF pt(const QJsonArray &a) {
    return QPointF(a.at(0).toDouble(), a.at(1).toDouble());
}

static QJsonArray jpt(double x, double y) {
    return QJsonArray{x, y};
}

static QJsonArray jpt(QPointF p) {
    return QJsonArray{p.x(), p.y()};
}

// Shift every [x,y] pair in a points/cp1/cp2 array.
static QJsonArray shiftedPairs(const QJsonArray &arr, double dx, double dy)
{
    QJsonArray out;
    for (const QJsonValue &v : arr) {
        const QJsonArray a = v.toArray();
        out.append(jpt(a.at(0).toDouble() + dx, a.at(1).toDouble() + dy));
    }
    return out;
}

// Scale every [x,y] pair about `about`.
static QJsonArray scaledPairs(const QJsonArray &arr, double f, QPointF about)
{
    QJsonArray out;
    for (const QJsonValue &v : arr) {
        const QJsonArray a = v.toArray();
        out.append(jpt(about.x() + (a.at(0).toDouble() - about.x()) * f,
                       about.y() + (a.at(1).toDouble() - about.y()) * f));
    }
    return out;
}

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

QByteArray Element::toJson() const
{
    return QJsonDocument(raw).toJson(QJsonDocument::Indented);
}

void Element::rebuildPath()
{
    painterPath = (behavior == Text) ? buildText(raw) : buildPointModel(raw);
}

void Element::translate(double dx, double dy)
{
    if (behavior == Text) {
        QJsonArray t = raw.value("transform").toArray();
        if (t.size() == 9) {
            t[6] = t.at(6).toDouble() + dx;
            t[7] = t.at(7).toDouble() + dy;
            raw.insert("transform", t);
        }
    } else if (raw.contains("center")) {
        // Closed shape: points are center-relative, so only the anchors move.
        for (const char *k : {"center", "position"}) {
            const QJsonArray a = raw.value(QLatin1String(k)).toArray();
            raw.insert(QLatin1String(k),
                       jpt(a.at(0).toDouble() + dx, a.at(1).toDouble() + dy));
        }
    } else {
        // Path: absolute coordinates, position stays [0,0].
        raw.insert("points", shiftedPairs(raw.value("points").toArray(), dx, dy));
        raw.insert("cp1",    shiftedPairs(raw.value("cp1").toArray(),    dx, dy));
        raw.insert("cp2",    shiftedPairs(raw.value("cp2").toArray(),    dx, dy));
    }
    rebuildPath();
}

void Element::scaleBy(double factor)
{
    if (factor <= 0)
        return;

    if (behavior == Text) {
        // Scale the linear part; translation (elements 6,7) is the anchor.
        QJsonArray t = raw.value("transform").toArray();
        if (t.size() == 9) {
            for (int i : {0, 1, 3, 4})
                t[i] = t.at(i).toDouble() * factor;
            raw.insert("transform", t);
        }
    } else if (raw.contains("center")) {
        // Closed shape: scale the center-relative model about [0,0].
        raw.insert("points", scaledPairs(raw.value("points").toArray(), factor, {}));
        raw.insert("cp1",    scaledPairs(raw.value("cp1").toArray(),    factor, {}));
        raw.insert("cp2",    scaledPairs(raw.value("cp2").toArray(),    factor, {}));
        for (const char *k : {"radius", "width", "height"})
            if (raw.contains(QLatin1String(k)))
                raw.insert(QLatin1String(k), raw.value(QLatin1String(k)).toDouble() * factor);
    } else {
        // Path: absolute coordinates, scale about the bounding-box center.
        const QPointF c = painterPath.boundingRect().center();
        raw.insert("points", scaledPairs(raw.value("points").toArray(), factor, c));
        raw.insert("cp1",    scaledPairs(raw.value("cp1").toArray(),    factor, c));
        raw.insert("cp2",    scaledPairs(raw.value("cp2").toArray(),    factor, c));
    }
    rebuildPath();
}

// Shared scaffolding for new elements: id, layer, group/tab arrays.
static QJsonObject newElementShell(const QString &geometryType, int behavior, QPointF center)
{
    QJsonObject o;
    o.insert("behavior", behavior);
    o.insert("geometryType", geometryType);
    o.insert("id", QUuid::createUuid().toString());   // "{...}" braces, as CC writes
    o.insert("group_id", QJsonArray{});
    o.insert("tabs", QJsonArray{});
    o.insert("center",   jpt(center));
    o.insert("position", jpt(center));
    QJsonObject layer;
    layer.insert("name", QStringLiteral("DEFAULT"));
    layer.insert("uuid", QString());
    layer.insert("red", 0);
    layer.insert("green", 0);
    layer.insert("blue", 0);
    layer.insert("locked", false);
    layer.insert("visible", true);
    o.insert("layer", layer);
    return o;
}

Element Element::makeCircle(QPointF center, double radius)
{
    // Four cubic quadrants, CC's anchor/control layout (see samples): anchors
    // W,N,E,S back to W, kappa control points per arriving segment.
    const double r = radius;
    const double k = 0.5522847498307936 * r;

    QJsonObject o = newElementShell(QStringLiteral("circle"), Circle, center);
    o.insert("radius", r);
    o.insert("points", QJsonArray{jpt(-r, 0), jpt(0, r), jpt(r, 0), jpt(0, -r),
                                  jpt(-r, 0), jpt(0, 0)});
    o.insert("cp1", QJsonArray{jpt(0, 0), jpt(-r, k), jpt(k, r), jpt(r, -k),
                               jpt(-k, -r), jpt(0, 0)});
    o.insert("cp2", QJsonArray{jpt(0, 0), jpt(-k, r), jpt(r, k), jpt(k, -r),
                               jpt(-r, -k), jpt(0, 0)});
    o.insert("point_type", QJsonArray{0, 3, 3, 3, 3, 4});
    o.insert("smooth", QJsonArray{1, 1, 1, 1, 1, 1});
    return fromJson(o);
}

Element Element::makeRectangle(QPointF center, double width, double height)
{
    // Square-corner rectangle: anchors clockwise from NE, line segments whose
    // cp1/cp2 mirror the previous/current anchor (as CC writes them).
    const double w = width / 2.0, h = height / 2.0;
    const QJsonArray pts{jpt(w, h), jpt(w, -h), jpt(-w, -h), jpt(-w, h),
                         jpt(w, h), jpt(w, h)};

    QJsonObject o = newElementShell(QStringLiteral("rectangle"), Rectangle, center);
    o.insert("width", width);
    o.insert("height", height);
    o.insert("corner_type", 0);
    o.insert("radius", 6.35);   // CC's default corner radius; unused for corner_type 0
    o.insert("points", pts);
    // Line segments: cp1 mirrors the previous anchor, cp2 the current one.
    QJsonArray cp1, cp2;
    for (int i = 0; i < pts.size(); ++i) {
        cp1.append(pts.at(i == 0 ? 0 : i - 1));
        cp2.append(pts.at(i));
    }
    o.insert("cp1", cp1);
    o.insert("cp2", cp2);
    o.insert("point_type", QJsonArray{0, 1, 1, 1, 1, 4});
    o.insert("smooth", QJsonArray{0, 0, 0, 0, 0, 1});
    return fromJson(o);
}

} // namespace c2d
