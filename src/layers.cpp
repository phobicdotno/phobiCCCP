#include "layers.h"
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
} // namespace c2d
