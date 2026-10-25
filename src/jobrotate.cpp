#include "jobrotate.h"
#include "backgroundimage.h"
#include "c2ddocument.h"
#include "gcodeexport.h"
#include "model3d.h"
#include "parametric.h"
#include "vectorops.h"

#include <QJsonArray>
#include <QSet>

#include <cmath>

namespace c2d {
namespace jobrotate {

namespace {

QPointF jsonPt(const QJsonValue &v)
{
    const QJsonArray a = v.toArray();
    return a.size() == 2 ? QPointF(a.at(0).toDouble(), a.at(1).toDouble()) : QPointF();
}

QJsonArray jsonXY(QPointF p)
{
    return QJsonArray{p.x(), p.y()};
}

// The rows of a point list turned by the transform's linear part (they are
// relative to the element's position).
QJsonArray turnRows(const QJsonValue &rows, const QTransform &lin)
{
    QJsonArray out;
    for (const QJsonValue &v : rows.toArray())
        out.append(jsonXY(lin.map(jsonPt(v))));
    return out;
}

// The toolpath fields that are a direction on the board. "angle" means a
// direction only on these types (on a drill, a v-carve or a tool it is not).
bool directional(const QString &type, const QString &key)
{
    if (key == QLatin1String("raster_angle"))
        return true;
    if (key != QLatin1String("angle"))
        return false;
    return type == QLatin1String("pocket_toolpath") || type == QLatin1String("keyhole_toolpath")
        || type == QLatin1String("texture_toolpath") || type == QLatin1String("engrave_toolpath");
}

// An expression bound to a direction field, a quarter turn on: " - 90" more,
// counted up to " - 270" and then dropped, so four turns give it back.
QString turnExpr(const QString &expr)
{
    static const char *steps[] = {" - 90", " - 180", " - 270"};
    for (int i = 0; i < 3; ++i) {
        const QString s = QLatin1String(steps[i]);
        if (expr.endsWith(s)) {
            const QString base = expr.left(expr.size() - s.size());
            if (i < 2)
                return base + QLatin1String(steps[i + 1]);
            // A whole turn: back to the expression as it was ("(e)" to "e").
            return base.startsWith(QLatin1Char('(')) && base.endsWith(QLatin1Char(')'))
                       ? base.mid(1, base.size() - 2) : base;
        }
    }
    return QLatin1Char('(') + expr + QLatin1Char(')') + QLatin1String(steps[0]);
}

bool tiedToOrigin(const Constraint &c)
{
    for (const GeoRef &r : c.refs)
        if (r.isOrigin())
            return true;
    return false;
}

} // namespace

QRectF designBounds(const Document &doc, const Model3D *model)
{
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    auto add = [&](const QRectF &r) {
        x0 = std::min(x0, r.left());
        y0 = std::min(y0, r.top());
        x1 = std::max(x1, r.right());
        y1 = std::max(y1, r.bottom());
    };
    for (const Element &e : doc.elements())
        if (!e.painterPath.isEmpty())
            add(e.painterPath.boundingRect());
    if (model)
        for (const ModelComponent &c : model->components) {
            const QRectF f = c.footprint();
            if (!f.isEmpty())
                add(f);
        }
    if (!(x1 >= x0) || !(y1 >= y0))
        return {};
    return QRectF(QPointF(x0, y0), QPointF(x1, y1));
}

QTransform quarterTurn(const QRectF &b)
{
    // x' = y + dx, y' = -x + dy (Qt: x' = m11 x + m21 y + dx, y' = m12 x + m22 y + dy).
    const double x0 = b.left(), y0 = b.top();
    return QTransform(0, -1, 1, 0, x0 - y0, x0 + y0 + b.width());
}

double turnAngle(double deg)
{
    double a = deg - 90.0;
    if (a <= -180.0)
        a += 360.0;
    return a;
}

Element turnElement(const Element &e, const QTransform &q)
{
    if (e.geometryType == QLatin1String("text"))
        return vec::transformElement(e, q);
    const QTransform lin(q.m11(), q.m12(), q.m21(), q.m22(), 0, 0);
    QJsonObject o = e.raw;
    for (const char *key : {"position", "center"})
        if (o.contains(QLatin1String(key)))
            o.insert(QLatin1String(key), jsonXY(q.map(jsonPt(o.value(QLatin1String(key))))));
    for (const char *key : {"points", "cp1", "cp2"})
        if (o.contains(QLatin1String(key)))
            o.insert(QLatin1String(key), turnRows(o.value(QLatin1String(key)), lin));
    if (e.geometryType == QLatin1String("rectangle")) {
        // Lying the other way: width and height swap, its own rotation stays.
        const QJsonValue w = o.value("width"), h = o.value("height");
        o.insert("width", h);
        o.insert("height", w);
    } else if (e.geometryType == QLatin1String("regular_polygon")) {
        o.insert("rotation", turnAngle(o.value("rotation").toDouble()));
    }
    return Element::fromJson(o);
}

Report turnDocument(Document &doc, const QTransform &q)
{
    Report rep;
    const Document before = doc;
    const QStringList wasRed = param::unsatisfied(before);

    QHash<QString, QTransform> through;
    QVector<Element> els = doc.elements();
    for (Element &e : els) {
        e = turnElement(e, q);
        through.insert(e.id, q);
    }
    doc.setElements(els);
    rep.vectors = int(els.size());

    // Toolpaths point at vectors by id; only their directions turn.
    QVector<Toolpath> tps = doc.toolpaths();
    for (Toolpath &t : tps) {
        QJsonObject j = t.json;
        QJsonObject binds = j.value(QStringLiteral("phobic_expr")).toObject();
        bool any = false;
        for (const QString key : {QStringLiteral("angle"), QStringLiteral("raster_angle")}) {
            if (!directional(t.type, key) || !j.contains(key))
                continue;
            j.insert(key, turnAngle(numOr(j.value(key), 0)));
            if (binds.contains(key)) {
                QJsonObject b = binds.value(key).toObject();
                b.insert(QStringLiteral("expr"), turnExpr(b.value(QStringLiteral("expr")).toString()));
                binds.insert(key, b);
                j.insert(QStringLiteral("phobic_expr"), binds);
            }
            any = true;
        }
        if (any) {
            t.json = j;
            ++rep.toolpaths;
        }
    }
    doc.setToolpaths(tps);

    if (doc.parametric().constraints.isEmpty())
        return rep;

    // Constraints follow their geometry (references renumbered where a
    // shape's numbering changed); pinned points, dimension labels and the
    // axis constraints turn with it.
    param::reconcile(before, doc, nullptr, nullptr, through);
    const QTransform lin(q.m11(), q.m12(), q.m21(), q.m22(), 0, 0);
    Parametric p = doc.parametric();
    for (Constraint &c : p.constraints) {
        if (c.type == Constraint::Fix)
            c.at = q.map(c.at);
        if (c.isDimension() && !c.label.isNull())
            c.label = lin.map(c.label);
        switch (c.type) {
        case Constraint::Horizontal: c.type = Constraint::Vertical; break;
        case Constraint::Vertical: c.type = Constraint::Horizontal; break;
        case Constraint::HDistance: c.type = Constraint::VDistance; break;
        case Constraint::VDistance: c.type = Constraint::HDistance; break;
        default: break;
        }
    }
    doc.setParametric(p);
    // Which side a horizontal or vertical distance holds on, as it lies now.
    for (Constraint &c : p.constraints)
        if (c.type == Constraint::HDistance || c.type == Constraint::VDistance) {
            Constraint fresh = c;
            param::prime(doc, fresh);
            c.sense = fresh.sense;
        }
    doc.setParametric(p);

    // Tied to the origin, which stays: a plain-number dimension nothing else
    // uses takes the value it has now; the rest cannot hold and is reported.
    const QStringList red = param::unsatisfied(doc);
    const QSet<QString> used = param::usedNames(doc, p);
    bool changed = false;
    for (Constraint &c : p.constraints) {
        // Red already and not about the origin: not this turn's doing.
        if (!red.contains(c.uid) || (wasRed.contains(c.uid) && !tiedToOrigin(c)))
            continue;
        const bool plain = c.isDimension() && param::namesIn(c.expr).isEmpty()
                           && (c.name.isEmpty() || !used.contains(c.name));
        if (tiedToOrigin(c) && plain) {
            Constraint fresh = c;
            param::prime(doc, fresh);
            c.value = fresh.value;
            c.sense = fresh.sense;
            const QString shown = param::formatNumber(c.value);
            c.expr = std::fabs(shown.toDouble() - c.value) < 1e-9 ? shown
                                                                   : QString::number(c.value, 'g', 15);
            ++rep.remeasured;
            changed = true;
        } else {
            rep.notHeld << c.uid;
        }
    }
    if (changed)
        doc.setParametric(p);
    return rep;
}

void turnModel(Model3D &model, const QTransform &q)
{
    for (ModelComponent &c : model.components) {
        if (c.kind == ModelComponent::FromVectors)
            continue;   // made from vectors: follows them
        if (c.kind == ModelComponent::Texture) {
            // Tiles without bounds: the tiling turns about its anchor.
            const QPointF a = q.map(QPointF(c.x, c.y));
            c.x = a.x();
            c.y = a.y();
        } else {
            const QRectF f = c.footprint();
            if (f.isEmpty())
                continue;   // nothing placed (no bytes that decode)
            const QRectF g = q.mapRect(f);
            c.x = g.left();
            c.y = g.top();
        }
        c.turns = (c.turns + 1) & 3;
    }
    model.userChanged = true;
}

void turnBackground(BackgroundImage &bg, const QTransform &q)
{
    if (bg.isNull())
        return;
    const QRectF r = bg.rectMm();
    const QPointF c = q.map(r.center());
    bg.x = c.x() - r.width() / 2;
    bg.y = c.y() - r.height() / 2;
    bg.rotationDeg = turnAngle(bg.rotationDeg);
    bg.userChanged = true;
}

} // namespace jobrotate
} // namespace c2d
