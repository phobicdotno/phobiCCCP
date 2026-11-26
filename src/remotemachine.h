#pragma once
#include "projectreceiver.h"   // MulticastHold
#include "sendtodialog.h"      // SendToDialog::Computer, discovery

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QWidget>
#include <functional>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTimer;
class QUdpSocket;

namespace c2d {

// The Machine page on a pad. A pad has no serial port, so instead of the
// desktop's machine panel it shows the machine of a phobiCCCP on the
// network, as the Flutter pad app does (Android/lib/ui/machine_page.dart):
// the computers found, their machines and what the machine is doing, and
// Send / Send and start for the design open here.
//
// It speaks the pad link of projectreceiver.h ("PHOBICCCP-API 1"), with the
// requests MainWindow::onPadRequest answers: "info" every 1.5 s, "machine" to
// switch machines, and "open" with the design whole (as File > Send to
// phobiCCCP does), which starts cutting only when asked and when that computer
// lets pads start (Machine > Pads may start cutting there). There is no remote
// jog, hold or stop: the link has no such request.

// The machine status in an "info" answer (MachinePanel::statusJson), in the
// pad's words.
namespace remotestatus {
QString words(const QJsonObject &status);
// What the computer checks before it starts a job from a pad.
bool readyToCut(const QJsonObject &status);
// The heading over the job's steps: RUNNING / FINISHED / STOPPED / LAST JOB ON <machine>.
QString jobHeading(const QJsonObject &status, const QString &machine);
} // namespace remotestatus

// The client side, no widgets: the computers, their last "info", and the
// requests. Testable on the desktop.
class RemoteMachine : public QObject
{
    Q_OBJECT
public:
    static constexpr int kPollMs = 1500;
    static constexpr int kDiscoverMs = 3000;
    static constexpr int kInfoTimeoutMs = 4000;
    // Someone at the computer may be asked to save their own work first.
    static constexpr int kSendTimeoutMs = 5 * 60 * 1000;
    // No answer for this long: shown as not answering.
    static constexpr int kStaleMs = 6000;

    struct Computer {
        QString id, host, address;
        quint16 port = 0;
        bool typed = false;      // added by address here (kept in the settings)
        QJsonObject info;        // the last "info" answer; empty until one came
        qint64 answeredAt = 0;   // when it last answered (ms since 1970)
        QString error;           // why the last "info" failed
        bool asking = false;     // an "info" is on its way
        QString key() const { return QStringLiteral("%1:%2").arg(address).arg(port); }
        QString name() const { return host.isEmpty() ? address : host; }
    };

    // `ownId`: this install's id (MainWindow::instanceId), so it never lists itself.
    explicit RemoteMachine(const QString &ownId, QObject *parent = nullptr);

    // Look for computers and ask them for their status, until stop().
    void start();
    void stop();
    bool isRunning() const { return m_running; }
    // Ask the network again now, on a new socket (after the app was in the
    // background, where Android may have taken the old one away).
    void rediscover();

    const QList<Computer> &computers() const { return m_computers; }
    // "shop-pc", "192.168.1.20" or "host:port": added (and remembered) unless
    // it is listed already. Its index, or -1 for an empty text.
    int addAddress(const QString &text);
    // A typed-in computer, no longer kept.
    void forget(int index);
    // A discovery answer (pads and this install are left out).
    void addFound(const SendToDialog::Computer &c);
    void select(int index);
    int selected() const { return m_selected; }
    const Computer *current() const;
    bool online(int index) const;

    // Ask now instead of at the next tick.
    void pollNow();
    // Switch the selected computer to machine `id`; machineSwitched() tells.
    void switchMachine(const QString &id);
    // Send the design (the bytes of a .c2d) to the selected computer's
    // machine in use; with `start`, ask it to start cutting. False (and
    // sent() is not emitted) when there is no computer or a send is on its way.
    bool send(const QString &name, const QByteArray &c2d, bool start);
    bool isSending() const { return m_sending; }

signals:
    void changed();
    void machineSwitched(bool ok, const QString &message);
    // `message` is what to tell the user (done, refused, or not known).
    void sent(bool ok, bool started, const QString &message);

private:
    void tick();
    void discoverNow();
    void ask(int index);
    int indexOf(const QString &key) const;
    void saveTyped() const;
    void onAppState(Qt::ApplicationState state);

    QString m_ownId;
    QList<Computer> m_computers;
    int m_selected = -1;
    QString m_selectedKey;   // remembered, so the same computer is picked next time
    QUdpSocket *m_udp = nullptr;
    QTimer *m_poll = nullptr;
    QTimer *m_discover = nullptr;
    std::unique_ptr<MulticastHold> m_multicast;   // while running
    bool m_running = false;
    bool m_heard = false;    // a discovery answer came (else rounds also sweep)
    bool m_sending = false;
    bool m_paused = false;   // the app is in the background
    int m_ticks = 0;         // polls
    int m_rounds = 0;        // discovery rounds
};

// The page itself. MainWindow shows it in place of the machine panel on a pad.
class RemoteMachinePanel : public QWidget
{
    Q_OBJECT
public:
    // What the page needs of the design open here.
    struct Design {
        std::function<QString()> name;             // "Untitled" when not saved
        std::function<int()> toolpaths;            // how many there are
        std::function<QString(QString *error)> makeCopy;   // a .c2d of it as it is now
    };
    RemoteMachinePanel(const QString &ownId, Design design, QWidget *parent = nullptr);
    RemoteMachine *client() const { return m_client; }

protected:
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;

private:
    void refresh();
    void refreshComputers();
    void refreshMachines(const RemoteMachine::Computer *c);
    void refreshSteps(const RemoteMachine::Computer *c, const QString &machine);
    void addTyped();
    void send(bool start);

    Design m_design;
    RemoteMachine *m_client = nullptr;
    QListWidget *m_list = nullptr;        // computers
    QLineEdit *m_address = nullptr;
    QPushButton *m_forget = nullptr;
    QLabel *m_status = nullptr;           // the computer's status words
    QListWidget *m_machines = nullptr;
    QLabel *m_allow = nullptr;
    QLabel *m_job = nullptr;              // the design open here
    QLabel *m_toolpaths = nullptr;
    QLabel *m_to = nullptr;
    QLabel *m_warn = nullptr;
    QLabel *m_stepsHead = nullptr;
    QListWidget *m_steps = nullptr;
    QLabel *m_result = nullptr;           // what came of the last send or switch
    QPushButton *m_start = nullptr;
    QPushButton *m_send = nullptr;
    // What the lists show, so they are rebuilt only when it changes (a
    // rebuild under the finger would lose the tap).
    QJsonArray m_shownMachines, m_shownSteps;
    QString m_shownKey;
};

} // namespace c2d
