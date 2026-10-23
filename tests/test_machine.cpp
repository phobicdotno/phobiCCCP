// The Machine panel against a scripted controller on a pseudo-terminal: the
// tool-change prompt, zeroing and jogging during and around a job, cancelling
// and disconnecting mid-job. Popups (shopAsk) are answered by a timer that
// reads them and presses a button by its text.
//
// Skips (exit 77) where no pty can be opened as a serial port.
//
// Plain asserts, no framework; exits 0 on success.

#include "../src/c2ddocument.h"
#include "../src/element.h"
#include "../src/gamepad.h"
#include "../src/gcodeexport.h"
#include "../src/machinepanel.h"
#include "../src/toollibrary.h"
#include "../src/toolstrip.h"

#include <QApplication>
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
    double mx = -100, my = -100, mz = -30;
    double wcoZ = 0;   // work Z offset in the status reports
    // A jog reports Jog for `jogMs` after its line (it does not move the
    // position: tests set that), or until a jog cancel.
    int jogMs = 300;
    QElapsedTimer jogClock, homeClock;
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
                send("<" + QByteArray(locked || alarm ? "Alarm" : held ? "Hold:0" : home ? "Home" : jogging() ? "Jog" : "Idle")
                     + "|MPos:" + num(mx) + ',' + num(my) + ',' + num(mz)
                     + "|FS:0,0|WCO:0.000,0.000," + num(wcoZ) + ">\r\n");
            } else if (c == '!') {
                held = true;
                events << "!";
            } else if (c == '~') {
                held = false;
                events << "~";
            } else if (uchar(c) == 0x85) {
                jogClock.invalidate();
                events << "JOGCANCEL";
            } else if (c == 0x18) {
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
                else if (line.contains("G38.2"))
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
    QApplication::setApplicationName(QStringLiteral("test_machine"));
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
        std::printf("test_machine: no pty, skipped\n");
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

    // Park stays inside the travel the controller reports ($131=425), not
    // the Settings size (a 4x4 by default).
    {
        grbl.events.clear();
        button(QStringLiteral("Park front right"))->click();
        spin(1500, [&] { return grbl.count("G53 G0 X") > 0 && !s->isMacroRunning(); });
        const int i = grbl.indexOf("G53 G0 X");
        check(i >= 0, "park: moves");
        const QByteArray mv = grbl.events.at(i);
        const double y = mv.mid(mv.indexOf('Y') + 1).toDouble();
        check(y >= -425 && y <= -400, "park: Y inside the reported 425 mm travel");
        wait(800);
    }

    // Jog targets come from a fresh report only. The panel's last report says
    // Z -30; the machine has since moved to Z -20 (a hold-jog decelerating
    // past the release). A tap right after the jog was stopped must start
    // from where the machine is, not from the stale report: Z+10 from -20,
    // not to -20 (which is down from where it stands).
    {
        grbl.mz = -20;
        button(QStringLiteral("STOP"))->click();
        key(Qt::Key_PageUp, true);
        key(Qt::Key_PageUp, false);
        spin(1500, [&] { return !lastJog().isEmpty(); });
        check(lastJog().contains("Z-10.000"), "jog: a tap after a stop starts from a fresh report");
        wait(800);
        grbl.mz = -10;
    }

    // Hold-jog lookahead: the machine reports it is not getting anywhere
    // (planner full, status lagging); holding Z- must not keep queueing.
    {
        wait(800);
        const int before = grbl.count("$J=");
        grbl.jogClock.invalidate();
        grbl.jogMs = 100000;                 // jogging, and getting nowhere
        key(Qt::Key_PageDown, true);
        wait(1600);
        const int sent = grbl.count("$J=") - before;
        key(Qt::Key_PageDown, false);
        grbl.jogMs = 300;
        check(sent >= 1 && sent <= 4, "jog: a hold queues only a little ahead of the machine");
        wait(1500);
    }

    // A stick held about half way wobbles round the 16000 threshold: each
    // drop under it was a tap, a full jog step (six steps for one push).
    // And a D-pad let go while the link was down, then pressed again after
    // reconnecting, was taken for no change and jogged nothing.
    {
        wait(800);
        grbl.events.clear();
        for (int v : {15900, 16100, 15950, 16080, 15920, 16120, 15980, 16050, 15900, 16100}) {
            emit pad->axisChanged(0, v);
            wait(120);
        }
        emit pad->axisChanged(0, 0);
        wait(1500);
        check(grbl.count("$J=") <= 4, "gamepad: a half-held stick is one push, not a tap per wobble");

        emit pad->axisChanged(1, -32767);
        wait(100);
        s->disconnectPort();
        wait(100);
        emit pad->axisChanged(1, 0);
        check(s->connectPort(QString::fromLatin1(name)), "gamepad: reconnects");
        spin(3000, [&] { return panel.statusJson().value("homed").toBool(); });
        wait(1200);
        grbl.events.clear();
        emit pad->axisChanged(1, -32767);
        wait(100);
        emit pad->axisChanged(1, 0);
        wait(1500);
        check(grbl.count("$J=") == 1, "gamepad: a press after a dropped link jogs");
    }

    // Tool changes: the second pause, same number, names the second bit.
    // BitSetter off: close the prompt, zero Z by hand, then Continue.
    {
        seen.clear();
        int prompts = 0;
        answer = [&](const Seen &v) {
            if (v.caption == QLatin1String("RUN PROGRAM"))
                return QStringLiteral("▶ Start");
            if (v.caption == QLatin1String("TOOL CHANGE"))
                return ++prompts == 1 ? QStringLiteral("Same length: continue")
                                      : QStringLiteral("Close: I'll zero Z");
            return QStringLiteral("OK");
        };
        bool finished = false, finishedOk = false;
        QObject::connect(s, &c2d::GrblStreamer::streamFinished, [&](bool ok) {
            finished = true;
            finishedOk = ok;
        });
        runBtn->click();
        check(s->isStreaming(), "job: started");
        spin(15000, [&] { return prompts == 2 && !QApplication::activeModalWidget(); });
        check(prompts == 2, "job: two tool-change prompts");
        QStringList bodies;
        for (const Seen &v : seen)
            if (v.caption == QLatin1String("TOOL CHANGE"))
                bodies << v.body;
        check(bodies.size() == 2 && bodies.at(0).contains(QLatin1String("1/4 endmill")),
              "job: the first prompt names the 1/4");
        check(bodies.at(1).contains(QLatin1String("1/8 endmill"))
                  && bodies.at(1).contains(QLatin1String("3.175")),
              "job: the second prompt names the 1/8, the bit the program changes to");
        wait(500);
        check(s->isParkedForTool(), "manual zero: the program stays parked after Close");

        const int g10 = grbl.count("G10");
        button(QStringLiteral("Zero X and Y here"))->click();
        button(QStringLiteral("Zero all"))->click();
        wait(400);
        check(grbl.count("G10") == g10, "zero: XY is refused while a job is parked");
        button(QStringLiteral("Zero"), QStringLiteral("work Z0"))->click();
        spin(600, [&] { return grbl.count("G10 L20 P1 Z0") > 0; });
        check(grbl.count("G10 L20 P1 Z0") == 1, "manual zero: Z can be zeroed for the new bit");
        wait(600);                           // answered, and stands still on fresh reports

        const int dialogs = seen.size();
        check(runBtn->text().contains(QLatin1String("Continue")), "manual zero: the button says Continue");
        runBtn->click();
        spin(10000, [&] { return finished; });
        check(seen.size() == dialogs, "manual zero: Continue after zeroing asks nothing");
        check(finished && finishedOk, "manual zero: the job runs to the end");
        QObject::disconnect(s, &c2d::GrblStreamer::streamFinished, nullptr, nullptr);
        spin(1500, [&] { return !panel.isBusy(); });
    }

    // Zero refused mid-job without the manual-zero answer, too.
    {
        grbl.hangDwell = true;
        s->startStream({QStringLiteral("M3S1000"), QStringLiteral("G4P3.00"), QStringLiteral("G0X1")});
        wait(400);
        const int g10 = grbl.count("G10");
        button(QStringLiteral("Zero"), QStringLiteral("work Z0"))->click();
        wait(300);
        check(grbl.count("G10") == g10, "zero: refused while a job runs");

        // Cancel during the dwell: GRBL says Idle, but the job is about to
        // carry on: the hold must go out before the question.
        grbl.events.clear();
        answer = [&](const Seen &) { return QStringLiteral("Keep running"); };
        button(QStringLiteral("✕ Cancel job"))->click();
        wait(500);
        check(grbl.events.indexOf("!") >= 0 && grbl.events.indexOf("~") > grbl.events.indexOf("!"),
              "cancel: held during a dwell, and released by Keep running");
        grbl.events.clear();
        answer = [&](const Seen &) { return QStringLiteral("Cancel job"); };
        button(QStringLiteral("✕ Cancel job"))->click();
        spin(3000, [&] { return grbl.events.contains("^X"); });
        check(grbl.events.indexOf("!") >= 0 && grbl.events.indexOf("^X") > grbl.events.indexOf("!"),
              "cancel: hold, then reset");
        check(!s->isStreaming(), "cancel: the job is over");
        grbl.hangDwell = false;
        spin(2500, [&] { return !panel.isBusy(); });
    }

    // Continue pressed while the park is still on its way: refused, the
    // prompt comes up at the spot, and its "Stop the job" stops the job.
    {
        seen.clear();
        int toolPrompts = 0;
        answer = [&](const Seen &v) {
            if (v.caption == QLatin1String("TOOL CHANGE")) {
                ++toolPrompts;
                return QStringLiteral("Stop the job");
            }
            return QStringLiteral("OK");
        };
        bool parkDone = false;
        auto c = QObject::connect(s, &c2d::GrblStreamer::macroFinished, [&](bool) { parkDone = true; });
        grbl.events.clear();
        s->startStream({QStringLiteral("M0 ;T2"), QStringLiteral("G90G21"),
                        QStringLiteral("M3S10000"), QStringLiteral("G4P30.00"), QStringLiteral("G0X1")});
        spin(1500, [&] { return parkDone; });
        QObject::disconnect(c);
        check(parkDone && runBtn->text().contains(QLatin1String("Continue")), "early continue: parked");
        runBtn->click();                     // the machine is still travelling
        wait(200);
        check(s->isParkedForTool() && grbl.count("G90G21") == 0,
              "early continue: refused until the tool prompt was answered");
        spin(3000, [&] { return toolPrompts > 0; });
        wait(500);
        check(toolPrompts == 1, "early continue: the prompt comes up at the spot");
        check(!s->isStreaming() && grbl.count("G90G21") == 0, "early continue: Stop the job stops it");
        spin(2000, [&] { return !panel.isBusy(); });
        wait(500);
    }

    // Manual zero: Zero pressed while the last jog still moves is refused
    // (GRBL would answer error:9), and so is Continue; once it stands
    // still both work and the job runs to the end.
    {
        seen.clear();
        answer = [&](const Seen &v) {
            if (v.caption == QLatin1String("TOOL CHANGE"))
                return QStringLiteral("Close: I'll zero Z");
            return QStringLiteral("Cancel");
        };
        bool finished = false, finishedOk = false;
        auto c = QObject::connect(s, &c2d::GrblStreamer::streamFinished, [&](bool ok) {
            finished = true;
            finishedOk = ok;
        });
        s->startStream({QStringLiteral("M0 ;T2"), QStringLiteral("G90G21"), QStringLiteral("G0X1")});
        spin(5000, [&] { return !seen.isEmpty() && !QApplication::activeModalWidget(); });
        wait(400);
        check(s->isParkedForTool(), "zero while jogging: parked, prompt closed");
        grbl.jogMs = 1500;
        grbl.events.clear();
        key(Qt::Key_PageDown, true);
        key(Qt::Key_PageDown, false);
        spin(1200, [&] { return grbl.count("$J=") > 0; });
        wait(100);
        button(QStringLiteral("Zero"), QStringLiteral("work Z0"))->click();
        wait(300);
        check(grbl.count("G10") == 0 && grbl.count("G49") == 0, "zero while jogging: refused");
        runBtn->click();
        wait(400);
        check(s->isParkedForTool() && !finished && grbl.count("G90G21") == 0,
              "continue while jogging: refused, the job is not lost");

        // Standing still, but the controller refuses the zero: not counted.
        spin(2000, [&] { return !grbl.jogging(); });
        wait(700);
        grbl.jogMs = 300;
        grbl.reject = "G10";
        button(QStringLiteral("Zero"), QStringLiteral("work Z0"))->click();
        spin(600, [&] { return grbl.count("G10 L20 P1 Z0") > 0; });
        wait(300);
        grbl.reject.clear();
        check(grbl.count("G10 L20 P1 Z0") == 1, "zero refused by the controller: sent once");
        int dialogs = seen.size();
        runBtn->click();                     // "Z not zeroed" -> Cancel
        wait(500);
        check(seen.size() == dialogs + 1 && seen.last().headline.contains(QLatin1String("not been zeroed")),
              "zero refused by the controller: Continue warns Z was not zeroed");
        check(s->isParkedForTool(), "zero refused by the controller: still parked");

        button(QStringLiteral("Zero"), QStringLiteral("work Z0"))->click();
        spin(600, [&] { return grbl.count("G10 L20 P1 Z0") > 1; });
        wait(300);
        dialogs = seen.size();
        runBtn->click();
        spin(5000, [&] { return finished; });
        QObject::disconnect(c);
        check(seen.size() == dialogs, "zero acked: Continue asks nothing");
        check(finished && finishedOk, "zero acked: the job runs to the end");
        spin(2000, [&] { return !panel.isBusy(); });
        wait(500);
    }

    // A tap still waiting for a fresh report, then Zero: the zero must not
    // land before the step.
    {
        grbl.jogMs = 50;
        button(QStringLiteral("STOP"))->click();   // forgets the jog plan
        grbl.events.clear();
        key(Qt::Key_PageDown, true);
        key(Qt::Key_PageDown, false);
        button(QStringLiteral("Zero"), QStringLiteral("work Z0"))->click();
        spin(1500, [&] { return grbl.count("$J=") > 0; });
        wait(300);
        const int g = grbl.indexOf("G10"), j = grbl.indexOf("$J=");
        check(j >= 0 && (g < 0 || g > j), "tap then zero: the zero never lands before the tap");
        grbl.jogMs = 300;
        wait(800);
    }

    // Hold in the last seconds: the stream finishes while the machine holds.
    // The button keeps offering Resume and Cancel job still works.
    {
        grbl.holdAcks = true;
        bool finished = false;
        auto c = QObject::connect(s, &c2d::GrblStreamer::streamFinished, [&](bool) { finished = true; });
        s->startStream({QStringLiteral("G90G21"), QStringLiteral("G1X5F500"), QStringLiteral("G4P1.00"),
                        QStringLiteral("G0Z5")});
        wait(300);
        QPushButton *pause = button(QStringLiteral("⏸ Hold"));
        pause->click();
        wait(300);
        grbl.release();                      // the last lines go into the planner while held
        spin(1500, [&] { return finished; });
        wait(600);
        QObject::disconnect(c);
        check(finished && grbl.held && pause->text().contains(QLatin1String("Resume")),
              "hold at the end: the button still says Resume");
        seen.clear();
        answer = [&](const Seen &) { return QStringLiteral("Keep running"); };
        button(QStringLiteral("✕ Cancel job"))->click();
        wait(400);
        check(seen.size() == 1 && seen.last().headline.contains(QLatin1String("Cancel")),
              "hold at the end: Cancel job asks, not 'nothing to cancel'");
        check(pause->text().contains(QLatin1String("Resume")), "hold at the end: Keep running stays held");
        grbl.held = false;                   // lifted at the machine
        spin(1500, [&] { return pause->text().contains(QLatin1String("Hold")); });
        check(pause->text().contains(QLatin1String("Hold")), "hold lifted: the button follows the machine");
        wait(800);
    }

    // BitSetter on and no reference: Run refuses, as the rehearsal does.
    {
        QToolButton *bs = nullptr;
        for (QToolButton *b : panel.findChildren<QToolButton *>())
            if (b->text() == QLatin1String("BitSetter on"))
                bs = b;
        check(bs != nullptr, "bitsetter: switch found");
        bs->setChecked(true);
        seen.clear();
        answer = [&](const Seen &) { return QStringLiteral("OK"); };
        runBtn->click();
        wait(500);
        check(!s->isStreaming(), "no reference: Run does not start");
        check(!seen.isEmpty() && seen.last().headline.contains(QLatin1String("no BitSetter reference")),
              "no reference: and says why");
        QString why;
        check(!panel.startFromRemote(&why) && why.contains(QLatin1String("BitSetter reference")),
              "no reference: a pad's start is refused with the reason");
        bs->setChecked(false);
    }

    // Jog taps are limited while the controller has not answered.
    {
        wait(1000);
        grbl.ackJogs = false;
        const int before = grbl.count("$J=");
        for (int i = 0; i < 6; ++i) {
            key(Qt::Key_Left, true);
            key(Qt::Key_Left, false);
            wait(60);
        }
        wait(300);
        check(grbl.count("$J=") - before <= 2, "jog: taps stop while two are unanswered");
        grbl.ackJogs = true;
        spin(4000, [&] { return s->pendingCommands() == 0; });
    }

    // A pad cannot start the job while the surface map waits for the plate.
    {
        grbl.wcoZ = -60;   // Z zeroed on the stock, low enough for the map's travel
        wait(800);
        panel.showPage(QStringLiteral("bitzero"));
        for (QToolButton *b : panel.findChildren<QToolButton *>())
            if (b->text() == QLatin1String("Surface map (grid of Z)"))
                b->click();
        QPushButton *begin = nullptr;
        for (QPushButton *b : panel.findChildren<QPushButton *>())
            if (b->objectName() == QLatin1String("start") && b != runBtn)
                begin = b;
        check(begin != nullptr, "map: Begin found");
        seen.clear();
        answer = [&](const Seen &) { return QStringLiteral("OK"); };
        begin->click();
        QString logText;
        spin(3000, [&] {
            logText.clear();
            for (QPlainTextEdit *t : panel.findChildren<QPlainTextEdit *>())
                logText += t->toPlainText();
            return logText.contains(QLatin1String("Put the plate under the tool"));
        });
        check(logText.contains(QLatin1String("Put the plate under the tool")), "map: waits for the plate");
        check(panel.isBusy(), "map: waiting for the plate is busy");
        QString why;
        check(!panel.startFromRemote(&why) && !s->isStreaming(), "map: a pad's start is refused");
        // A zero now would file the rest of the heights under the wrong
        // grid points: refused from the pad and from the buttons alike.
        const int g10 = grbl.count("G10");
        padPress(0);                         // zeroxy
        padPress(3);                         // zeroz
        button(QStringLiteral("Zero all"))->click();
        wait(400);
        check(grbl.count("G10") == g10, "map: zeroing is refused while the map runs");
        button(QStringLiteral("Stop the surface map"))->click();
        wait(800);
        check(!panel.isBusy(), "map: stopped, not busy");
        panel.showPage(QStringLiteral("job"));
        grbl.wcoZ = 0;
    }

    // BitSetter turned on after "Close: I'll zero Z": the Zero Z beside Z
    // zeroes and measures the new bit as the reference. Continue must not
    // then warn that the tool was not measured.
    {
        wait(800);
        QToolButton *bs = nullptr;
        for (QToolButton *b : panel.findChildren<QToolButton *>())
            if (b->text() == QLatin1String("BitSetter on"))
                bs = b;
        seen.clear();
        answer = [&](const Seen &v) {
            if (v.caption == QLatin1String("TOOL CHANGE"))
                return QStringLiteral("Close: I'll zero Z");
            return QStringLiteral("Continue anyway");
        };
        bool finished = false, finishedOk = false;
        auto c = QObject::connect(s, &c2d::GrblStreamer::streamFinished, [&](bool ok) {
            finished = true;
            finishedOk = ok;
        });
        s->startStream({QStringLiteral("M0 ;T2"), QStringLiteral("G90G21"), QStringLiteral("G0X1")});
        spin(5000, [&] { return !seen.isEmpty() && !QApplication::activeModalWidget(); });
        wait(500);
        check(s->isParkedForTool(), "bs manual zero: parked after Close");
        bs->setChecked(true);
        button(QStringLiteral("Zero"), QStringLiteral("work Z0"))->click();
        spin(4000, [&] { return grbl.count("G38.2") >= 2 && !s->isMacroRunning(); });
        wait(600);
        check(grbl.count("G10 L20 P1 Z0") >= 1 && !s->isMacroRunning(),
              "bs manual zero: zeroed and measured");
        const int dialogs = seen.size();
        runBtn->click();
        spin(5000, [&] { return finished; });
        check(seen.size() == dialogs, "bs manual zero: Continue does not say the tool was not measured");
        check(finished && finishedOk, "bs manual zero: the job runs to the end");
        QObject::disconnect(c);
        bs->setChecked(false);
        spin(1500, [&] { return !panel.isBusy(); });
    }

    // A pad button mapped to home: never at a tool change, where the
    // operator's hands are at the collet; from Idle it homes and parks.
    {
        wait(800);
        seen.clear();
        answer = [&](const Seen &v) {
            if (v.caption == QLatin1String("TOOL CHANGE"))
                return QStringLiteral("Close: I'll zero Z");
            return QStringLiteral("OK");
        };
        s->startStream({QStringLiteral("M0 ;T2"), QStringLiteral("G90G21"), QStringLiteral("G0X1")});
        spin(5000, [&] { return !seen.isEmpty() && !QApplication::activeModalWidget(); });
        wait(500);
        check(s->isParkedForTool(), "pad home: parked");
        const int homes = grbl.count("$H");
        padPress(6);
        wait(500);
        check(grbl.count("$H") == homes, "pad home: refused during the tool-change pause");
        s->stopStream();
        spin(1500, [&] { return !panel.isBusy() && panel.statusJson().value("state").toString() == "Idle"; });
        padPress(6);
        spin(3000, [&] { return grbl.count("$H") > homes && !panel.isBusy(); });
        check(grbl.count("$H") == homes + 1, "pad home: homes from Idle");
        spin(3000, [&] { return grbl.count("G53 G0 X") > 0 && !panel.isBusy(); });
        wait(800);
    }

    // An end stop mid-job (ALARM:1): GRBL reads nothing until a soft reset.
    // Initialize resets, homes after the banner and parks; nothing is left
    // waiting for an answer, and the settings are asked for again.
    {
        bool finished = false, finishedOk = true;
        auto c = QObject::connect(s, &c2d::GrblStreamer::streamFinished, [&](bool ok) {
            finished = true;
            finishedOk = ok;
        });
        grbl.alarmOn = "G0X500";
        s->startStream({QStringLiteral("G90"), QStringLiteral("G0X500"), QStringLiteral("G0X1")});
        spin(3000, [&] { return finished; });
        QObject::disconnect(c);
        check(finished && !finishedOk, "limit alarm: the program stops");
        wait(600);
        check(s->needsReset(), "limit alarm: the controller needs a reset");
        check(!s->sendCommand(QStringLiteral("$X")) && s->pendingCommands() == 0,
              "limit alarm: a line is not left waiting for an answer that never comes");
        QPushButton *init = button(QStringLiteral("Initialize machine (home)"));
        check(init->isEnabled(), "limit alarm: Initialize is offered");
        grbl.events.clear();
        init->click();
        spin(3000, [&] { return grbl.count("$H") > 0 && s->pendingCommands() == 0
                                && grbl.count("G53 G0 X") > 0 && !panel.isBusy(); });
        const int reset = grbl.events.indexOf("^X"), home = grbl.indexOf("$H");
        check(reset >= 0 && home > reset, "limit alarm: Initialize resets, then homes");
        check(!s->needsReset() && s->pendingCommands() == 0, "limit alarm: nothing left pending");
        check(grbl.count("G53 G0 X") > 0, "limit alarm: parks after homing");
        wait(800);
        check(grbl.indexOf("$$", reset) > reset, "banner: the settings are asked for again");

        // The same with Unlock: reset, then $X.
        grbl.alarmOn = "G0X500";
        s->startStream({QStringLiteral("G90"), QStringLiteral("G0X500")});
        spin(3000, [&] { return s->needsReset() && !s->isStreaming(); });
        check(s->needsReset(), "limit alarm again");
        grbl.events.clear();
        button(QStringLiteral("Unlock"))->click();
        spin(2000, [&] { return grbl.count("$X") > 0 && s->pendingCommands() == 0; });
        const int reset2 = grbl.events.indexOf("^X"), unlock = grbl.indexOf("$X");
        check(reset2 >= 0 && unlock > reset2 && s->pendingCommands() == 0,
              "limit alarm: Unlock resets, then unlocks");
        // Unlocked without homing: home again for the tests after this.
        wait(800);
        button(QStringLiteral("Initialize"), QStringLiteral("homing cycle"))->click();
        spin(3000, [&] { return grbl.count("G53 G0 X") > 0 && !panel.isBusy()
                                && panel.statusJson().value("homed").toBool(); });
        check(panel.statusJson().value("homed").toBool(), "limit alarm: homed again");
        wait(800);
    }

    // Round 3. Jog taps held back for a fresh report (the machine still
    // decelerating) must not go out on the first Idle report after Stop.
    {
        spin(1500, [&] { return !panel.isBusy() && panel.statusJson().value("state").toString() == "Idle"; });
        grbl.jogMs = 4000;                   // reports say Jog for a while
        key(Qt::Key_Left, true);             // hold X-
        wait(1200);
        key(Qt::Key_Left, false);            // released: cancel, plan forgotten
        grbl.jogClock.start();               // ...still decelerating
        wait(100);
        key(Qt::Key_Left, true);             // a tap: held back for a fresh report
        key(Qt::Key_Left, false);
        wait(100);
        // Counted on our side of the line: the fake may read a jog sent just
        // before the Stop only after it.
        bool stopped = false;
        int after = 0;
        auto c = QObject::connect(s, &c2d::GrblStreamer::consoleLine, [&](const QString &l) {
            stopped = stopped || l == QLatin1String("> [soft reset]");
            after += stopped && l.startsWith(QLatin1String("> $J="));
        });
        button(QStringLiteral("⏹ Stop"))->click();
        wait(2500);
        QObject::disconnect(c);
        check(stopped && after == 0, "stop: no held-back jog goes out after Stop");
        grbl.jogMs = 300;
        wait(800);
    }

    // Initialize (reset first when locked), home, and the park after it done.
    auto initialize = [&] {
        const int parks = grbl.count("G53 G0 X");
        button(QStringLiteral("Initialize"), QStringLiteral("homing cycle"))->click();
        spin(3000, [&] { return grbl.count("G53 G0 X") > parks && !s->isMacroRunning()
                                && !panel.isBusy() && !s->needsReset()
                                && panel.statusJson().value("homed").toBool(); });
        wait(800);
    };

    // A command still unanswered (a jog the planner has not taken), Park
    // queued behind it, then an end stop: ALARM:1 is that command's answer.
    // The macro ends; nothing is written into the locked controller.
    {
        spin(1500, [&] { return !panel.isBusy() && s->pendingCommands() == 0
                                && panel.statusJson().value("state").toString() == "Idle"; });
        if (!panel.statusJson().value("homed").toBool())
            initialize();
        bool macroOk = true, macroDone = false;
        auto c = QObject::connect(s, &c2d::GrblStreamer::macroFinished, [&](bool ok) {
            macroDone = true;
            macroOk = ok;
        });
        grbl.ackJogs = false;
        s->sendCommand(QStringLiteral("$J=G91 X-1.000 F1000"));   // never answered
        // The Park button now refuses while a jog is unanswered; its macro
        // queued straight on the streamer is the case this covers.
        s->startMacro({QStringLiteral("G90"), QStringLiteral("G53 G0 Z-5.000"),
                       QStringLiteral("G53 G0 X-5.000 Y-5.000")});
        wait(200);
        check(s->pendingCommands() == 1 && s->isMacroRunning(), "alarm answer: park waits behind the jog");
        grbl.locked = true;
        grbl.events.clear();
        grbl.send("ALARM:1\r\n[MSG:Reset to continue]\r\n");
        spin(1500, [&] { return macroDone; });
        QObject::disconnect(c);
        grbl.ackJogs = true;
        check(macroDone && !macroOk && !s->isMacroRunning(), "alarm answer: the macro ends, failed");
        check(s->needsReset() && s->pendingCommands() == 0, "alarm answer: needs a reset, nothing pending");
        bool wrote = false;
        for (const QByteArray &e : grbl.events)
            wrote = wrote || (!e.isEmpty() && e != "^X" && e != "!" && e != "~" && e != "JOGCANCEL");
        check(!wrote, "alarm answer: no line written into the locked controller");

        // Run pressed, and the end stop fires while the confirmation is open:
        // Start must not stream the job into the locked controller.
        // (First out of this alarm: Initialize resets and homes.)
        spin(1500, [&] { return panel.statusJson().value("state").toString() == "Alarm"; });
        initialize();
        check(!s->needsReset() && panel.statusJson().value("homed").toBool(), "alarm answer: recovered");
        seen.clear();
        answer = [&](const Seen &v) {
            if (v.headline.startsWith(QLatin1String("Stream "))) {
                grbl.locked = true;
                grbl.send("ALARM:1\r\n[MSG:Reset to continue]\r\n");
                wait(600);
                return QStringLiteral("▶ Start");
            }
            return QStringLiteral("OK");
        };
        bool started = false;
        auto c2 = QObject::connect(s, &c2d::GrblStreamer::progressChanged, [&](int, int) { started = true; });
        runBtn->click();
        spin(5000, [&] { return seen.size() >= 2 && !QApplication::activeModalWidget(); });
        QObject::disconnect(c2);
        check(!started && !s->isStreaming(), "run: refused once the controller locked during the confirmation");
        spin(1500, [&] { return panel.statusJson().value("state").toString() == "Alarm"; });
        initialize();
        check(!s->needsReset() && panel.statusJson().value("homed").toBool(), "run: recovered");
    }

    // Held with nothing moving (Hold:0): Run and the rehearsal are refused,
    // and the button still offers Resume.
    {
        spin(1500, [&] { return !panel.isBusy() && !s->isMacroRunning()
                                && panel.statusJson().value("state").toString() == "Idle"; });
        QPushButton *pause = button(QStringLiteral("⏸ Hold"));
        pause->click();
        wait(800);
        check(pause->text() == QStringLiteral("⏵ Resume"), "held: Resume offered");
        seen.clear();
        answer = [&](const Seen &v) {
            return v.headline.startsWith(QLatin1String("Stream ")) ? QStringLiteral("▶ Start")
                                                                   : QStringLiteral("OK");
        };
        bool started = false;
        auto c = QObject::connect(s, &c2d::GrblStreamer::progressChanged, [&](int, int) { started = true; });
        runBtn->click();
        spin(5000, [&] { return seen.size() >= 2 && !QApplication::activeModalWidget(); });
        wait(300);
        check(!started && pause->text() == QStringLiteral("⏵ Resume"),
              "held: Run refused, the button still says Resume");
        seen.clear();
        QToolButton *bs = nullptr;
        for (QToolButton *b : panel.findChildren<QToolButton *>())
            if (b->text() == QLatin1String("BitSetter on"))
                bs = b;
        bs->setChecked(true);
        button(QStringLiteral("Rehearse tool change"))->click();
        wait(600);
        QObject::disconnect(c);
        bs->setChecked(false);
        check(seen.isEmpty() && !started && pause->text() == QStringLiteral("⏵ Resume"),
              "held: the rehearsal is refused, the button still says Resume");
        pause->click();                      // Resume
        wait(800);
        check(pause->text() == QStringLiteral("⏸ Hold"), "held: resumed");
    }

    // A tap refused while a macro owns the line must not leave a jog plan
    // behind: the next tap after the macro goes from where the macro left
    // the machine, not from the stale plan.
    {
        spin(1500, [&] { return !panel.isBusy() && s->pendingCommands() == 0; });
        wait(800);
        grbl.holdAcks = true;                // the dwell's ok waits for release()
        s->startMacro({QStringLiteral("G4P0.5")});
        wait(800);     // Idle reports during the dwell
        const int jogs = grbl.count("$J=");
        key(Qt::Key_Left, true);             // a tap: refused, a macro runs
        key(Qt::Key_Left, false);
        grbl.jogMs = 100000;                 // then reports say Jog: never "settled"
        grbl.jogClock.start();
        wait(300);
        check(grbl.count("$J=") == jogs && s->isMacroRunning(), "jog plan: the tap is refused during the macro");
        const double mx0 = grbl.mx;
        grbl.mx = mx0 - 50;                  // the macro moved the machine
        grbl.release();
        spin(1000, [&] { return !s->isMacroRunning(); });
        key(Qt::Key_Left, true);
        key(Qt::Key_Left, false);
        wait(300);
        grbl.jogClock.invalidate();          // stands: Idle reports again
        grbl.jogMs = 300;
        spin(1500, [&] { return grbl.count("$J=") > jogs; });
        check(grbl.count("$J=") == jogs + 1, "jog plan: the tap after the macro goes out once");
        const QByteArray j = lastJog();
        const double to = j.mid(j.indexOf('X') + 1).split(' ').first().toDouble();
        check(to < mx0 - 50 && to > mx0 - 50 - 101, "jog plan: the tap starts where the macro left the machine");
        grbl.mx = mx0;
        wait(800);
    }

    // Stop during the homing of Initialize also calls off the park after it.
    {
        spin(1500, [&] { return !panel.isBusy() && !s->isMacroRunning()
                                && panel.statusJson().value("state").toString() == "Idle"; });
        const int parks = grbl.count("G53 G0 X");
        button(QStringLiteral("Initialize"), QStringLiteral("homing cycle"))->click();
        button(QStringLiteral("⏹ Stop"))->click();
        wait(2000);
        spin(1500, [&] { return !panel.isBusy(); });
        check(grbl.count("G53 G0 X") == parks, "stop: no park after a homing that Stop interrupted");
        initialize();
    }

    // Cancel job rises to the top a moment after its reset; a Stop in that
    // moment calls the rise off too.
    {
        spin(1500, [&] { return !panel.isBusy() && !s->isMacroRunning(); });
        grbl.hangDwell = true;
        s->startStream({QStringLiteral("M3S1000"), QStringLiteral("G4P3.00"), QStringLiteral("G0X1")});
        wait(400);
        grbl.events.clear();
        answer = [&](const Seen &) { return QStringLiteral("Cancel job"); };
        button(QStringLiteral("✕ Cancel job"))->click();
        spin(3000, [&] { return grbl.events.contains("^X"); });
        button(QStringLiteral("⏹ Stop"))->click();
        wait(1500);
        grbl.hangDwell = false;
        check(grbl.count("G53 G0 Z") == 0 && !s->isMacroRunning(), "cancel then Stop: nothing rises after Stop");
        spin(1500, [&] { return !panel.isBusy(); });
    }

    // Disconnect mid-job: asked, then hold, reset, and only then closed.
    {
        grbl.hangDwell = true;
        s->startStream({QStringLiteral("M3S1000"), QStringLiteral("G4P3.00"), QStringLiteral("G0X1")});
        wait(400);
        grbl.events.clear();
        seen.clear();
        answer = [&](const Seen &) { return QStringLiteral("Keep running"); };
        button(QStringLiteral("Disconnect"))->click();
        wait(400);
        check(s->isConnected() && s->isStreaming() && grbl.events.isEmpty(),
              "disconnect: Keep running leaves the job alone");
        answer = [&](const Seen &) { return QStringLiteral("Stop and disconnect"); };
        button(QStringLiteral("Disconnect"))->click();
        spin(4000, [&] { return !s->isConnected(); });
        check(!s->isConnected(), "disconnect: the port is closed in the end");
        check(seen.size() == 2 && seen.last().headline.contains(QLatin1String("Disconnect")),
              "disconnect: asked first");
        check(grbl.events.indexOf("!") >= 0 && grbl.events.indexOf("^X") > grbl.events.indexOf("!"),
              "disconnect: hold, then reset, before closing");
    }

    // Reconnected to a controller still locked by an end stop from before
    // (it does not reboot when the port opens, and never says ALARM:1 to
    // this connection): Initialize and Unlock reset first, and nothing is
    // left waiting for an answer.
    {
        grbl.locked = true;
        grbl.alarm = true;
        grbl.events.clear();
        check(s->connectPort(QString::fromLatin1(name)), "locked reconnect: connects");
        spin(1500, [&] { return panel.statusJson().value("state").toString() == "Alarm"; });
        check(s->maybeLocked(), "locked reconnect: maybe locked before any answer");
        button(QStringLiteral("Initialize machine (home)"))->click();
        spin(3000, [&] { return grbl.count("$H") > 0 && s->pendingCommands() == 0 && !panel.isBusy(); });
        const int reset = grbl.events.indexOf("^X"), home = grbl.indexOf("$H");
        check(reset >= 0 && home > reset && s->pendingCommands() == 0,
              "locked reconnect: Initialize resets, then homes");
        spin(1500, [&] { return grbl.count("G53 G0 X") > 0 && !panel.isBusy(); });
        s->disconnectPort();
        wait(300);

        // Left alone, the unanswered $$ in Alarm gives the lock away.
        grbl.locked = true;
        grbl.alarm = true;
        check(s->connectPort(QString::fromLatin1(name)), "locked reconnect: connects again");
        spin(5000, [&] { return s->needsReset(); });
        check(s->needsReset() && s->pendingCommands() == 0,
              "locked reconnect: an unanswered line in Alarm means locked, nothing left pending");
        grbl.events.clear();
        button(QStringLiteral("Unlock"))->click();
        spin(2000, [&] { return grbl.count("$X") > 0 && s->pendingCommands() == 0; });
        const int reset2 = grbl.events.indexOf("^X"), unlock = grbl.indexOf("$X");
        check(reset2 >= 0 && unlock > reset2 && s->pendingCommands() == 0 && !s->needsReset(),
              "locked reconnect: Unlock resets, then unlocks");
        s->disconnectPort();
        wait(300);

        // A $X that goes unanswered in Alarm is not left pending: once the
        // lock shows, it is sent again after a reset.
        grbl.locked = true;
        grbl.alarm = true;
        grbl.events.clear();
        check(s->connectPort(QString::fromLatin1(name)), "locked reconnect: connects a third time");
        s->sendCommand(QStringLiteral("$X"));
        spin(5000, [&] { return grbl.count("$X") >= 2 && s->pendingCommands() == 0 && !s->needsReset()
                                && panel.statusJson().value("state").toString() == "Idle"; });
        const int reset3 = grbl.events.indexOf("^X");
        check(reset3 >= 0 && grbl.indexOf("$X", reset3) > reset3 && s->pendingCommands() == 0
                  && !s->needsReset() && panel.statusJson().value("state").toString() != "Alarm",
              "locked reconnect: an unanswered $X is retried after a reset");
        s->disconnectPort();
    }

    answerer.stop();
    ::close(slave);
    ::close(master);
    QSettings().clear();
    std::printf("test_machine: %d checks OK\n", g_checks);
    return 0;
}
