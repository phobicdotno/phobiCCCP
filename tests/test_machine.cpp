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
// feed hold), acks every line except a dwell when `hangDwell` and a jog when
// not `ackJogs`. Everything it receives goes into `events`, realtime bytes
// as "!", "~" and "^X".
struct FakeGrbl {
    int fd = -1;
    QByteArray line;
    bool held = false;
    bool hangDwell = false;
    bool ackJogs = true;
    double mx = -100, my = -100, mz = -30;
    // A jog reports Jog for `jogMs` after its line (it does not move the
    // position: tests set that), or until a jog cancel.
    int jogMs = 300;
    QElapsedTimer jogClock;
    QList<QByteArray> events;

    void send(const QByteArray &s) { (void)!::write(fd, s.constData(), size_t(s.size())); }
    QByteArray num(double v) const { return QByteArray::number(v, 'f', 3); }

    void onRead()
    {
        char buf[512];
        const ssize_t n = ::read(fd, buf, sizeof buf);
        for (ssize_t i = 0; i < n; ++i) {
            const char c = buf[i];
            if (c == '?') {
                const bool jog = jogClock.isValid() && jogClock.elapsed() < jogMs;
                send("<" + QByteArray(held ? "Hold:0" : jog ? "Jog" : "Idle") + "|MPos:" + num(mx) + ',' + num(my)
                     + ',' + num(mz) + "|FS:0,0|WCO:0.000,0.000,0.000>\r\n");
            } else if (c == '!') {
                held = true;
                events << "!";
            } else if (c == '~') {
                held = false;
                events << "~";
            } else if (uchar(c) == 0x85) {
                jogClock.invalidate();
            } else if (c == 0x18) {
                held = false;
                jogClock.invalidate();
                line.clear();
                events << "^X";
                send("\r\nGrbl 1.1h ['$' for help]\r\n");
            } else if (c == '\n') {
                events << line;
                if (line.startsWith("$J="))
                    jogClock.start();
                const bool hang = (hangDwell && line.startsWith("G4"))
                                  || (!ackJogs && line.startsWith("$J="));
                if (!hang)
                    send("ok\r\n");
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

    auto spin = [&](int ms, const std::function<bool()> &until) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms && !until())
            QApplication::processEvents(QEventLoop::AllEvents, 20);
    };
    auto key = [&](int k, bool press) {
        QKeyEvent e(press ? QEvent::KeyPress : QEvent::KeyRelease, k, Qt::NoModifier);
        QApplication::sendEvent(&panel, &e);
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
    spin(800, [] { return false; });

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
        spin(800, [] { return false; });
        grbl.mz = -10;
    }

    // Hold-jog lookahead: the machine reports it is not getting anywhere
    // (planner full, status lagging); holding Z- must not keep queueing.
    {
        spin(800, [] { return false; });
        const int before = grbl.count("$J=");
        grbl.jogClock.invalidate();
        grbl.jogMs = 100000;                 // jogging, and getting nowhere
        key(Qt::Key_PageDown, true);
        spin(1600, [] { return false; });
        const int sent = grbl.count("$J=") - before;
        key(Qt::Key_PageDown, false);
        grbl.jogMs = 300;
        check(sent >= 1 && sent <= 4, "jog: a hold queues only a little ahead of the machine");
        spin(1500, [] { return false; });
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
        spin(500, [] { return false; });
        check(s->isParkedForTool(), "manual zero: the program stays parked after Close");

        const int g10 = grbl.count("G10");
        button(QStringLiteral("Zero X and Y here"))->click();
        button(QStringLiteral("Zero all"))->click();
        spin(400, [] { return false; });
        check(grbl.count("G10") == g10, "zero: XY is refused while a job is parked");
        button(QStringLiteral("Zero"), QStringLiteral("work Z0"))->click();
        spin(600, [&] { return grbl.count("G10 L20 P1 Z0") > 0; });
        check(grbl.count("G10 L20 P1 Z0") == 1, "manual zero: Z can be zeroed for the new bit");

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
        spin(400, [] { return false; });
        const int g10 = grbl.count("G10");
        button(QStringLiteral("Zero"), QStringLiteral("work Z0"))->click();
        spin(300, [] { return false; });
        check(grbl.count("G10") == g10, "zero: refused while a job runs");

        // Cancel during the dwell: GRBL says Idle, but the job is about to
        // carry on: the hold must go out before the question.
        grbl.events.clear();
        answer = [&](const Seen &) { return QStringLiteral("Keep running"); };
        button(QStringLiteral("✕ Cancel job"))->click();
        spin(500, [] { return false; });
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
        spin(500, [] { return false; });
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
        spin(1000, [] { return false; });
        grbl.ackJogs = false;
        const int before = grbl.count("$J=");
        for (int i = 0; i < 6; ++i) {
            key(Qt::Key_Left, true);
            key(Qt::Key_Left, false);
            spin(60, [] { return false; });
        }
        spin(300, [] { return false; });
        check(grbl.count("$J=") - before <= 2, "jog: taps stop while two are unanswered");
        grbl.ackJogs = true;
        spin(4000, [&] { return s->pendingCommands() == 0; });
    }

    // Disconnect mid-job: asked, then hold, reset, and only then closed.
    {
        grbl.hangDwell = true;
        s->startStream({QStringLiteral("M3S1000"), QStringLiteral("G4P3.00"), QStringLiteral("G0X1")});
        spin(400, [] { return false; });
        grbl.events.clear();
        seen.clear();
        answer = [&](const Seen &) { return QStringLiteral("Keep running"); };
        button(QStringLiteral("Disconnect"))->click();
        spin(400, [] { return false; });
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

    answerer.stop();
    ::close(slave);
    ::close(master);
    QSettings().clear();
    std::printf("test_machine: %d checks OK\n", g_checks);
    return 0;
}
