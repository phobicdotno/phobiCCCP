#include "stocksync.h"

#include "stockstamp.h"
#include "toolcollections.h"
#include "toollibrary.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QSettings>
#include <QTimer>
#include <QUdpSocket>
#include <QUuid>
#include <memory>

namespace c2d {

static const char *kCollKey = "tools/collections";
static const char *kCollTs = "tools/collectionsTs";
static const char *kFresh = "tools/collectionsFresh";
static const char *kApp = "phobiCCCP-stock";

StockSync::StockSync(const QString &settingsFile, quint16 listenPort, const QHostAddress &target,
                     quint16 targetPort, QObject *parent)
    : QObject(parent), m_file(settingsFile), m_listen(listenPort), m_targetPort(targetPort),
      m_target(target), m_self(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
    m_soon = new QTimer(this);
    m_soon->setSingleShot(true);
    m_soon->setInterval(500);
    connect(m_soon, &QTimer::timeout, this, [this] { announce(); });
}

QSettings *StockSync::open() const
{
    return m_file.isEmpty() ? new QSettings : new QSettings(m_file, QSettings::IniFormat);
}

bool StockSync::start()
{
    if (m_sock)
        return true;                                    // started already
    m_sock = new QUdpSocket(this);
    if (!m_sock->bind(QHostAddress::AnyIPv4, m_listen,
                      QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        m_sock->deleteLater();
        m_sock = nullptr;
        return false;
    }
    connect(m_sock, &QUdpSocket::readyRead, this, &StockSync::incoming);
    auto *t = new QTimer(this);
    connect(t, &QTimer::timeout, this, [this] { announce(); });
    t->start(30000);
    QTimer::singleShot(1000, this, [this] { announce(); });
    return true;
}

const QSet<int> &StockSync::knownTools() const
{
    // Only bits of the tool library are taken (and sent): any packet on the
    // port could otherwise fill the settings with numbers 1..99999. Reread
    // now and then, so bits a library sync brought in count too.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_knownAt == 0 || now - m_knownAt > 60000 || now < m_knownAt) {
        ToolLibrary lib;
        QSet<int> known;
        if (lib.loadDefault())
            for (const LibraryTool &t : lib.tools())
                if (t.number > 0)
                    known.insert(t.number);
        m_known = known;
        m_knownAt = now;
    }
    return m_known;
}

static QJsonObject stampsJson(const QHash<QString, qint64> &m)
{
    QJsonObject o;
    for (auto it = m.constBegin(); it != m.constEnd(); ++it)
        o.insert(it.key(), static_cast<double>(it.value()));
    return o;
}

static int jsonSize(const QJsonObject &o)
{
    return int(QJsonDocument(o).toJson(QJsonDocument::Compact).size());
}

// The Collections as sent: the stored list, the whole-set stamp an older copy
// goes by, and the per-collection stamps and deletions a current one merges
// with. A fresh copy sends no whole-set stamp (an older copy would take its
// few lists over its own) and no deletions, and says it is fresh.
static QJsonObject collectionsPayload(QSettings &s)
{
    QJsonObject coll;
    const qint64 cts = s.value(QLatin1String(kCollTs), 0).toLongLong();
    if (cts <= 0)
        return coll;
    const QByteArray stored = s.value(QLatin1String(kCollKey)).toByteArray();
    const QJsonDocument doc = QJsonDocument::fromJson(stored);
    if (!doc.isObject())
        return coll;
    const QByteArray json = doc.toJson(QJsonDocument::Compact);
    if (json.size() >= 50000)
        return coll;
    const CollectionStamps st = CollectionStamps::fromSettings(s);
    coll.insert(QStringLiteral("json"), QString::fromUtf8(json));
    coll.insert(QStringLiteral("items"), stampsJson(st.ts));
    if (s.value(QLatin1String(kFresh), false).toBool()) {
        coll.insert(QStringLiteral("fresh"), true);
    } else {
        coll.insert(QStringLiteral("ts"), static_cast<double>(cts));
        coll.insert(QStringLiteral("gone"), stampsJson(st.gone));
    }
    return coll;
}

QList<QByteArray> StockSync::payloads() const
{
    std::unique_ptr<QSettings> s(open());
    s->sync();
    const QSet<int> &known = knownTools();
    QJsonObject root;
    root.insert(QStringLiteral("app"), QLatin1String(kApp));
    root.insert(QStringLiteral("v"), 1);
    root.insert(QStringLiteral("from"), m_self);
    root.insert(QStringLiteral("coll"), collectionsPayload(*s));
    // ,"stock":{} and a little to spare: each estimate is at least the size
    // the entry takes in the compact JSON.
    constexpr int kStockFrame = 16;
    if (jsonSize(root) + kStockFrame > kMaxDatagram)
        root.insert(QStringLiteral("coll"), QJsonObject());   // cannot be sent: skipped
    QList<QByteArray> out;
    QJsonObject stock;
    int size = jsonSize(root) + kStockFrame;
    s->beginGroup(QStringLiteral("toolStock"));
    for (const QString &n : s->childGroups()) {
        bool ok = false;
        const int number = n.toInt(&ok);
        if (!ok || !known.contains(number))
            continue;
        s->beginGroup(n);
        const qint64 ts = s->value(QStringLiteral("ts"), 0).toLongLong();
        if (ts > 0) {
            QJsonObject o;
            o.insert(QStringLiteral("have"), s->value(QStringLiteral("have"), 0).toInt());
            o.insert(QStringLiteral("broken"), s->value(QStringLiteral("broken"), 0).toInt());
            o.insert(QStringLiteral("ts"), static_cast<double>(ts));
            const int add = int(n.size()) + 4 + jsonSize(o);   // "n":{...},
            if (!stock.isEmpty() && size + add > kMaxDatagram) {
                QJsonObject full = root;
                full.insert(QStringLiteral("stock"), stock);
                out.append(QJsonDocument(full).toJson(QJsonDocument::Compact));
                root.insert(QStringLiteral("coll"), QJsonObject());   // in the first one only
                stock = QJsonObject();
                size = jsonSize(root) + kStockFrame;
            }
            stock.insert(n, o);
            size += add;
        }
        s->endGroup();
    }
    s->endGroup();
    root.insert(QStringLiteral("stock"), stock);
    out.append(QJsonDocument(root).toJson(QJsonDocument::Compact));
    return out;
}

QByteArray StockSync::payload() const
{
    return payloads().value(0);
}

// Replace (or append) one collection, keeping the others' order.
static void putCollection(ToolCollections &c, const ToolCollection &x)
{
    ToolCollections one;
    one.add(x.name);
    for (int n : x.tools)
        one.addTool(x.name, n);
    c.merge(one);
}

// Stamps from a packet: in range, and for names that are plausible.
static QHash<QString, qint64> readStamps(const QJsonObject &o, qint64 limit, int max)
{
    QHash<QString, qint64> out;
    for (auto it = o.constBegin(); it != o.constEnd() && out.size() < max; ++it) {
        const qint64 t = static_cast<qint64>(it.value().toDouble());
        if (it.key().trimmed().isEmpty() || it.key().size() > 200 || t <= 0 || t > limit)
            continue;
        out.insert(it.key(), t);
    }
    return out;
}

// A peer's Collections into ours (settings `s`). True when ours changed;
// *tell when the peer lacks something we have now.
static bool mergeCollections(QSettings &s, const QJsonObject &coll, qint64 limit, bool *tell)
{
    if (coll.isEmpty())
        return false;
    const bool pfresh = coll.value(QStringLiteral("fresh")).toBool();
    const qint64 pts = static_cast<qint64>(coll.value(QStringLiteral("ts")).toDouble());
    const QByteArray pj = coll.value(QStringLiteral("json")).toString().toUtf8();
    if (pj.size() >= 50000 || pts < 0 || pts > limit || (!pfresh && pts == 0))
        return false;
    // Only what the Collections reader takes: any other JSON object ("{}")
    // read back as nothing and showed the two built-in lists instead of the
    // user's own, which the next edit then saved over them.
    ToolCollections peer;
    if (!ToolCollections::fromJson(pj, &peer))
        return false;

    const qint64 lts = s.value(QLatin1String(kCollTs), 0).toLongLong();
    const bool lfresh = lts > 0 && s.value(QLatin1String(kFresh), false).toBool();
    const QByteArray lj = s.value(QLatin1String(kCollKey)).toByteArray();
    ToolCollections local;
    if (lj.isEmpty() || !ToolCollections::fromJson(lj, &local))
        local = ToolCollections::defaults();
    const CollectionStamps mine = CollectionStamps::fromSettings(s);

    // The peer's stamps: per collection from a current copy; an older copy
    // sends its Collections as one, newer replacing all of ours.
    CollectionStamps theirs;
    if (coll.contains(QStringLiteral("items"))) {
        const QHash<QString, qint64> items =
            readStamps(coll.value(QStringLiteral("items")).toObject(), limit, 1000);
        for (const ToolCollection &c : peer.all())
            if (items.contains(c.name))
                theirs.ts.insert(c.name, items.value(c.name));
        if (!pfresh)
            theirs.gone = readStamps(coll.value(QStringLiteral("gone")).toObject(), limit,
                                     CollectionStamps::kMaxGone);
    } else {
        if (!lfresh && lts > 0 && pts <= lts)
            return false;
        for (const ToolCollection &c : peer.all())
            theirs.ts.insert(c.name, pts);
        if (!lfresh)
            for (const ToolCollection &c : local.all())
                if (!peer.byName(c.name))
                    theirs.gone.insert(c.name, pts);
    }

    // Nothing of our own yet: theirs as they are (a fresh peer's stay its own
    // until it has merged with a copy that has history).
    if (lts == 0) {
        if (pfresh)
            return false;
        s.setValue(QLatin1String(kCollKey), pj);
        s.setValue(QLatin1String(kCollTs), static_cast<qint64>(pts));
        theirs.toSettings(s);
        return true;
    }

    ToolCollections result = local;
    CollectionStamps out = mine;
    bool changed = false;
    qint64 newTs = 0;                      // set when something is stamped here and now

    if (lfresh || pfresh) {
        // A fresh install meets a set it has never seen: neither side's lists
        // replace the other's, the bits and collections are pooled.
        newTs = qMax(QDateTime::currentMSecsSinceEpoch(), qMax(lts, pts) + 1);
        bool stamped = false;
        for (const ToolCollection &p : peer.all()) {
            if (result.byName(p.name)) {
                for (int n : p.tools)
                    result.addTool(p.name, n);
            } else if (p.tools.isEmpty() && theirs.ts.value(p.name, 0) == 0) {
                continue;                  // an untouched built-in default adds nothing
            } else if (lfresh || mine.gone.value(p.name, -1) < theirs.ts.value(p.name, 0)) {
                putCollection(result, p);
            }
        }
        // Ours that the peer deleted after we last touched them go, and so do
        // built-in defaults never touched here that it does not have: a fresh
        // install's empty "Unused" does not come back everywhere.
        if (lfresh && !pfresh)
            for (const ToolCollection &c : local.all()) {
                if (peer.byName(c.name))
                    continue;
                const qint64 touched = mine.ts.value(c.name, 0);
                if (theirs.gone.value(c.name, 0) > touched) {
                    result.remove(c.name);
                    out.ts.remove(c.name);
                    out.gone.insert(c.name, theirs.gone.value(c.name));
                    changed = true;
                } else if (touched == 0 && c.tools.isEmpty()) {
                    result.remove(c.name);
                    changed = true;
                }
            }
        for (const ToolCollection &c : result.all()) {
            const ToolCollection *was = local.byName(c.name);
            const ToolCollection *their = peer.byName(c.name);
            const bool here = !was || was->tools != c.tools;
            // The same bits in another order are nothing the peer lacks.
            const bool there = !their || their->tools.size() != c.tools.size()
                               || QSet<int>(their->tools.cbegin(), their->tools.cend())
                                      != QSet<int>(c.tools.cbegin(), c.tools.cend());
            changed = changed || here;
            if (there)
                *tell = true;
            if (there && (here || (lfresh && !pfresh))) {
                // New here, or ours to give: stamped now so the peer takes it.
                out.ts.insert(c.name, newTs);
                out.gone.remove(c.name);
                stamped = true;
            } else if (!there) {
                const qint64 t = qMax(out.ts.value(c.name, 0), theirs.ts.value(c.name, 0));
                if (t > 0)
                    out.ts.insert(c.name, t);
                out.gone.remove(c.name);
            }
        }
        if (!stamped)
            newTs = 0;
        // Met a copy with history: from now on the newest edit wins.
        if (lfresh && !pfresh)
            s.setValue(QLatin1String(kFresh), false);
    } else {
        for (const ToolCollection &p : peer.all()) {
            const qint64 t = theirs.ts.value(p.name, 0);
            if (const ToolCollection *c = result.byName(p.name)) {
                if (t > out.ts.value(p.name, 0)) {
                    if (c->tools != p.tools) {
                        putCollection(result, p);
                        changed = true;
                    }
                    out.ts.insert(p.name, t);
                }
            } else if (t > out.gone.value(p.name, -1)) {
                putCollection(result, p);
                if (t > 0)
                    out.ts.insert(p.name, t);
                out.gone.remove(p.name);
                changed = true;
            }
        }
        for (auto it = theirs.gone.constBegin(); it != theirs.gone.constEnd(); ++it) {
            if (result.byName(it.key())) {
                if (out.ts.value(it.key(), 0) < it.value()) {
                    result.remove(it.key());
                    out.ts.remove(it.key());
                    out.gone.insert(it.key(), it.value());
                    changed = true;
                }
            } else if (it.value() > out.gone.value(it.key(), 0)) {
                out.gone.insert(it.key(), it.value());
            }
        }
    }

    const bool restamped = out.ts != mine.ts || out.gone != mine.gone;
    if (!changed && !restamped && newTs == 0)
        return false;
    if (changed)
        s.setValue(QLatin1String(kCollKey), result.toJson());
    s.setValue(QLatin1String(kCollTs), static_cast<qint64>(qMax(qMax(lts, pts), newTs)));
    out.prune();
    out.toSettings(s);
    return changed;
}

bool StockSync::merge(const QByteArray &data)
{
    m_tellPeers = false;
    if (data.size() > kMaxDatagram)
        return false;
    const QJsonObject root = QJsonDocument::fromJson(data).object();
    if (root.value(QStringLiteral("app")).toString() != QLatin1String(kApp)
        || root.value(QStringLiteral("v")).toInt() != 1)
        return false;
    const qint64 limit = QDateTime::currentMSecsSinceEpoch() + 24LL * 3600 * 1000;
    const QSet<int> &known = knownTools();
    std::unique_ptr<QSettings> s(open());
    s->sync();
    bool changed = false;
    const QJsonObject stock = root.value(QStringLiteral("stock")).toObject();
    int seen = 0;
    for (auto it = stock.constBegin(); it != stock.constEnd() && seen < kMaxStockEntries;
         ++it, ++seen) {
        bool ok = false;
        const int number = it.key().toInt(&ok);
        const QJsonObject o = it.value().toObject();
        const qint64 ts = static_cast<qint64>(o.value(QStringLiteral("ts")).toDouble());
        const int have = o.value(QStringLiteral("have")).toInt(-1);
        const int broken = o.value(QStringLiteral("broken")).toInt(-1);
        if (!ok || number < 1 || number > 99999 || !known.contains(number) || ts <= 0
            || ts > limit || have < 0 || have > 9999 || broken < 0 || broken > 9999)
            continue;
        const QString base = QStringLiteral("toolStock/%1/").arg(number);
        if (ts <= s->value(base + QStringLiteral("ts"), 0).toLongLong())
            continue;
        s->setValue(base + QStringLiteral("have"), have);
        s->setValue(base + QStringLiteral("broken"), broken);
        s->setValue(base + QStringLiteral("ts"), ts);
        changed = true;
    }
    bool tell = false;
    if (mergeCollections(*s, root.value(QStringLiteral("coll")).toObject(), limit, &tell))
        changed = true;
    m_tellPeers = tell;
    s->sync();
    return changed;
}

void StockSync::announce()
{
    if (!m_sock)
        return;
    // 255.255.255.255 alone does not leave every interface on every system:
    // also each up, non-loopback interface's own broadcast address.
    QList<QHostAddress> to{m_target};
    if (m_target == QHostAddress(QHostAddress::Broadcast))
        for (const QNetworkInterface &i : QNetworkInterface::allInterfaces()) {
            const auto f = i.flags();
            if (!(f & QNetworkInterface::IsUp) || (f & QNetworkInterface::IsLoopBack)
                || !(f & QNetworkInterface::CanBroadcast))
                continue;
            for (const QNetworkAddressEntry &e : i.addressEntries())
                if (!e.broadcast().isNull() && !to.contains(e.broadcast()))
                    to.append(e.broadcast());
        }
    for (const QByteArray &p : payloads())
        for (const QHostAddress &a : to)
            m_sock->writeDatagram(p, a, m_targetPort);
}

void StockSync::announceSoon()
{
    if (!m_soon->isActive())
        m_soon->start();
}

void StockSync::incoming()
{
    while (m_sock && m_sock->hasPendingDatagrams()) {
        const QNetworkDatagram d = m_sock->receiveDatagram();
        const QByteArray data = d.data();
        if (data.size() > kMaxDatagram)
            continue;
        const QJsonObject root = QJsonDocument::fromJson(data).object();
        if (root.value(QStringLiteral("from")).toString() == m_self)
            continue;                                   // our own broadcast
        // Not one of ours: no answer either, or any stray packet on this port
        // makes every copy broadcast its numbers.
        if (root.value(QStringLiteral("app")).toString() != QLatin1String(kApp)
            || root.value(QStringLiteral("v")).toInt() != 1)
            continue;
        const bool changed = merge(data);
        if (changed)
            emit this->changed();
        // The sender may be behind: tell it what we have, at most every 2 s
        // (at once when pooling a fresh install's Collections left it so).
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (m_tellPeers) {
            announceSoon();
        } else if (!changed && now - m_lastReply > 2000) {
            m_lastReply = now;
            announceSoon();
        }
    }
}

void StockSync::setAsInstance()
{
    setStockChangedHook([this] { announceSoon(); });
}

} // namespace c2d
