#pragma once
#include "padlink.h"

#include <QDialog>
#include <functional>

class QLabel;
class QListWidget;
class QPushButton;
class QTimer;

namespace c2d {

// File > Send to pad…: the open design goes to a PhobiCCCP pad app that is
// in touch with this computer, and opens there to be edited. Edits made on
// the pad come back to the file as any pad edit does (padlink.h).
//
// A pad is never connected to: the design waits here (padlink::Pads) until
// the pad's next sync round, a few seconds at most, takes it.
class SendToPadDialog : public QDialog
{
    Q_OBJECT
public:
    struct Hooks {
        // The pads in touch lately.
        std::function<QList<padlink::Pads::Pad>()> pads;
        // Send the design to that pad: the request's number, or 0 with
        // `error` set. `note` gets what the pad will not hold, if anything.
        std::function<qint64(const QString &padId, QString *note, QString *error)> send;
        // Still waiting for that pad to take request `seq`.
        std::function<bool(const QString &padId, qint64 seq)> waiting;
    };
    SendToPadDialog(const QString &name, bool receiving, Hooks hooks, QWidget *parent = nullptr);

private:
    void refresh();
    void send();
    void tick();

    QString m_name;
    Hooks m_hooks;
    QList<padlink::Pads::Pad> m_shown;
    QListWidget *m_list = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_send = nullptr;
    QString m_padId, m_padName, m_note;
    qint64 m_seq = 0;
    qint64 m_sentAt = 0;
};

} // namespace c2d
