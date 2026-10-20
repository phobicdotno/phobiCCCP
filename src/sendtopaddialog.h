#pragma once
#include "padlink.h"
#include "sendtodialog.h"

#include <QDialog>
#include <functional>

class QLabel;
class QListWidget;
class QPushButton;
class QUdpSocket;

namespace c2d {

// File > Send to pad…: the open design goes to a pad, and opens there to be
// edited. Two kinds of pad are listed:
//
// - the PhobiCCCP pad app (Android/), in touch with this computer by its
//   sync rounds. It is never connected to: the design waits here
//   (padlink::Pads) until the pad's next round, a few seconds at most, takes
//   it. Edits made there come back to the file as any pad edit does.
// - this program running on a pad (qt-android/), found by discovery like
//   File > Send to phobiCCCP… finds it, and sent the whole file ("open").
//   It is sent back from there the same way.
class SendToPadDialog : public QDialog
{
    Q_OBJECT
public:
    struct Hooks {
        // The pad apps in touch lately.
        std::function<QList<padlink::Pads::Pad>()> pads;
        // Send the design to that pad app: the request's number, or 0 with
        // `error` set. `note` gets what the pad will not hold, if anything.
        std::function<qint64(const QString &padId, QString *note, QString *error)> send;
        // Still waiting for that pad app to take request `seq`.
        std::function<bool(const QString &padId, qint64 seq)> waiting;
        // A copy of the design as a .c2d, for a pad running this program; its
        // path, or empty with `error` set.
        std::function<QString(QString *error)> makeCopy;
        QString ownId;   // this install's id, so it does not list itself
    };
    SendToPadDialog(const QString &name, bool receiving, Hooks hooks, QWidget *parent = nullptr);

private:
    struct Row {
        bool app = true;   // the pad app; else this program on a pad
        QString id, name, address;
        quint16 port = 0;
    };
    void refresh();
    void send();
    void sendWhole(const Row &r);
    void tick();
    void idle();

    QString m_name;
    bool m_receiving = false;
    Hooks m_hooks;
    QList<Row> m_rows;
    QList<SendToDialog::Computer> m_found;   // this program on pads
    QUdpSocket *m_udp = nullptr;
    QListWidget *m_list = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_send = nullptr;
    QString m_padId, m_padName, m_note;
    qint64 m_seq = 0;
    qint64 m_sentAt = 0;
    bool m_busy = false;
    bool m_emptyShown = false;   // the status says no pad is listed
    int m_ticks = 0;
};

} // namespace c2d
