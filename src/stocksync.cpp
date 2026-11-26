#include "stocksync.h"

#include "stockstamp.h"
#include "toolcollections.h"
#include "toollibrary.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
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

quint16 StockSync::localPort() const
{
    return m_sock ? m_sock->localPort() : 0;
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
    // port could otherwise fill the settings with numbers 1..99999. The
    // library changes only when ToolSync writes into its synced copy: read
    // again when index.json or any folder of it changes (a file put back
    // with the index as it was changes its folder's time). A handful of
    // stats per packet.
    const QString dir = ToolLibrary::cacheDir();
    QString key;
    const QFileInfo ix(dir + QStringLiteral("/index.json"));
    if (ix.exists())
        key = QStringLiteral("%1/%2").arg(ix.lastModified().toMSecsSinceEpoch()).arg(ix.size());
    const QFileInfo top(dir);
    if (top.isDir()) {
        key += QStringLiteral(";%1").arg(top.lastModified().toMSecsSinceEpoch());
        for (const QFileInfo &d : QDir(dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
            key += QStringLiteral(";%1=%2").arg(d.fileName()).arg(d.lastModified().toMSecsSinceEpoch());
    }
    if (!m_knownLoaded || key != m_knownIndex) {
        ToolLibrary lib;
        QSet<int> known;
        if (lib.loadDefault())
            for (const LibraryTool &t : lib.tools())
                if (t.number > 0)
                    known.insert(t.number);
        m_known = known;
        m_knownIndex = key;
        m_knownLoaded = true;
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
// `small`: without the stamps, for when the full form does not fit in a
// datagram. A current copy then takes it as an older copy's whole set, which
// it is: our newest set at our whole-set stamp.
static QJsonObject collectionsPayload(QSettings &s, bool small = false)
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
    const bool fresh = s.value(QLatin1String(kFresh), false).toBool();
    coll.insert(QStringLiteral("json"), QString::fromUtf8(json));
    if (fresh) {
        coll.insert(QStringLiteral("fresh"), true);
        // Pooling needs no stamps: an empty list keeps it a current-copy packet.
        coll.insert(QStringLiteral("items"),
                    small ? QJsonObject() : stampsJson(CollectionStamps::fromSettings(s).ts));
        return coll;
    }
    coll.insert(QStringLiteral("ts"), static_cast<double>(cts));
    if (!small) {
        const CollectionStamps st = CollectionStamps::fromSettings(s);
        coll.insert(QStringLiteral("items"), stampsJson(st.ts));
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
    // ,"stock":{} and ,"part":N with some to spare: each estimate below is
    // at least the size the entry takes in the compact JSON.
    constexpr int kStockFrame = 40;
    // The Collections in the full form if that fits, else without the
    // stamps; nothing only when neither fits.
    root.insert(QStringLiteral("coll"), collectionsPayload(*s));
    if (jsonSize(root) + kStockFrame > kMaxDatagram)
        root.insert(QStringLiteral("coll"), collectionsPayload(*s, true));
    if (jsonSize(root) + kStockFrame > kMaxDatagram)
        root.insert(QStringLiteral("coll"), QJsonObject());

    QList<QByteArray> out;
    QJsonObject stock;
    int size = jsonSize(root) + kStockFrame;
    auto flush = [&] {
        QJsonObject full = root;
        full.insert(QStringLiteral("part"), int(out.size()));
        full.insert(QStringLiteral("stock"), stock);
        out.append(QJsonDocument(full).toJson(QJsonDocument::Compact));
        root.insert(QStringLiteral("coll"), QJsonObject());   // in the first one only
        stock = QJsonObject();
        size = jsonSize(root) + kStockFrame;
    };
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
            // Does not fit: this packet goes as it is (the first may hold
            // only the Collections) and the entry starts the next one, where
            // it always fits. Each part says it is one, so a receiver does
            // not take bits missing from one part as bits we lack.
            if (size + add > kMaxDatagram)
                flush();
            stock.insert(n, o);
            size += add;
        }
        s->endGroup();
    }
    s->endGroup();
    if (!out.isEmpty())
        root.insert(QStringLiteral("part"), int(out.size()));
    root.insert(QStringLiteral("stock"), stock);
    out.append(QJsonDocument(root).toJson(QJsonDocument::Compact));
    // The estimates above are upper bounds; this only guards the promise.
    QList<QByteArray> sendable;
    for (const QByteArray &p : out)
        if (p.size() <= kMaxDatagram)
            sendable.append(p);
    return sendable;
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

// Deletion stamps from a packet: in range, at most as many as a sender keeps
// (CollectionStamps::kMaxGone, which it prunes to before sending). Any name
// the Collections reader takes is a name here too.
static QHash<QString, qint64> readGone(const QJsonObject &o, qint64 limit)
{
    QHash<QString, qint64> out;
    for (auto it = o.constBegin(); it != o.constEnd() && out.size() < CollectionStamps::kMaxGone;
         ++it) {
        const qint64 t = jsonStamp(it.value());
        if (it.key().isEmpty() || t <= 0 || t > limit)
            continue;
        out.insert(it.key(), t);
    }
    return out;
}

// The same bits, in whatever order.
static bool sameBits(const QList<int> &a, const QList<int> &b)
{
    return a.size() == b.size()
        && QSet<int>(a.cbegin(), a.cend()) == QSet<int>(b.cbegin(), b.cend());
}

// The same collections with the same bits (order aside).
static bool sameSet(const ToolCollections &a, const ToolCollections &b)
{
    if (a.all().size() != b.all().size())
        return false;
    for (const ToolCollection &c : a.all()) {
        const ToolCollection *o = b.byName(c.name);
        if (!o || !sameBits(c.tools, o->tools))
            return false;
    }
    return true;
}

// A peer's Collections into ours (settings `s`). True when ours changed.
// *tell when the peer is behind: it would take something of ours (by the
// rules it merges with), so it is worth answering.
static bool mergeCollections(QSettings &s, const QJsonObject &coll, bool whole, qint64 limit,
                             bool *tell)
{
    const qint64 lts = s.value(QLatin1String(kCollTs), 0).toLongLong();
    const bool lfresh = lts > 0 && s.value(QLatin1String(kFresh), false).toBool();
    if (coll.isEmpty()) {
        // A packet with all a copy has and no Collections: it has none yet,
        // and takes a set with history whole.
        if (whole && lts > 0 && !lfresh)
            *tell = true;
        return false;
    }
    const bool pfresh = coll.value(QStringLiteral("fresh")).toBool();
    // No "ts" (a fresh copy): 0. One that is not a sane stamp: refused.
    const qint64 pts = coll.contains(QStringLiteral("ts")) ? jsonStamp(coll.value(QStringLiteral("ts"))) : 0;
    const QByteArray pj = coll.value(QStringLiteral("json")).toString().toUtf8();
    if (pj.size() >= 50000 || pts < 0 || pts > limit || (!pfresh && pts == 0))
        return false;
    // Only what the Collections reader takes: any other JSON object ("{}")
    // read back as nothing and showed the two built-in lists instead of the
    // user's own, which the next edit then saved over them.
    ToolCollections peer;
    if (!ToolCollections::fromJson(pj, &peer))
        return false;

    const QByteArray lj = s.value(QLatin1String(kCollKey)).toByteArray();
    ToolCollections local;
    if (lj.isEmpty() || !ToolCollections::fromJson(lj, &local))
        local = ToolCollections::defaults();
    const CollectionStamps mine = CollectionStamps::fromSettings(s);

    // The peer's stamps: per collection from a current copy; an older copy
    // sends its Collections as one, newer replacing all of ours.
    CollectionStamps theirs;
    if (coll.contains(QStringLiteral("items"))) {
        // One stamp per collection in the list, looked up by its name: as
        // many as the list holds, whatever the names.
        const QJsonObject items = coll.value(QStringLiteral("items")).toObject();
        for (const ToolCollection &c : peer.all()) {
            const qint64 t = jsonStamp(items.value(c.name));
            if (t > 0 && t <= limit)
                theirs.ts.insert(c.name, t);
        }
        if (!pfresh)
            theirs.gone = readGone(coll.value(QStringLiteral("gone")).toObject(), limit);
    } else {
        if (!lfresh && lts > 0 && pts <= lts) {
            // An older copy takes only a strictly newer whole set.
            if (pts < lts)
                *tell = true;
            return false;
        }
        for (const ToolCollection &c : peer.all())
            theirs.ts.insert(c.name, pts);
        if (!lfresh)
            for (const ToolCollection &c : local.all())
                if (!peer.byName(c.name))
                    theirs.gone.insert(c.name, pts);
    }

    // Nothing of our own yet: theirs as they are (a fresh peer's stay its own
    // until it has merged with another copy).
    if (lts == 0) {
        if (pfresh)
            return false;
        s.setValue(QLatin1String(kCollKey), pj);
        // Never below a stamp kept: the next edit here is stamped after it.
        s.setValue(QLatin1String(kCollTs), static_cast<qint64>(qMax(pts, theirs.latest())));
        theirs.toSettings(s);
        return true;
    }

    ToolCollections result = local;
    CollectionStamps out = mine;
    bool changed = false;
    // Later than every stamp on either side (a peer's clock may run ahead).
    const qint64 now = qMax(QDateTime::currentMSecsSinceEpoch(),
                            qMax(qMax(lts, pts), qMax(mine.latest(), theirs.latest())) + 1);
    bool stamped = false;                  // something stamped `now` here

    if (lfresh || pfresh) {
        // A fresh install meets another copy: neither side's lists replace
        // the other's, the bits and collections are pooled. Once, on this
        // side: from then on the newest edit wins and deletions stick.
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
        if (lfresh)
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
            const bool there = !their || !sameBits(their->tools, c.tools);
            const bool untouchedDefault = c.tools.isEmpty() && out.ts.value(c.name, 0) == 0;
            changed = changed || here;
            if (there && !untouchedDefault && (here || lfresh)) {
                // Pooled here, or a fresh copy's own to give: stamped now
                // so the peer takes it.
                out.ts.insert(c.name, now);
                out.gone.remove(c.name);
                stamped = true;
            } else if (!there) {
                const qint64 t = qMax(out.ts.value(c.name, 0), theirs.ts.value(c.name, 0));
                if (t > 0)
                    out.ts.insert(c.name, t);
                out.gone.remove(c.name);
            }
        }
        // The peer's deletions of collections we do not have are kept, or
        // a late announce from a copy that still has one brings it back
        // once this copy goes by the newest edit.
        for (auto it = theirs.gone.constBegin(); it != theirs.gone.constEnd(); ++it)
            if (!result.byName(it.key()) && it.value() > out.gone.value(it.key(), 0))
                out.gone.insert(it.key(), it.value());
        if (lfresh)
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

    // Is the peer behind? Only what it would take counts: a collection of
    // ours that differs from its copy and is stamped later than its copy and
    // its deletion of it, or a deletion of ours later than its copy. (Not an
    // untouched default, stamped never: that went back and forth for ever.)
    for (const ToolCollection &c : result.all()) {
        const ToolCollection *their = peer.byName(c.name);
        const qint64 t = out.ts.value(c.name, 0);
        if ((!their || !sameBits(their->tools, c.tools)) && t > theirs.ts.value(c.name, 0)
            && t > theirs.gone.value(c.name, 0))
            *tell = true;
    }
    for (auto it = out.gone.constBegin(); it != out.gone.constEnd(); ++it)
        if (peer.byName(it.key()) && it.value() > theirs.ts.value(it.key(), 0))
            *tell = true;

    const bool restamped = out.ts != mine.ts || out.gone != mine.gone;
    if (!changed && !restamped && !stamped && !lfresh)
        return false;
    if (changed)
        s.setValue(QLatin1String(kCollKey), result.toJson());
    // The whole-set stamp: older copies go by it alone. A merge that leaves
    // us with a set the peer does not have is newer than both, or an older
    // copy that already took the peer's set at that stamp never takes ours.
    // A copy that merges this next ends with the same set as ours and does
    // not raise it again.
    // Also when ours changed to exactly the peer's set but our stamp is not
    // below its own: an older copy may hold our old content at our stamp,
    // and would never take the peer's (older) one or ours at that stamp.
    // The next copy to merge this has nothing to change, so it stops there.
    const bool newer = stamped || (changed && (!sameSet(result, peer) || lts >= pts));
    // Never below a stamp kept (one adopted from a fresh packet, which has no
    // whole-set stamp, or from a clock that runs ahead).
    out.prune();
    s.setValue(QLatin1String(kCollTs),
               static_cast<qint64>(qMax(newer ? now : qMax(lts, pts), out.latest())));
    if (newer)
        *tell = true;
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
    bool tell = false;
    // A split announce: each part holds only some of the sender's bits.
    const bool split = root.contains(QStringLiteral("part"));
    const QJsonObject stock = root.value(QStringLiteral("stock")).toObject();
    int seen = 0;
    for (auto it = stock.constBegin(); it != stock.constEnd() && seen < kMaxStockEntries;
         ++it, ++seen) {
        bool ok = false;
        const int number = it.key().toInt(&ok);
        const QJsonObject o = it.value().toObject();
        const qint64 ts = jsonStamp(o.value(QStringLiteral("ts")));
        const int have = o.value(QStringLiteral("have")).toInt(-1);
        const int broken = o.value(QStringLiteral("broken")).toInt(-1);
        if (!ok || number < 1 || number > 99999 || !known.contains(number) || ts <= 0
            || ts > limit || have < 0 || have > 9999 || broken < 0 || broken > 9999)
            continue;
        const QString base = QStringLiteral("toolStock/%1/").arg(number);
        const qint64 ours = s->value(base + QStringLiteral("ts"), 0).toLongLong();
        if (ts < ours)
            tell = true;                   // the sender has older counts
        if (ts <= ours)
            continue;
        s->setValue(base + QStringLiteral("have"), have);
        s->setValue(base + QStringLiteral("broken"), broken);
        s->setValue(base + QStringLiteral("ts"), ts);
        changed = true;
    }
    // A bit the sender has no counts for at all is left to the 30 s
    // announce: its library may not have that bit, and answering for it
    // answered every packet of that copy.
    const bool whole = !split || root.value(QStringLiteral("part")).toInt(-1) == 0;
    bool collTell = false;
    if (mergeCollections(*s, root.value(QStringLiteral("coll")).toObject(), whole, limit, &collTell))
        changed = true;
    // Collections too big for a datagram are not sent: answering for them
    // would only repeat a packet without them, every 2 s for ever.
    if (collTell) {
        QJsonObject mineNow = collectionsPayload(*s);
        if (jsonSize(mineNow) + 256 > kMaxDatagram)
            mineNow = collectionsPayload(*s, true);
        if (!mineNow.isEmpty() && jsonSize(mineNow) + 256 <= kMaxDatagram)
            tell = true;
    }
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
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        // Only when the sender is behind (it would take something of ours),
        // and at most every 2 s: answering every packet had two copies
        // answer each other for ever.
        if (m_tellPeers && now - m_lastReply > 2000) {
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
