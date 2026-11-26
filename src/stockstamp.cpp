#include "stockstamp.h"

#include "toolcollections.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <algorithm>
#include <iterator>
#include <utility>

namespace c2d {

static std::function<void()> g_hook;

static const char *kCollKey = "tools/collections";
static const char *kCollTs = "tools/collectionsTs";
static const char *kStampsKey = "tools/collectionsStamps";
static const char *kFreshKey = "tools/collectionsFresh";

// Never older than the stamp it replaces: a copy whose clock runs ahead has
// sent numbers stamped later than this clock says now, and a change made here
// with a plain "now" lost to them on the next announce and was put back.
static qint64 stamp(QSettings &s, const QString &key)
{
    const qint64 was = s.value(key, 0).toLongLong();
    const qint64 now = qMax(QDateTime::currentMSecsSinceEpoch(), was + 1);
    s.setValue(key, now);
    return now;
}

void stampStock(int toolNumber)
{
    QSettings s;
    stamp(s, QStringLiteral("toolStock/%1/ts").arg(toolNumber));
    if (g_hook)
        g_hook();
}

void stampCollections(const QStringList &changed, const QStringList &removed)
{
    QSettings s;
    const qint64 was = s.value(QLatin1String(kCollTs), 0).toLongLong();
    // Read before the new stamp: settings from before per-collection stamps
    // date every stored collection at the old whole-set stamp.
    CollectionStamps st = CollectionStamps::fromSettings(s);
    const qint64 now = stamp(s, QLatin1String(kCollTs));
    if (was == 0)
        s.setValue(QLatin1String(kFreshKey), true);
    if (!changed.isEmpty() || !removed.isEmpty()) {
        for (const QString &n : changed) {
            st.ts.insert(n, now);
            st.gone.remove(n);
        }
        for (const QString &n : removed) {
            st.ts.remove(n);
            st.gone.insert(n, now);
        }
        st.prune();
        st.toSettings(s);
    }
    if (g_hook)
        g_hook();
}

void setStockChangedHook(std::function<void()> hook)
{
    g_hook = std::move(hook);
}

static QHash<QString, qint64> readMap(const QJsonObject &o)
{
    QHash<QString, qint64> out;
    for (auto it = o.constBegin(); it != o.constEnd(); ++it) {
        const qint64 t = static_cast<qint64>(it.value().toDouble());
        if (t > 0 && !it.key().isEmpty())
            out.insert(it.key(), t);
    }
    return out;
}

static QJsonObject writeMap(const QHash<QString, qint64> &m)
{
    QJsonObject o;
    for (auto it = m.constBegin(); it != m.constEnd(); ++it)
        o.insert(it.key(), static_cast<double>(it.value()));
    return o;
}

CollectionStamps CollectionStamps::fromSettings(QSettings &s)
{
    CollectionStamps st;
    if (s.contains(QLatin1String(kStampsKey))) {
        const QJsonObject o =
            QJsonDocument::fromJson(s.value(QLatin1String(kStampsKey)).toByteArray()).object();
        st.ts = readMap(o.value(QStringLiteral("ts")).toObject());
        st.gone = readMap(o.value(QStringLiteral("gone")).toObject());
        return st;
    }
    const qint64 whole = s.value(QLatin1String(kCollTs), 0).toLongLong();
    ToolCollections stored;
    const QByteArray json = s.value(QLatin1String(kCollKey)).toByteArray();
    if (whole > 0 && !json.isEmpty() && ToolCollections::fromJson(json, &stored))
        for (const ToolCollection &c : stored.all())
            st.ts.insert(c.name, whole);
    return st;
}

void CollectionStamps::toSettings(QSettings &s) const
{
    const QJsonObject o{{QStringLiteral("ts"), writeMap(ts)},
                        {QStringLiteral("gone"), writeMap(gone)}};
    s.setValue(QLatin1String(kStampsKey), QJsonDocument(o).toJson(QJsonDocument::Compact));
}

void CollectionStamps::prune()
{
    if (gone.size() <= kMaxGone)
        return;
    QList<qint64> times = gone.values();
    std::sort(times.begin(), times.end(), std::greater<qint64>());
    const qint64 cut = times.at(kMaxGone - 1);
    for (auto it = gone.begin(); it != gone.end();)
        it = (it.value() < cut) ? gone.erase(it) : std::next(it);
    // Equal stamps at the cut can leave a few more; drop those too.
    while (gone.size() > kMaxGone)
        gone.erase(gone.begin());
}

} // namespace c2d
