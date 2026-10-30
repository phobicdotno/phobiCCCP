#include "stockstamp.h"

#include <QDateTime>
#include <QSettings>

namespace c2d {

static std::function<void()> g_hook;

void stampStock(int toolNumber)
{
    QSettings().setValue(QStringLiteral("toolStock/%1/ts").arg(toolNumber),
                         QDateTime::currentMSecsSinceEpoch());
    if (g_hook)
        g_hook();
}

void stampCollections()
{
    QSettings().setValue(QStringLiteral("tools/collectionsTs"), QDateTime::currentMSecsSinceEpoch());
    if (g_hook)
        g_hook();
}

void setStockChangedHook(std::function<void()> hook)
{
    g_hook = std::move(hook);
}

} // namespace c2d
