#include "stockstamp.h"

#include <QDateTime>
#include <QSettings>

namespace c2d {

static std::function<void()> g_hook;

// Never older than the stamp it replaces: a copy whose clock runs ahead has
// sent numbers stamped later than this clock says now, and a change made here
// with a plain "now" lost to them on the next announce and was put back.
static void stamp(const QString &key)
{
    QSettings s;
    const qint64 was = s.value(key, 0).toLongLong();
    s.setValue(key, qMax(QDateTime::currentMSecsSinceEpoch(), was + 1));
}

void stampStock(int toolNumber)
{
    stamp(QStringLiteral("toolStock/%1/ts").arg(toolNumber));
    if (g_hook)
        g_hook();
}

void stampCollections()
{
    stamp(QStringLiteral("tools/collectionsTs"));
    if (g_hook)
        g_hook();
}

void setStockChangedHook(std::function<void()> hook)
{
    g_hook = std::move(hook);
}

} // namespace c2d
