#pragma once
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

class QSettings;
class QTimer;
class QUdpSocket;

namespace c2d {

// Keeps the bit stock (In stock / broken per bit) and the Collections the same
// on every phobiCCCP copy on the local network: the laptop, the CNC PC, vega.
// Each copy announces its numbers by UDP broadcast at start, 0.5 s after a
// change and every 30 s; a copy that hears newer numbers takes them. The
// newest change per bit wins, and per collection the newest edit or deletion.
// A fresh install's first Collections only add to a peer's (and the peer's to
// them) until the two have met, so they never replace a richer set. No
// server, no internet, no login: anyone on the same network can send these
// packets, so only bits the tool library knows, with numbers in range, are
// taken and nothing but stock and collections can be set this way.
class StockSync : public QObject
{
    Q_OBJECT
public:
    static constexpr quint16 kPort = 47233;
    static constexpr int kMaxDatagram = 60000;   // bigger packets are neither sent nor read
    static constexpr int kMaxStockEntries = 2000; // bits looked at per packet
    // settingsFile empty: the app's own settings. Tests pass an .ini path and a
    // target of their own so two copies can talk inside one process.
    explicit StockSync(const QString &settingsFile = QString(), quint16 listenPort = kPort,
                       const QHostAddress &target = QHostAddress::Broadcast,
                       quint16 targetPort = kPort, QObject *parent = nullptr);

    bool start();
    void announce();                       // send everything now
    // What announce() sends: one datagram, or more when the stock does not
    // fit in kMaxDatagram (the Collections ride in the first).
    QList<QByteArray> payloads() const;
    QByteArray payload() const;            // the first of payloads()
    bool merge(const QByteArray &payload); // true: something here changed

    // The app's one running sync: stamps from stockstamp.h make it announce.
    void setAsInstance();

signals:
    void changed();                        // numbers or collections changed by a peer

private:
    QSettings *open() const;
    void incoming();
    void announceSoon();
    const QSet<int> &knownTools() const;

    QString m_file;
    quint16 m_listen, m_targetPort;
    QHostAddress m_target;
    QUdpSocket *m_sock = nullptr;
    QTimer *m_soon = nullptr;
    QString m_self;
    qint64 m_lastReply = 0;
    bool m_tellPeers = false;              // the last merge saw the sender is behind us
    mutable QSet<int> m_known;             // tool numbers of the library
    mutable bool m_knownLoaded = false;
    mutable QString m_knownIndex;          // the synced index.json it was read with (time, size)
};

} // namespace c2d
