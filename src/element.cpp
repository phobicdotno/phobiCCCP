#include "element.h"
#include <QJsonArray>
#include <QPointF>
#include <QTransform>

namespace c2d {

static QPointF pt(const QJsonArray &a) {
    return QPointF(a.at(0).toDouble(), a.at(1).toDouble());
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

} // namespace c2d
