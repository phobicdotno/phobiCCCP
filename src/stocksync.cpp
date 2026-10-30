#include "stocksync.h"

#include "stockstamp.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QSettings>
#include <QTimer>
#include <QUdpSocket>
#include <QUuid>
#include <memory>

namespace c2d {

static const char *kCollKey = "tools/collections";
static const char *kCollTs = "tools/collectionsTs";
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

QByteArray StockSync::payload() const
{
    std::unique_ptr<QSettings> s(open());
    s->sync();
    QJsonObject stock;
    s->beginGroup(QStringLiteral("toolStock"));
    for (const QString &n : s->childGroups()) {
        s->beginGroup(n);
        const qint64 ts = s->value(QStringLiteral("ts"), 0).toLongLong();
        if (ts > 0) {
            QJsonObject o;
            o.insert(QStringLiteral("have"), s->value(QStringLiteral("have"), 0).toInt());
            o.insert(QStringLiteral("broken"), s->value(QStringLiteral("broken"), 0).toInt());
            o.insert(QStringLiteral("ts"), ts);
            stock.insert(n, o);
        }
        s->endGroup();
    }
    s->endGroup();
    QJsonObject coll;
    const qint64 cts = s->value(QLatin1String(kCollTs), 0).toLongLong();
    if (cts > 0) {
        coll.insert(QStringLiteral("ts"), cts);
        coll.insert(QStringLiteral("json"),
                    QString::fromUtf8(s->value(QLatin1String(kCollKey)).toByteArray()));
    }
    QJsonObject root;
    root.insert(QStringLiteral("app"), QLatin1String(kApp));
    root.insert(QStringLiteral("v"), 1);
    root.insert(QStringLiteral("from"), m_self);
    root.insert(QStringLiteral("stock"), stock);
    root.insert(QStringLiteral("coll"), coll);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool StockSync::merge(const QByteArray &data)
{
    if (data.size() > 60000)
        return false;
    const QJsonObject root = QJsonDocument::fromJson(data).object();
    if (root.value(QStringLiteral("app")).toString() != QLatin1String(kApp)
        || root.value(QStringLiteral("v")).toInt() != 1)
        return false;
    const qint64 limit = QDateTime::currentMSecsSinceEpoch() + 24LL * 3600 * 1000;
    std::unique_ptr<QSettings> s(open());
    s->sync();
    bool changed = false;
    const QJsonObject stock = root.value(QStringLiteral("stock")).toObject();
    for (auto it = stock.constBegin(); it != stock.constEnd(); ++it) {
        bool ok = false;
        const int number = it.key().toInt(&ok);
        const QJsonObject o = it.value().toObject();
        const qint64 ts = static_cast<qint64>(o.value(QStringLiteral("ts")).toDouble());
        const int have = o.value(QStringLiteral("have")).toInt(-1);
        const int broken = o.value(QStringLiteral("broken")).toInt(-1);
        if (!ok || number < 1 || number > 99999 || ts <= 0 || ts > limit
            || have < 0 || have > 9999 || broken < 0 || broken > 9999)
            continue;
        const QString base = QStringLiteral("toolStock/%1/").arg(number);
        if (ts <= s->value(base + QStringLiteral("ts"), 0).toLongLong())
            continue;
        s->setValue(base + QStringLiteral("have"), have);
        s->setValue(base + QStringLiteral("broken"), broken);
        s->setValue(base + QStringLiteral("ts"), ts);
        changed = true;
    }
    const QJsonObject coll = root.value(QStringLiteral("coll")).toObject();
    const qint64 cts = static_cast<qint64>(coll.value(QStringLiteral("ts")).toDouble());
    const QByteArray cj = coll.value(QStringLiteral("json")).toString().toUtf8();
    if (cts > 0 && cts <= limit && cts > s->value(QLatin1String(kCollTs), 0).toLongLong()
        && QJsonDocument::fromJson(cj).isObject() && cj.size() < 50000) {
        s->setValue(QLatin1String(kCollKey), cj);
        s->setValue(QLatin1String(kCollTs), cts);
        changed = true;
    }
    s->sync();
    return changed;
}

void StockSync::announce()
{
    if (!m_sock)
        return;
    m_sock->writeDatagram(payload(), m_target, m_targetPort);
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
        if (data.size() > 60000)
            continue;
        const QJsonObject root = QJsonDocument::fromJson(data).object();
        if (root.value(QStringLiteral("from")).toString() == m_self)
            continue;                                   // our own broadcast
        const bool changed = merge(data);
        if (changed)
            emit this->changed();
        // The sender may be behind: tell it what we have, at most every 2 s.
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (!changed && now - m_lastReply > 2000 && payload() != QByteArray()) {
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
