#pragma once
#include <QHash>
#include <QString>
#include <QStringList>
#include <functional>

class QJsonValue;
class QSettings;

namespace c2d {

// Stamp a change to a bit's stock counts or to the Collections with the time,
// so copies of the app on the network can tell which side is newer (see
// StockSync). The hook is called after each stamp; StockSync sets it.
void stampStock(int toolNumber);
// `changed`: collections added or edited, `removed`: deleted (a rename is
// both). Each gets its own stamp so a peer takes only the collections that
// are newer here. The first stamp of a copy that never had Collections of
// its own (a fresh install) also marks it fresh: until it has merged with a
// peer's, its lists only add to theirs, they never replace them.
void stampCollections(const QStringList &changed = {}, const QStringList &removed = {});
void setStockChangedHook(std::function<void()> hook);

// A stamp (ms since the epoch) from JSON: -1 unless it is a whole number from
// 0 to 2^53. Converting any double to qint64 is undefined when it is out of
// range, and these come from anyone on the network.
qint64 jsonStamp(const QJsonValue &v);

// Per-collection stamps kept beside tools/collections: when each collection
// last changed (none: never changed, a built-in default) and when one was
// deleted, so a copy that still has it drops it.
struct CollectionStamps {
    static constexpr int kMaxGone = 200;
    QHash<QString, qint64> ts;
    QHash<QString, qint64> gone;

    // Settings written before there were per-collection stamps count every
    // stored collection as changed at tools/collectionsTs.
    static CollectionStamps fromSettings(QSettings &s);
    void toSettings(QSettings &s) const;
    void prune();                     // keep the kMaxGone newest deletions
    qint64 latest() const;            // the newest stamp of either kind, 0 if none
};

} // namespace c2d
