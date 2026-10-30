#pragma once
#include <QHostAddress>
#include <QObject>
#include <QString>

class QSettings;
class QTimer;
class QUdpSocket;

namespace c2d {

// Keeps the bit stock (In stock / broken per bit) and the Collections the same
// on every phobiCCCP copy on the local network: the laptop, the CNC PC, vega.
// Each copy announces its numbers by UDP broadcast at start, 0.5 s after a
// change and every 30 s; a copy that hears newer numbers takes them. The
// newest change per bit wins (and the newest edit of the Collections as a
// whole). No server, no internet, no login: anyone on the same network can
// send these packets, so only numbers in range are taken and nothing but
// stock and collections can be set this way.
class StockSync : public QObject
{
    Q_OBJECT
public:
    static constexpr quint16 kPort = 47233;
    // settingsFile empty: the app's own settings. Tests pass an .ini path and a
    // target of their own so two copies can talk inside one process.
    explicit StockSync(const QString &settingsFile = QString(), quint16 listenPort = kPort,
                       const QHostAddress &target = QHostAddress::Broadcast,
                       quint16 targetPort = kPort, QObject *parent = nullptr);

    bool start();
    void announce();                       // send everything now
    QByteArray payload() const;
    bool merge(const QByteArray &payload); // true: something here changed

    // The app's one running sync: stamps from stockstamp.h make it announce.
    void setAsInstance();

signals:
    void changed();                        // numbers or collections changed by a peer

private:
    QSettings *open() const;
    void incoming();
    void announceSoon();

    QString m_file;
    quint16 m_listen, m_targetPort;
    QHostAddress m_target;
    QUdpSocket *m_sock = nullptr;
    QTimer *m_soon = nullptr;
    QString m_self;
    qint64 m_lastReply = 0;
};

} // namespace c2d
