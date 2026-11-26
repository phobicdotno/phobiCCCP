#include "layers.h"
#include <QJsonArray>
#include <QSet>
#include <QUuid>

namespace c2d {
namespace layers {

QString keyOf(const QJsonObject &layer)
{
    const QString uuid = layer.value(QStringLiteral("uuid")).toString();
    if (!uuid.isEmpty())
        return uuid;
    return QStringLiteral("name:") + layer.value(QStringLiteral("name")).toString();
}

QString keyOf(const Element &e)
{
    return keyOf(e.raw.value(QStringLiteral("layer")).toObject());
}

bool isHidden(const Element &e)
{
    return !e.raw.value(QStringLiteral("layer")).toObject().value(QStringLiteral("visible")).toBool(true);
}

bool isLocked(const Element &e)
{
    return e.raw.value(QStringLiteral("layer")).toObject().value(QStringLiteral("locked")).toBool(false);
}

QVector<Layer> list(const Document &d, const QVector<QJsonObject> &extra)
{
    QVector<Layer> out;
    QHash<QString, int> at;
    for (const Element &e : d.elements()) {
        const QJsonObject l = e.raw.value(QStringLiteral("layer")).toObject();
        const QString k = keyOf(l);
        auto it = at.constFind(k);
        if (it == at.constEnd()) {
            at.insert(k, int(out.size()));
            out.append({k, l, 1});
        } else {
            ++out[*it].count;
        }
    }
    for (const QJsonObject &l : extra) {
        const QString k = keyOf(l);
        if (!at.contains(k)) {
            at.insert(k, int(out.size()));
            out.append({k, l, 0});
        }
    }
    return out;
}

QJsonObject makeLayer(const QString &name)
{
    QJsonObject l;
    l.insert("blue", 0); l.insert("green", 0); l.insert("red", 0);
    l.insert("locked", false);
    l.insert("name", name);
    l.insert("uuid", QUuid::createUuid().toString(QUuid::WithoutBraces));
    l.insert("visible", true);
    return l;
}

QVector<Element> edited(const Document &d, const QString &key,
                        const std::function<void(QJsonObject &)> &change)
{
    QVector<Element> out;
    for (const Element &e : d.elements()) {
        if (keyOf(e) != key)
            continue;
        QJsonObject o = e.raw;
        QJsonObject l = o.value(QStringLiteral("layer")).toObject();
        change(l);
        o.insert(QStringLiteral("layer"), l);
        Element n = e;
        n.raw = o;
        out.append(n);
    }
    return out;
}

QVector<Element> movedTo(const Document &d, const QStringList &ids, const QJsonObject &layer)
{
    const QSet<QString> want(ids.begin(), ids.end());
    const QString k = keyOf(layer);
    QVector<Element> out;
    for (const Element &e : d.elements()) {
        if (!want.contains(e.id) || keyOf(e) == k)
            continue;
        Element n = e;
        n.raw.insert(QStringLiteral("layer"), layer);
        out.append(n);
    }
    return out;
}

QStringList idsOn(const Document &d, const QString &key)
{
    QStringList ids;
    for (const Element &e : d.elements())
        if (keyOf(e) == key)
            ids << e.id;
    return ids;
}

} // namespace layers

namespace groups {

static QJsonArray idsOf(const Element &e)
{
    return e.raw.value(QStringLiteral("group_id")).toArray();
}

QString outermost(const Element &e)
{
    const QJsonArray g = idsOf(e);
    for (int i = int(g.size()) - 1; i >= 0; --i)
        if (g.at(i).isString() && !g.at(i).toString().isEmpty())
            return g.at(i).toString();
    return QString();
}

QStringList expand(const Document &d, const QStringList &ids)
{
    const QSet<QString> want(ids.begin(), ids.end());
    QSet<QString> outer;
    for (const Element &e : d.elements())
        if (want.contains(e.id)) {
            const QString g = outermost(e);
            if (!g.isEmpty())
                outer.insert(g);
        }
    if (outer.isEmpty())
        return ids;
    QStringList out = ids;
    for (const Element &e : d.elements())
        if (!want.contains(e.id) && outer.contains(outermost(e)))
            out << e.id;
    return out;
}

QVector<Element> grouped(const Document &d, const QStringList &ids)
{
    // Whole groups go in: a shape of a group brings the rest of it along.
    const QStringList all = expand(d, ids);
    if (all.size() < 2)
        return {};
    const QSet<QString> want(all.begin(), all.end());
    const QString gid = QUuid::createUuid().toString();
    QVector<Element> out;
    for (const Element &e : d.elements()) {
        if (!want.contains(e.id))
            continue;
        Element n = e;
        QJsonArray g = idsOf(e);
        g.append(gid);
        n.raw.insert(QStringLiteral("group_id"), g);
        out.append(n);
    }
    return out;
}

QVector<Element> ungrouped(const Document &d, const QStringList &ids)
{
    QSet<QString> outer;
    const QSet<QString> want(ids.begin(), ids.end());
    for (const Element &e : d.elements())
        if (want.contains(e.id) && !outermost(e).isEmpty())
            outer.insert(outermost(e));
    QVector<Element> out;
    for (const Element &e : d.elements()) {
        const QString o = outermost(e);
        if (o.isEmpty() || !outer.contains(o))
            continue;
        QJsonArray g = idsOf(e);
        for (int i = int(g.size()) - 1; i >= 0; --i)
            if (g.at(i).toString() == o) { g.removeAt(i); break; }
        Element n = e;
        n.raw.insert(QStringLiteral("group_id"), g);
        out.append(n);
    }
    return out;
}

} // namespace groups
} // namespace c2d
