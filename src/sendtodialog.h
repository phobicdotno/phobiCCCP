#pragma once
#include <QDialog>
#include <QJsonObject>
#include <QList>
#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QUdpSocket;

namespace c2d {

// File > Send to phobiCCCP…: sends the open design, whole, to phobiCCCP on
// another computer (the one by the machine), which opens it and, if asked
// and allowed there, starts cutting. This is how the pad sends a design to
// the shop PC; it works between two computers just the same.
//
// The other side is the pad link of projectreceiver.h: computers are found
// with the discovery datagram, then one API connection per request: "info"
// for its machines, "open" with the .c2d as base64.
class SendToDialog : public QDialog
{
    Q_OBJECT
public:
    // `makeCopy` writes the design as it is now to a .c2d and returns its
    // path, or an empty string with the reason. `ownId` is this install's
    // id, so it does not list itself.
    SendToDialog(const QString &name, const QString &ownId,
                 std::function<QString(QString *error)> makeCopy, QWidget *parent = nullptr);

    // The biggest design that fits in one request: base64 makes it 4/3 the
    // size, and the request (ProjectReceiver::kMaxRequest, 16 MiB) also
    // carries the JSON around it.
    static constexpr qint64 kMaxDesign = (16LL * 1024 * 1024 - 64 * 1024) / 4 * 3;

    // A phobiCCCP found on the network. `pad`: it runs on a pad (Android or
    // iPad), so it has no machine of its own.
    struct Computer {
        QString id, host, address, machine;
        quint16 port = 0;
        bool pad = false;
    };
    // Send the discovery datagram from `udp` to every broadcast address.
    static void discover(QUdpSocket *udp);
    // The phobiCCCPs among the answers waiting on `udp`.
    static QList<Computer> answers(QUdpSocket *udp);
    // One request on its own connection, owned by `owner`; `done` gets the
    // reply, or {"ok": false, "error": …}, unless `owner` is gone by then.
    static void call(QObject *owner, const QString &address, quint16 port, const QJsonObject &req,
                     int timeoutMs, std::function<void(const QJsonObject &)> done);

private:
    void onDatagrams();
    void addComputer(const Computer &c);
    void addByAddress();
    void selectComputer();
    void send();
    void setBusy(bool busy);

    QString m_name, m_ownId;
    std::function<QString(QString *)> m_makeCopy;
    QList<Computer> m_computers;
    QUdpSocket *m_udp = nullptr;
    QListWidget *m_list = nullptr;
    QLineEdit *m_address = nullptr;
    QComboBox *m_machine = nullptr;
    QCheckBox *m_start = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_send = nullptr;
    int m_infoSerial = 0;   // only the newest "info" answer is shown
    bool m_busy = false;
};

} // namespace c2d
