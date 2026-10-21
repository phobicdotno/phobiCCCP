// The Machine panel against a scripted controller on a pseudo-terminal:
// Cancel job and Disconnect while the hold settles - a Stop in that wait,
// and the job's last lines answered in it (as GRBL acks lines into its
// planner while held), Resume pressed in it; and a BitZero step left
// waiting when an end stop resets the controller. Popups (shopAsk) are answered by a timer that
// reads them and presses a button by its text.
//
// Skips (exit 77) where no pty can be opened as a serial port.
//
// Plain asserts, no framework; exits 0 on success.

#include <QApplication>
#include <QButtonGroup>
#include <QAbstractButton>
#include <QDialog>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QComboBox>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QProgressBar>
#include <QStackedWidget>
#include <functional>
#include <memory>
#include "../src/c2ddocument.h"
#include "../src/element.h"
#include "../src/gamepad.h"
#include "../src/gcodeexport.h"
#include "../src/toollibrary.h"
#include "../src/toolstrip.h"
#define private public
#define protected public
#include "../src/bitzeropanel.h"
#include "../src/machinepanel.h"
#undef private
#undef protected

#include <QApplication>
#include <QButtonGroup>
#include <QAbstractButton>
#include <QDialog>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <fcntl.h>
#include <pty.h>
#include <termios.h>
#include <unistd.h>

static int g_checks = 0;
static QWidget *g_panel = nullptr;   // its log is printed on a failure

static void check(bool cond, const char *what)
{
    ++g_checks;
    if (!cond) {
        if (g_panel)
            for (QPlainTextEdit *t : g_panel->findChildren<QPlainTextEdit *>())
                std::fprintf(stderr, "%s\n", qPrintable(t->toPlainText()));
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

// The far end of the pty: Idle at a set machine position (Hold:0 after a
// feed hold, Home for 300 ms and at least two reports after $H), acks every
// line except a dwell when `hangDwell` and a jog when not `ackJogs`. Like GRBL 1.1 it answers G-code
// sent while jogging with error:9, and lines starting with `reject` too.
// `$$` reports a 425 mm machine. With `holdAcks`, every ok from the first G4
// on is held back until release(). A line starting with `alarmOn` answers
// ALARM:1 (an end stop) and locks the controller up like GRBL does: no line
// is read until ctrl-X, after which it boots into Alarm until $H or $X.
// G38.2 touches at machine Z `probeZ`. Everything it receives goes into
// `events`, realtime bytes as "!", "~", "^X" and "JOGCANCEL".
struct FakeGrbl {
    int fd = -1;
    QByteArray line;
    bool held = false;
    bool hangDwell = false;
    bool ackJogs = true;
    bool holdAcks = false;
    int heldOks = 0;
    QByteArray reject;
    QByteArray alarmOn;
    bool locked = false;      // after ALARM:1: deaf until ctrl-X
    bool alarm = false;       // Alarm state: G-code answered error:9
    double probeZ = -50;
    bool running = false;
    QByteArray pins;
    double mx = -100, my = -100, mz = -30;
    // A jog reports Jog for `jogMs` after its line (it does not move the
    // position: tests set that), or until a jog cancel.
    int jogMs = 300;
    QElapsedTimer jogClock, homeClock, holdClock;
    int holdSettleMs = 0;
    int homeReports = 0;      // Home is reported at least this often, however slow the poll
    QByteArray travel = "$130=425.000\r\n$131=425.000\r\n$132=75.000\r\n$27=1.500\r\n";
    QList<QByteArray> events;

    void send(const QByteArray &s) { (void)!::write(fd, s.constData(), size_t(s.size())); }
    QByteArray num(double v) const { return QByteArray::number(v, 'f', 3); }
    bool jogging() const { return jogClock.isValid() && jogClock.elapsed() < jogMs; }
    void release()
    {
        while (heldOks > 0) {
            send("ok\r\n");
            --heldOks;
        }
        holdAcks = false;
    }

    void onRead()
    {
        char buf[512];
        const ssize_t n = ::read(fd, buf, sizeof buf);
        for (ssize_t i = 0; i < n; ++i) {
            const char c = buf[i];
            if (c == '?') {
                const bool home = homeClock.isValid()
                                  && (homeClock.elapsed() < 300 || homeReports > 0);
                if (home && homeReports > 0)
                    --homeReports;
                send("<" + QByteArray(locked || alarm ? "Alarm" : held ? (holdClock.isValid() && holdClock.elapsed() < holdSettleMs ? "Hold:1" : "Hold:0") : home ? "Home" : jogging() ? "Jog" : running ? "Run" : "Idle")
                     + "|MPos:" + num(mx) + ',' + num(my) + ',' + num(mz)
                     + "|FS:0,0" + (pins.isEmpty() ? QByteArray() : "|Pn:" + pins) + "|WCO:0.000,0.000,0.000>\r\n");
            } else if (c == '!') {
                if (!held) holdClock.start();
                held = true;
                events << "!";
            } else if (c == '~') {
                held = false;
                events << "~";
            } else if (uchar(c) == 0x85) {
                jogClock.invalidate();
                events << "JOGCANCEL";
            } else if (c == 0x18) {
                const bool moving = (running && !held) || jogging()
                                    || (held && holdClock.isValid() && holdClock.elapsed() < holdSettleMs);
                running = false;
                if (moving) { send("ALARM:3\r\n"); alarm = true; events << "ALARM3"; }
                held = false;
                jogClock.invalidate();
                homeReports = 0;
                line.clear();
                heldOks = 0;
                holdAcks = false;
                events << "^X";
                alarm = alarm || locked;
                locked = false;
                send("\r\nGrbl 1.1h ['$' for help]\r\n");
            } else if (c == '\n') {
                events << line;
                if (locked) {                        // read by nobody
                    line.clear();
                    continue;
                }
                QByteArray reply = "ok\r\n";
                if (!alarmOn.isEmpty() && line.startsWith(alarmOn)) {
                    alarmOn.clear();
                    locked = true;
                    send("ALARM:1\r\n[MSG:Reset to continue]\r\n");
                    line.clear();
                    continue;
                }
                if (line == "$$")
                    send(travel);
                else if (line == "$H") {
                    homeClock.start();
                    homeReports = 2;
                    alarm = false;
                } else if (line == "$X")
                    alarm = false;
                else if (alarm && !line.isEmpty() && line[0] != '$')
                    reply = "error:9\r\n";
                else if (line.contains("G38.2") || line.contains("G38.3"))
                    send("[PRB:" + num(mx) + "," + num(my) + "," + num(probeZ) + ":1]\r\n");
                else if (line.startsWith("$J="))
                    jogClock.start();
                else if (!line.isEmpty() && line[0] != '$'
                         && (jogging() || (!reject.isEmpty() && line.startsWith(reject))))
                    reply = "error:9\r\n";
                const bool hang = (hangDwell && line.startsWith("G4"))
                                  || (!ackJogs && line.startsWith("$J="));
                if (!hang) {
                    if (holdAcks && (line.startsWith("G4") || heldOks > 0))
                        ++heldOks;
                    else
                        send(reply);
                }
                line.clear();
            } else if (c != '\r' && uchar(c) < 0x80) {   // 0x85 jog cancel &c.
                line += c;
            }
        }
    }
    int count(const QByteArray &prefix) const
    {
        int k = 0;
        for (const QByteArray &e : events)
            k += e.startsWith(prefix);
        return k;
    }
    int indexOf(const QByteArray &prefix, int from = 0) const
    {
        for (int i = from; i < events.size(); ++i)
            if (events.at(i).startsWith(prefix))
                return i;
        return -1;
    }
};

struct Seen {
    QString caption, headline, body;
};

// A design with two pockets cut by two different bits that share number 1.
static void addPocket(c2d::Document &doc, const QString &shape, const QString &uuid,
                      const QString &name, double dia, const QString &bit)
{
    QJsonObject j;
    j.insert("type", QStringLiteral("pocket_toolpath"));
    j.insert("name", name);
    j.insert("enabled", true);
    j.insert("uuid", uuid);
    j.insert("start_depth", QStringLiteral("0.000"));
    j.insert("end_depth", QStringLiteral("1.000"));
    j.insert("stepdown", 1.0);
    j.insert("stepover", dia / 2);
    j.insert("stock_to_leave", 0.0);
    j.insert("speeds", QJsonObject{{"feedrate", 1000}, {"plungerate", 300}, {"rpm", 10000}});
    j.insert("tool", QJsonObject{{"diameter", dia}, {"number", 1}, {"name", bit}});
    j.insert("elements", QJsonArray{QJsonObject{{"uuid", shape}}});
    c2d::Toolpath tp;
    tp.uuid = uuid;
    tp.type = QStringLiteral("pocket_toolpath");
    tp.json = j;
    doc.addToolpath(tp);
}

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("phobicccp-test"));
    QApplication::setApplicationName(QStringLiteral("test_machinestop_") + QString::fromLatin1(qgetenv("R4MODE")));
    QSettings().clear();

    // ---- which bit a tool change wants (no machine needed) ----------------
    c2d::Document doc;
    const c2d::Element rect = c2d::Element::makeRectangle({30, 20}, 40, 20, QJsonObject());
    doc.addElement(rect);
    addPocket(doc, rect.id, QStringLiteral("{tp-1}"), QStringLiteral("big"), 6.35,
              QStringLiteral("1/4 endmill"));
    addPocket(doc, rect.id, QStringLiteral("{tp-2}"), QStringLiteral("small"), 3.175,
              QStringLiteral("1/8 endmill"));
    {
        const c2d::GcodeResult g = c2d::exportGcode(doc);
        const QVector<c2d::Op> changes = c2d::toolChanges(g.ops);
        check(changes.size() == 2 && g.gcode.count(QStringLiteral("M0 ;T1")) == 2,
              "bits: one pause per bit, both numbered 1");
        check(c2d::toolForChange(doc, changes.at(0)).name == QLatin1String("1/4 endmill"),
              "bits: the first change wants the 1/4");
        const c2d::LibraryTool second = c2d::toolForChange(doc, changes.at(1));
        check(second.name == QLatin1String("1/8 endmill") && std::abs(second.diameter - 3.175) < 1e-9,
              "bits: the second change wants the 1/8, not the first bit with that number");
    }

    // ---- the panel on a fake controller -----------------------------------
    int master = -1, slave = -1;
    char name[256] = {};
    if (openpty(&master, &slave, name, nullptr, nullptr) != 0) {
        std::printf("test_machinestop: no pty, skipped\n");
        return 77;
    }
    termios tio;
    tcgetattr(slave, &tio);
    cfmakeraw(&tio);
    tcsetattr(slave, TCSANOW, &tio);
    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
    FakeGrbl grbl;
    grbl.fd = master;
    QSocketNotifier notifier(master, QSocketNotifier::Read);
    QObject::connect(&notifier, &QSocketNotifier::activated, [&] { grbl.onRead(); });

    // Homed as far as the panel can tell: the controller stands exactly
    // where the last homed session left it.
    QSettings().setValue(QStringLiteral("machine/homedAt"),
                         QVariantList{grbl.mx, grbl.my, grbl.mz});
    QSettings().setValue(QStringLiteral("machine/port"), QString::fromLatin1(name));
    QSettings().setValue(QStringLiteral("gamepad/6"), QStringLiteral("home"));

    c2d::MachinePanel panel;
    g_panel = &panel;
    panel.resize(1400, 900);
    panel.show();
    panel.setDocument(&doc);
    c2d::GrblStreamer *s = panel.streamer();

    auto button = [&](const QString &text, const QString &tip = QString()) -> QPushButton * {
        for (QPushButton *b : panel.findChildren<QPushButton *>())
            if (b->text() == text && (tip.isEmpty() || b->toolTip().contains(tip)))
                return b;
        std::fprintf(stderr, "no button '%s'\n", qPrintable(text));
        std::exit(1);
    };

    QPushButton *runBtn = button(QStringLiteral("▶  Start"));
    check(runBtn != nullptr, "run button found");

    // Popups: recorded, and answered with the button `answer` names.
    QList<Seen> seen;
    std::function<QString(const Seen &)> answer = [](const Seen &) { return QString(); };
    QTimer answerer;
    answerer.setInterval(30);
    QObject::connect(&answerer, &QTimer::timeout, [&] {
        auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dlg || dlg->objectName() != QLatin1String("shopDialog") || dlg->property("seen").toBool())
            return;
        Seen v;
        for (QLabel *l : dlg->findChildren<QLabel *>()) {
            if (l->objectName() == QLatin1String("caption")) v.caption = l->text();
            if (l->objectName() == QLatin1String("headline")) v.headline = l->text();
            if (l->objectName() == QLatin1String("body")) v.body = l->text();
        }
        dlg->setProperty("seen", true);
        seen << v;
        const QString pick = answer(v);
        for (QPushButton *b : dlg->findChildren<QPushButton *>())
            if (b->text() == pick) {
                QTimer::singleShot(0, b, &QPushButton::click);
                return;
            }
        std::fprintf(stderr, "no answer '%s' to '%s'\n", qPrintable(pick), qPrintable(v.headline));
        std::exit(1);
    });
    answerer.start();

    // Waits are meant for an idle machine; under load (ctest -j, CI) events
    // and status reports come late. A wait for a condition gets four times
    // its nominal time (it returns as soon as the condition holds), and a
    // plain pause lasts until as many status reports as it would see at the
    // nominal 250 ms poll have arrived, so the panel saw what it was meant to.
    auto spin = [&](int ms, const std::function<bool()> &until) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 4 * ms && !until())
            QApplication::processEvents(QEventLoop::AllEvents, 20);
    };
    int reports = 0;
    QObject::connect(s, &c2d::GrblStreamer::statusChanged, [&] { ++reports; });
    auto wait = [&](int ms) {
        const int want = reports + ms / 250;
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms
               || (reports < want && s->isConnected() && t.elapsed() < ms + 10000))
            QApplication::processEvents(QEventLoop::AllEvents, 20);
    };
    auto key = [&](int k, bool press) {
        QKeyEvent e(press ? QEvent::KeyPress : QEvent::KeyRelease, k, Qt::NoModifier);
        QApplication::sendEvent(&panel, &e);
    };
    c2d::Gamepad *pad = panel.findChild<c2d::Gamepad *>();
    check(pad != nullptr, "gamepad found");
    auto padPress = [&](int n) {
        emit pad->buttonChanged(n, true);
        emit pad->buttonChanged(n, false);
    };
    auto lastJog = [&]() -> QByteArray {
        for (int i = grbl.events.size() - 1; i >= 0; --i)
            if (grbl.events.at(i).startsWith("$J="))
                return grbl.events.at(i);
        return {};
    };

    check(s->connectPort(QString::fromLatin1(name)), "connects to the fake controller");
    spin(1500, [&] { return panel.statusJson().value("homed").toBool(); });
    check(panel.statusJson().value("homed").toBool(), "homed (resumed where the last session left it)");
    wait(800);   // $$ answered
    // One scenario per run (ctest runs each): R4MODE = cancelstop, cancelend
    // or disconnend.
    const QByteArray mode = qgetenv("R4MODE");

    if (mode == "cancelstop") {
        // A job runs (a dwell). Cancel job, confirmed; the operator then hits
        // Stop (on screen or the pad's Stop) while the hold settles.
        grbl.hangDwell = true;
        s->startStream({QStringLiteral("M3S1000"), QStringLiteral("G4P3.00"), QStringLiteral("G0X1")});
        wait(400);
        grbl.events.clear();
        answer = [&](const Seen &) { return QStringLiteral("Cancel job"); };
        bool closed = false;
        QTimer::singleShot(0, [&] {
            // wait until the question is gone, then Stop at once
            QTimer *t = new QTimer;
            t->setInterval(5);
            QObject::connect(t, &QTimer::timeout, [&, t] {
                if (QApplication::activeModalWidget() || grbl.count("!") < 1)
                    return;
                t->stop();
                t->deleteLater();
                closed = true;
                QTimer::singleShot(0, [&] {
                    panel.m_stopBtn->click();
                });
            });
            t->start();
        });
        button(QStringLiteral("✕ Cancel job"))->click();
        spin(3000, [&] { return closed; });
        wait(3000);
        grbl.hangDwell = false;
        const int firstReset = grbl.events.indexOf("^X");
        check(firstReset >= 0, "cancel then Stop: the controller is reset");
        check(grbl.indexOf("G53 G0 Z", firstReset) < 0 && !s->isMacroRunning(),
              "cancel then Stop: nothing moves after the Stop");
    }

    if (mode == "cancelend" || mode == "disconnend") {
        // The last lines of a job are still unanswered when the operator
        // cancels (or disconnects); their oks arrive during the hold wait,
        // as real GRBL acks lines into the planner while held.
        const bool disc = mode == "disconnend";
        grbl.holdAcks = true;
        grbl.holdSettleMs = 1500;               // decelerating for 1.5 s
        panel.m_endOfJobPending = true;         // as runProgram sets it
        s->startStream({QStringLiteral("M3S1000"), QStringLiteral("G4P0.5"),
                        QStringLiteral("G1X1F500"), QStringLiteral("G0Z-1")});
        wait(400);
        grbl.events.clear();
        answer = [&](const Seen &) { return disc ? QStringLiteral("Stop and disconnect")
                                                 : QStringLiteral("Cancel job"); };
        bool closed = false;
        QTimer::singleShot(0, [&] {
            QTimer *t = new QTimer;
            t->setInterval(5);
            QObject::connect(t, &QTimer::timeout, [&, t] {
                if (QApplication::activeModalWidget() || grbl.count("!") < 1)
                    return;
                t->stop();
                t->deleteLater();
                closed = true;
                QTimer::singleShot(100, [&] {
                    grbl.release();
                });
            });
            t->start();
        });
        if (disc)
            panel.m_connectBtn->click();
        else
            button(QStringLiteral("✕ Cancel job"))->click();
        spin(3000, [&] { return closed; });
        wait(7000);
        check(grbl.count("^X") >= 1 && !grbl.held,
              disc ? "disconnect at the job's end: the held controller is reset before the port closes"
                   : "cancel at the job's end: the held controller is reset");
        check(!s->isStreaming(), "and nothing streams");
        if (disc)
            check(!s->isConnected(), "disconnect at the job's end: the port closes");
        else
            check(panel.m_pauseBtn->text().contains(QLatin1String("Hold")), "cancel at the job's end: not left on Resume");
    }

    auto runUntilDialogGone = [&](std::function<void()> after) {
        QTimer *t = new QTimer;
        t->setInterval(5);
        QObject::connect(t, &QTimer::timeout, [&, t, after] {
            if (QApplication::activeModalWidget() || grbl.count("!") < 1)
                return;
            t->stop();
            t->deleteLater();
            after();
        });
        t->start();
    };

    if (mode == "bzstale") {
        // BitZero (corner, X Y Z, BitSetter on): the last macro (rise to the
        // top, back over the corner) is acked, bitZeroApply waits for Idle;
        // on the way an end stop fires (watchdog + ALARM:3). Then Initialize.
        panel.m_bsEnabled->setChecked(true);
        panel.m_bzWantX = panel.m_bzWantY = panel.m_bzWantZ = true;
        panel.m_bzCx = -50; panel.m_bzCy = -60; panel.m_bzStockZ = -70;
        grbl.running = true;
        panel.bitZeroApply();
        wait(300);
        grbl.pins = "Z";
        wait(600);
        grbl.pins.clear();
        check(grbl.count("^X") >= 1, "bzstale: the end stop reset the controller");
        grbl.events.clear();
        panel.initializeMachine();
        wait(5000);
        check(grbl.count("G10 L20") == 0 && grbl.count("G38.2") == 0,
              "bzstale: nothing of the failed BitZero runs after Initialize");
    }

    if (mode == "cancelresume") {
        // Cancel job confirmed; the hold is still decelerating when the
        // operator presses Resume (the button still says Resume).
        grbl.hangDwell = true;
        grbl.holdSettleMs = 1500;
        s->startStream({QStringLiteral("M3S1000"), QStringLiteral("G4P30.00"), QStringLiteral("G0X1")});
        wait(400);
        grbl.running = true;
        wait(300);
        grbl.events.clear();
        answer = [&](const Seen &) { return QStringLiteral("Cancel job"); };
        bool closed = false;
        QTimer::singleShot(0, [&] {
            runUntilDialogGone([&] {
                closed = true;
                QTimer::singleShot(100, [&] { panel.m_pauseBtn->click(); });
            });
        });
        button(QStringLiteral("✕ Cancel job"))->click();
        spin(3000, [&] { return closed; });
        wait(7000);
        check(grbl.count("ALARM3") == 0, "cancelresume: no reset while moving");
    }

    if (mode == "safez") {
        // Safe Z typed as 0 (the box allows 0..-1000): every safe-Z move
        // keeps out of the Z switch's trigger zone, not only Park.
        panel.m_bsSafeZ->setValue(0.0);
        grbl.events.clear();
        panel.park();
        wait(600);
        panel.parkForTool();
        wait(600);
        panel.goToXY0();
        wait(600);
        int moves = 0;
        for (const QByteArray &e : grbl.events)
            if (e.startsWith("G53 G0 Z")) {
                ++moves;
                const double z = e.mid(8).toDouble();
                std::printf("sent: %s\n", e.constData());
                check(z <= -panel.switchClearance() + 1e-9, "safez: below the switch clearance");
            }
        check(moves == 3, "safez: park, tool park and X0 Y0 each rise");
    }

    if (mode == "descend") {
        // Safe Z set to machine Z -30; the machine stands higher, at -10
        // (a tall board: the job's retract height is above -30).
        panel.m_bsSafeZ->setValue(-30.0);
        grbl.mz = -10;
        wait(600);
        grbl.events.clear();
        panel.goToXY0();
        wait(600);
        std::printf("goToXY0 from Z-10:\n");
        for (const QByteArray &e : grbl.events) std::printf("  sent: %s\n", e.constData());
        grbl.events.clear();
        panel.m_endOfJobPending = true;
        s->startStream({QStringLiteral("M3S1000"), QStringLiteral("G0X1")});
        wait(1200);
        std::printf("end of job from Z-10:\n");
        for (const QByteArray &e : grbl.events) std::printf("  sent: %s\n", e.constData());
        bool down = false;
        for (const QByteArray &e : grbl.events)
            if (e.startsWith("G53 G0 Z") && e.mid(8).toDouble() < grbl.mz - 1e-9)
                down = true;
        check(!down, "descend: a 'rise' to safe Z never moves Z down");
    }

    if (mode == "tlo") {
        // Machine A: a tool measured 12 mm longer than the reference.
        s->applyToolLengthOffset(12.0);
        wait(600);
        // Switch to machine B (same port here), which has no BitSetter.
        grbl.events.clear();
        QString why;
        panel.reloadMachine(&why);
        wait(300);
        grbl.send("\r\nGrbl 1.1h ['$' for help]\r\n");   // the board reboots on port open (DTR)
        wait(2500);
        std::printf("offset label on machine B: %s\n", qPrintable(panel.m_tloLabel->text()));
        for (const QByteArray &e : grbl.events) std::printf("  sent: %s\n", e.constData());
        check(grbl.count("G43.1 Z12") == 0, "tlo: machine A's tool offset is not applied on machine B");
    }

    if (mode == "descendlive") {
        // Safe Z -30. The job cuts at machine Z -40 and its last line is the
        // retract to its clearance, machine Z -10 (tall board). GRBL acks that
        // line as soon as it is planned: the last report still shows -40.
        panel.m_bsSafeZ->setValue(-30.0);
        grbl.mz = -40;
        wait(600);
        grbl.events.clear();
        panel.m_endOfJobPending = true;
        grbl.running = true;   // reports Run while the planner works through the lines
        s->startStream({QStringLiteral("G1X5F500"), QStringLiteral("G0Z30")});
        spin(2000, [&] { return !s->isStreaming(); });
        // Every line is acked; 500 ms later the planner has run the retract.
        QTimer::singleShot(500, [&] { grbl.mz = -10; grbl.running = false; });
        wait(2000);
        std::printf("end of job, last report at Z-40, retract to Z-10:\n");
        for (const QByteArray &e : grbl.events) std::printf("  sent: %s\n", e.constData());
        bool down = false;
        for (const QByteArray &e : grbl.events)
            if (e.startsWith("G53 G0 Z") && e.mid(8).toDouble() < -10 - 1e-9)
                down = true;
        check(!down, "descendlive: the end-of-job rise never moves Z down from the job's retract");
    }

    if (mode == "tc") {
        panel.m_bsEnabled->setChecked(true);
        panel.m_haveRef = true;
        panel.m_refZ = -50;
        grbl.probeZ = -48;
        panel.m_bsSafeZ->setValue(-30.0);
        grbl.mz = -40;
        wait(600);
        grbl.events.clear();
        seen.clear();
        answer = [&](const Seen &v) {
            std::printf("prompt: %s\n", qPrintable(v.headline));
            return QStringLiteral("Measure and continue");
        };
        panel.m_endOfJobPending = true;
        s->startStream({QStringLiteral("M3S1000"), QStringLiteral("G1X5F500"),
                        QStringLiteral("M0 ;T2"), QStringLiteral("M3S1000"), QStringLiteral("G1X6F500")});
        spin(8000, [&] { return !s->isStreaming(); });
        wait(2000);
        for (const QByteArray &e : grbl.events) std::printf("  sent: %s\n", e.constData());
        check(seen.size() == 1, "tc: one prompt");
        check(!s->isStreaming(), "tc: job finished");
        check(grbl.indexOf("G43.1 Z2") >= 0, "tc: offset applied");
        check(grbl.indexOf("G1X6") > grbl.indexOf("G43.1 Z2"), "tc: rest after offset");
        check(grbl.indexOf("M5", grbl.indexOf("G1X6")) > 0, "tc: spindle off at end");
    }

    if (mode == "holdend") {
        panel.m_endOfJobPending = true;
        grbl.running = true;
        wait(300);
        grbl.events.clear();
        s->startStream({QStringLiteral("M3S1000"), QStringLiteral("G1X5F500"), QStringLiteral("G0Z30")});
        spin(2000, [&] { return !s->isStreaming(); });
        panel.m_pauseBtn->click();          // hold in the last seconds
        wait(300);
        grbl.running = false;               // planner would be done, but held
        wait(1500);
        check(grbl.indexOf("G53 G0 Z") < 0, "holdend: no rise while held");
        std::printf("pause button: %s\n", qPrintable(panel.m_pauseBtn->text()));
        panel.m_pauseBtn->click();          // resume
        wait(1500);
        for (const QByteArray &e : grbl.events) std::printf("  sent: %s\n", e.constData());
        check(grbl.indexOf("G53 G0 Z") >= 0 && grbl.indexOf("M5", grbl.indexOf("G53 G0 Z")) >= 0,
              "holdend: rise and M5 after Resume");
    }

    if (mode == "parkend" || mode == "gotoend" || mode == "measureend") {
        // As descendlive: safe Z -30, the job cuts at machine Z -40 and its
        // last line retracts to Z -10 (tall board). Every line is acked, the
        // planner still runs them (Run). The operator presses Park (or Go to
        // X0 Y0 / Measure bit) as soon as the log says "program finished".
        panel.m_bsSafeZ->setValue(-30.0);
        panel.m_bsEnabled->setChecked(true);
        grbl.mz = -40;
        wait(600);
        grbl.events.clear();
        panel.m_endOfJobPending = true;
        grbl.running = true;
        s->startStream({QStringLiteral("G1X5F500"), QStringLiteral("G0Z30")});
        spin(2000, [&] { return !s->isStreaming(); });
        if (mode == "parkend") panel.park();
        else if (mode == "gotoend") panel.goToXY0();
        else panel.measureTool(c2d::MachinePanel::Phase::MeasureManual);
        QTimer::singleShot(500, [&] { grbl.mz = -10; grbl.running = false; });
        wait(2500);
        for (const QByteArray &e : grbl.events) std::printf("  sent: %s\n", e.constData());
        bool down = false;
        for (const QByteArray &e : grbl.events)
            if (e.startsWith("G53 G0 Z") && e.mid(8).toDouble() < -10 - 1e-9)
                down = true;
        check(!down, "parkend: no move down from the job's final retract");
    }

    if (mode == "rehearsetc") {
        // Tool-change spot typed as Y +20 (the minus forgotten): outside the
        // travel (machine coordinates are negative). Run refuses a job with
        // tool changes for that; the rehearsal should too.
        panel.m_bsEnabled->setChecked(true);
        panel.m_haveRef = true;
        panel.m_refZ = -50;
        panel.m_tcX->setValue(-120.0);
        panel.m_tcY->setValue(20.0);
        std::printf("insideTravel(tc) = %d\n", int(panel.insideTravel(-120, 20)));
        answer = [&](const Seen &v) {
            std::printf("prompt: %s\n", qPrintable(v.headline));
            if (v.headline.contains("Start the rehearsal")) return QStringLiteral("Start rehearsal");
            if (v.headline.contains("outside the machine's travel")) return QStringLiteral("OK");
            return QStringLiteral("Stop the job");
        };
        grbl.events.clear();
        panel.rehearseToolChange();
        wait(2500);
        for (const QByteArray &e : grbl.events) std::printf("  sent: %s\n", e.constData());
        check(grbl.indexOf("G53 G0 X-120.000 Y20.000") < 0,
              "rehearsetc: no rapid to a tool-change spot outside the travel");
    }

    if (mode == "zeroend") {
        // Zero Z (BitSetter on) pressed right as the job's last move ends:
        // the end-of-job rise fires while the measure macro runs.
        panel.m_bsEnabled->setChecked(true);
        panel.m_haveRef = true;
        panel.m_refZ = -50;
        grbl.probeZ = -48;
        wait(600);
        grbl.events.clear();
        panel.m_endOfJobPending = true;
        grbl.running = true;
        s->startStream({QStringLiteral("G1X5F500"), QStringLiteral("G0Z30")});
        spin(2000, [&] { return !s->isStreaming(); });
        wait(500);
        grbl.running = false;
        // first Idle report
        const int r0 = reports;
        spin(1000, [&] { return reports > r0 && panel.m_machineState == "Idle"; });
        panel.zero(QStringLiteral("Z"));
        wait(3000);
        for (const QByteArray &e : grbl.events) std::printf("  sent: %s\n", e.constData());
        std::printf("haveRef=%d refZ=%.3f\n", int(panel.m_haveRef), panel.m_refZ);
        check(panel.m_haveRef, "zeroend: the reference measured by Zero Z is kept");
        check(grbl.indexOf("M5") >= 0, "zeroend: spindle off");
    }


    if (mode == "zerooff" || mode == "zerounhomed") {
        // BitSetter on; Zero Z. r12zerooff: button position typed as Y +20
        // (outside the travel; Measure tool refuses it). r12zerounhomed:
        // unlocked out of an alarm without homing (Measure tool refuses).
        panel.m_bsEnabled->setChecked(true);
        if (mode == "zerooff") {
            panel.m_bsX->setValue(-20.0);
            panel.m_bsY->setValue(20.0);
            std::printf("insideTravel(bs) = %d\n", int(panel.insideTravel(-20, 20)));
        } else {
            panel.m_homed = false;
            panel.m_unhomedWhy = QStringLiteral("unlocked out of an alarm without homing");
        }
        answer = [&](const Seen &v) { std::printf("prompt: %s\n", qPrintable(v.headline)); return QStringLiteral("OK"); };
        wait(600);
        grbl.events.clear();
        panel.measureTool(c2d::MachinePanel::Phase::MeasureManual);
        wait(600);
        const int measureG53 = grbl.count("G53");
        std::printf("Measure tool sent %d G53 moves\n", measureG53);
        grbl.events.clear();
        panel.zero(QStringLiteral("Z"));
        wait(2500);
        for (const QByteArray &e : grbl.events) std::printf("  sent: %s\n", e.constData());
        check(measureG53 == 0, "r12: Measure tool refuses");
        check(grbl.count("G53 G0 X") == 0, "r12: Zero Z sends no machine-coordinate move to the BitSetter Measure tool refuses");
    }

    if (mode == "bzoff") {
        // BitZero Z only, BitSetter on with its button typed outside the travel.
        panel.m_bsEnabled->setChecked(true);
        panel.m_bsX->setValue(-20.0);
        panel.m_bsY->setValue(20.0);
        panel.m_bzPanel->m_modes->button(c2d::BitZeroPanel::ZOnly)->click();
        wait(600);
        answer = [&](const Seen &v) {
            return v.headline.contains("off the machine") ? QStringLiteral("OK") : QString();
        };
        grbl.events.clear();
        panel.bitZeroBegin();
        wait(4000);
        for (const QByteArray &e : grbl.events) std::printf("  sent: %s\n", e.constData());
        check(grbl.count("G53 G0 X-20.000 Y20.000") == 0, "r12bz: no rapid to a BitSetter outside the travel");
    }

    if (mode == "deaf") {
        // Status reports reaching the panel while the tool-change prompt is open.
        panel.m_bsEnabled->setChecked(true);
        panel.m_haveRef = true;
        panel.m_refZ = -50;
        grbl.probeZ = -48;
        wait(600);
        grbl.events.clear();
        int during = -1, sentJogs = -1;
        double dro0 = 0, dro1 = 0;
        answer = [&](const Seen &v) {
            std::printf("prompt: %s\n", qPrintable(v.headline));
            const int r0 = reports;
            dro0 = panel.m_grbl->status().mx;
            grbl.mx = -150;   // the machine reports a new position meanwhile
            QElapsedTimer t; t.start();
            while (t.elapsed() < 1500) QApplication::processEvents(QEventLoop::AllEvents, 20);
            during = reports - r0;
            dro1 = panel.m_grbl->status().mx;
            const int j0 = grbl.count("$J=");
            for (int k = 0; k < 4; ++k) {
                padPress(4);   // z- tap on the pad
                QElapsedTimer u; u.start();
                while (u.elapsed() < 300) QApplication::processEvents(QEventLoop::AllEvents, 20);
            }
            sentJogs = grbl.count("$J=") - j0;
            return QStringLiteral("Measure and continue");
        };
        s->startStream({QStringLiteral("G1X5F500"), QStringLiteral("M0 ;T2"), QStringLiteral("G1X6F500")});
        spin(10000, [&] { return !s->isStreaming(); });
        wait(1000);
        std::printf("status reports processed during 1.5 s of open prompt: %d; DRO X %.3f -> %.3f; pad z- taps that went out: %d of 4\n",
                    during, dro0, dro1, sentJogs);
        check(during >= 3, "r12deaf: status keeps coming in while the tool prompt is open");
    }
    if (mode == "rehdeaf") {
        panel.m_bsEnabled->setChecked(true);
        panel.m_haveRef = true;
        panel.m_refZ = -50;
        grbl.probeZ = -48;
        panel.m_tcX->setValue(-120.0);
        panel.m_tcY->setValue(-120.0);
        wait(600);
        int during = -1, j0 = 0, jDuring = -1;
        answer = [&](const Seen &v) {
            std::printf("prompt: %s\n", qPrintable(v.headline));
            if (v.headline.contains("Start the rehearsal")) return QStringLiteral("Start rehearsal");
            if (v.headline.contains("Rehearsal")) {
                if (v.headline.contains("change the tool")) return QStringLiteral("Measure and continue");
                const int r0 = reports;
                QElapsedTimer t; t.start();
                while (t.elapsed() < 1500) QApplication::processEvents(QEventLoop::AllEvents, 20);
                during = reports - r0;
                j0 = grbl.count("$J=");
                for (int k = 0; k < 4; ++k) {
                    padPress(4);
                    QElapsedTimer u; u.start();
                    while (u.elapsed() < 300) QApplication::processEvents(QEventLoop::AllEvents, 20);
                }
                jDuring = grbl.count("$J=") - j0;
                return QStringLiteral("OK");
            }
            return QStringLiteral("OK");
        };
        panel.rehearseToolChange();
        spin(15000, [&] { return during >= 0; });
        wait(500);
        wait(1500);
        std::printf("r13: status reports during 1.5 s of the rehearsal summary: %d; pad jogs sent while open %d, total after close %d\n", during, jDuring, grbl.count("$J=") - j0);
        for (int i = grbl.indexOf("$J="); i >= 0 && i < grbl.events.size(); ++i) std::printf("  ev: %s\n", grbl.events.at(i).constData());
        check(during >= 3, "r13rehdeaf: status keeps coming while the rehearsal summary is open");
    }
    if (mode == "bztap") {
        // BitZero Z only, BitSetter off. The operator taps Z+ on the pad
        // while the plate probe runs.
        panel.m_bzPanel->m_modes->button(c2d::BitZeroPanel::ZOnly)->click();
        grbl.probeZ = -40;
        wait(600);
        grbl.events.clear();
        panel.bitZeroBegin();
        QTimer::singleShot(30, [&] { padPress(5); });
        wait(4000);
        for (const QByteArray &e : grbl.events) std::printf("  sent: %s\n", e.constData());
        const int g10 = grbl.indexOf("G10 L20");
        const int jog = grbl.indexOf("$J=");
        std::printf("r13bztap: jog at %d, G10 at %d\n", jog, g10);
        check(!(jog >= 0 && g10 > jog), "r13bztap: no jog goes out between the probe and BitZero's zero");
    }
    if (mode == "endtap") {
        // A job ends (all acked, planner still running); the operator taps
        // Z+ on the pad before the machine stands still.
        panel.m_endOfJobPending = true;
        grbl.running = true;
        wait(300);
        grbl.events.clear();
        s->startStream({QStringLiteral("M3S10000"), QStringLiteral("G1X5F500"), QStringLiteral("G0Z30")});
        spin(2000, [&] { return !s->isStreaming(); });
        wait(300);
        padPress(5);
        wait(300);
        grbl.running = false;
        wait(3000);
        for (const QByteArray &e : grbl.events) std::printf("  sent: %s\n", e.constData());
        const int m5 = grbl.indexOf("M5");
        std::printf("r13endtap: jog at %d, rise at %d, M5 at %d\n", grbl.indexOf("$J="), grbl.indexOf("G53 G0 Z"), m5);
        check(m5 >= 0, "r13endtap: the spindle is stopped at the end of the job");
    }
    if (mode == "steptap" || mode == "stephold") {
        wait(600);
        grbl.events.clear();
        int ran = 0; int evAt = -1;
        panel.whenIdle([&] { ++ran; evAt = grbl.events.size(); grbl.events << (grbl.jogging() ? "STEP-WHILE-JOG" : "STEP"); });
        if (mode == "steptap") {
            padPress(5);
        } else {
            panel.beginHoldJog('Z', 1);
            wait(1500);
            panel.endHoldJog();
        }
        wait(3000);
        for (const QByteArray &e : grbl.events) std::printf("  ev: %s\n", e.constData());
        int lastJog = -1;
        for (int i = 0; i < grbl.events.size(); ++i) if (grbl.events.at(i).startsWith("$J=") || grbl.events.at(i) == "JOGCANCEL") lastJog = i;
        std::printf("r14: ran %d, step at %d, last jog/cancel at %d\n", ran, evAt, lastJog);
        check(ran == 1, "r14: the step runs once");
        // Before the jog starts or after it ends, never in the middle (under
        // load the hold may only begin once the step has run).
        check(grbl.events.indexOf("STEP-WHILE-JOG") < 0 && (evAt > lastJog || evAt == 0),
              "r14: the step does not run in the middle of the jog");
    }
    if (mode == "bzstart") {
        panel.m_bzPanel->m_modes->button(c2d::BitZeroPanel::ZOnly)->click();
        grbl.probeZ = -40;
        panel.m_tcX->setValue(-120.0);
        panel.m_tcY->setValue(-120.0);
        wait(600);
        grbl.events.clear();
        panel.bitZeroBegin();
        spin(5000, [&] { return panel.m_onIdle && panel.m_phase == c2d::MachinePanel::Phase::Idle; });
        std::printf("step pending: %d, busy %d, state %s\n", bool(panel.m_onIdle), panel.isBusy(), qPrintable(panel.m_machineState));
        answer = [&](const Seen &v) { std::printf("prompt: %s\n", qPrintable(v.headline)); return QStringLiteral("Stop the job"); };
        QString why;
        const bool started = panel.startFromRemote(&why);
        std::printf("remote start: %d (%s), streaming %d\n", started, qPrintable(why), s->isStreaming());
        wait(2000);
        for (const QByteArray &e : grbl.events) std::printf("  sent: %s\n", e.constData());
        const int g10 = grbl.indexOf("G10 L20");
        const int g21 = grbl.indexOf("G21");
        std::printf("r14bzstart: G10 at %d, job's first line at %d\n", g10, g21);
        check(!(started && (g10 < 0 || g10 > g21)), "r14bzstart: no job starts before BitZero's zero is set");
        s->stopStream();
        wait(500);
    }
    if (mode == "jogstart") {
        // A pad start right after a jog tap, before the status shows Jog.
        grbl.jogMs = 800;
        wait(600);
        panel.m_step->setCurrentIndex(panel.m_step->count() - 1);
        padPress(4);
        QString why;
        const bool started = panel.startFromRemote(&why);
        std::printf("remote start right after a tap: %d (%s)\n", started, qPrintable(why));
        check(!started && !s->isStreaming(), "a start right after a jog tap is refused");
        wait(1500);
    }
    check(!mode.isEmpty(), "a scenario was named (R4MODE)");
    std::printf("test_machinestop %s: %d checks OK\n", mode.constData(), g_checks);
    answerer.stop();
    ::close(slave);
    ::close(master);
    QSettings().clear();
    return 0;
}
