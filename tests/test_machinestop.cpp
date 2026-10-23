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
#include "../src/surfacemap.h"
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
    bool homeOkLate = false, pendingHomeOk = false; QList<QByteArray> queued;
    int heldOks = 0;
    QByteArray reject;
    bool rejectHome = false;               // answer $H with error:5 (homing disabled)
    QByteArray alarmOn;
    bool locked = false;      // after ALARM:1: deaf until ctrl-X
    bool alarm = false;       // Alarm state: G-code answered error:9
    double probeZ = -50;
    double wcoX = -200, wcoY = -200;   // a work zero inside the travel (Run checks it)
    double wcoZ = -40;                    // work Z offset the status reports carry (zero under the switch)
    int wcoEvery = 0, wcoCount = 0;       // >0: WCO only on the first report and every Nth (real GRBL)
    // A V1 block model. Probes and G0 moves track the position.
    bool geo = false, rel = false, noiseOnce = false;
    QByteArray rebootOn;   // the controller reboots (brown-out, USB stall) on this line
    double bx = -130, by = -130, topZ = -40, r = 3.175, minZ = 1e9;
    double val(const QByteArray &l, char ax, bool &has) const {
        const int i = l.indexOf(ax); has = i >= 0; if (!has) return 0;
        int j = i + 1; while (j < l.size() && (l[j]=='-'||l[j]=='.'||(l[j]>='0'&&l[j]<='9'))) ++j;
        return l.mid(i + 1, j - i - 1).toDouble(); }
    bool geoLine(const QByteArray &l, QByteArray &reply) {
        if (l.contains("G91")) rel = true;
        if (l.contains("G90")) rel = false;
        const bool g53 = l.contains("G53");
        const bool probe = l.contains("G38.");
        if (!probe && !l.contains("G0") && !l.contains("G1")) return false;
        bool hx, hy, hz; const double X = val(l, 'X', hx), Y = val(l, 'Y', hy), Z = val(l, 'Z', hz);
        double tx = mx, ty = my, tz = mz;
        if (hx) tx = (rel && !g53) ? mx + X : X;
        if (hy) ty = (rel && !g53) ? my + Y : Y;
        if (hz) tz = (rel && !g53) ? mz + Z : Z;
        if (!probe) { mx = tx; my = ty; mz = tz; minZ = qMin(minZ, mz); return true; }
        bool hit = false; double cx = tx, cy = ty, cz = tz;
        const bool overBlock = mx >= bx && mx <= bx + 60 && my >= by && my <= by + 60;
        if (hz && tz < mz && overBlock && mz >= topZ && tz <= topZ) { hit = true; cz = topZ; }
        if (hx && tx > mx && mz < topZ && my >= by && my <= by + 60) {
            if (noiseOnce && l.contains("G38.3")) { noiseOnce = false; hit = true; cx = mx + 2; }
            else if (mx <= bx - r && tx >= bx - r) { hit = true; cx = bx - r; }
        }
        if (hy && ty > my && mz < topZ && mx >= bx && mx <= bx + 60 && my <= by - r && ty >= by - r) { hit = true; cy = by - r; }
        mx = cx; my = cy; mz = cz; minZ = qMin(minZ, mz);
        send("[PRB:" + num(mx) + "," + num(my) + "," + num(mz) + (hit ? ":1]\r\n" : ":0]\r\n"));
        if (!hit && l.contains("G38.2")) { send("ALARM:5\r\n"); alarm = true; reply = "ok\r\n"; }
        events << ("AT " + num(mx) + "," + num(my) + "," + num(mz));
        return true;
    }
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
                if (pendingHomeOk && !home) { pendingHomeOk = false; send("ok\r\n<Idle|MPos:" + num(mx) + ',' + num(my) + ',' + num(mz) + "|FS:0,0|WCO:0.000,0.000,0.000>\r\n");
                    for (const QByteArray &q : std::exchange(queued, {})) { if (q.startsWith("$J=")) jogClock.start(); send(q.startsWith("$J=") || !jogging() ? "ok\r\n" : "error:9\r\n"); }
                    continue; }
                send("<" + QByteArray(locked || alarm ? "Alarm" : held ? (holdClock.isValid() && holdClock.elapsed() < holdSettleMs ? "Hold:1" : "Hold:0") : home ? "Home" : jogging() ? "Jog" : running ? "Run" : "Idle")
                     + "|MPos:" + num(mx) + ',' + num(my) + ',' + num(mz)
                     + "|FS:0,0" + (pins.isEmpty() ? QByteArray() : "|Pn:" + pins) + (wcoEvery > 0 && wcoCount-- > 0 ? QByteArray() : (wcoCount = wcoEvery - 1, "|WCO:" + num(wcoX) + "," + num(wcoY) + "," + num(wcoZ))) + ">\r\n");
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
            } else if (uchar(c) >= 0x90 && uchar(c) <= 0x9D) {
                events << ("OVR" + QByteArray::number(uchar(c), 16));
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
                if (pendingHomeOk) { queued << line; line.clear(); continue; }
                if (locked) {                        // read by nobody
                    line.clear();
                    continue;
                }
                QByteArray reply = "ok\r\n";
                if (!rebootOn.isEmpty() && line.startsWith(rebootOn)) {
                    rebootOn.clear();
                    mx = my = mz = 0; alarm = true; events << "REBOOT";
                    send("\r\nGrbl 1.1h ['$' for help]\r\n[MSG:'$H'|'$X' to unlock]\r\n");
                    line.clear();
                    continue;
                }
                if (!alarmOn.isEmpty() && line.startsWith(alarmOn)) {
                    alarmOn.clear();
                    locked = true;
                    send("ALARM:1\r\n[MSG:Reset to continue]\r\n");
                    line.clear();
                    continue;
                }
                if (line == "$$" && (held || running))
                    reply = "error:8\r\n";   // GRBL refuses $$ in a hold or a cycle
                else if (line == "$$")
                    send(travel);
                else if (line == "$H" && rejectHome)
                    reply = "error:5\r\n";
                else if (line == "$H") {
                    homeClock.start();
                    homeReports = 2;
                    alarm = false;
                    if (homeOkLate) { pendingHomeOk = true; line.clear(); continue; }
                } else if (line == "$X")
                    alarm = false;
                else if (alarm && !line.isEmpty() && line[0] != '$')
                    reply = "error:9\r\n";
                else if (geo && geoLine(line, reply))
                    ;
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

    if (qgetenv("R4MODE") == "connrun") {
        // Still finishing the buffered moves of a job from before the link dropped.
        grbl.running = true;
        grbl.travel += "$13=1\r\n";
        grbl.mx = -100 / 25.4; grbl.my = -100 / 25.4; grbl.mz = -30 / 25.4;
        grbl.wcoX = -200 / 25.4; grbl.wcoY = -200 / 25.4; grbl.wcoZ = -60 / 25.4;
        QTimer::singleShot(2000, [&] { grbl.running = false; });
    }
    if (qgetenv("R4MODE") == "connhold")
        grbl.held = true;   // left in a feed hold mid-job (the USB link dropped)
    if (qgetenv("R4MODE") == "inch13wco") {
        // $13=1 with WCO only every 10th report: the offset that came before
        // $$ was answered is read in inches too.
        grbl.travel += "$13=1\r\n";
        grbl.wcoEvery = 10;
        grbl.mx = -100 / 25.4; grbl.my = -100 / 25.4; grbl.mz = -30 / 25.4;
        grbl.wcoX = -200 / 25.4; grbl.wcoY = -200 / 25.4; grbl.wcoZ = -60 / 25.4;
    }
    if (qgetenv("R4MODE") == "inch13") {
        // $13=1: positions reported in inches. Machine (-100,-100,-30) mm,
        // work zero (-200,-200,-60) mm.
        grbl.travel += "$13=1\r\n";
        grbl.mx = -100 / 25.4; grbl.my = -100 / 25.4; grbl.mz = -30 / 25.4;
        grbl.wcoX = -200 / 25.4; grbl.wcoY = -200 / 25.4; grbl.wcoZ = -60 / 25.4;
    }
    check(s->connectPort(QString::fromLatin1(name)), "connects to the fake controller");
    spin(1500, [&] { return panel.statusJson().value("homed").toBool(); });
    if (qgetenv("R4MODE") != "connrun")   // moving: not where it was left
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
    if (mode == "envxy" || mode == "envz") {
        // A job past the travel at the current zero is refused: with the XY
        // zero at the switches it ran to machine X +48; with Z zeroed 30 mm
        // under the switch, an air cut lifted 30 mm rapided past it.
        if (mode == "envxy") {
            grbl.wcoX = 0;
            grbl.wcoY = 0;
            grbl.wcoZ = -60;
        } else {
            grbl.wcoZ = -30;
            grbl.wcoX = -200;
            grbl.wcoY = -200;
            panel.m_airCut->setChecked(true);
            panel.m_airLift->setValue(30);
        }
        wait(800);
        grbl.events.clear();
        bool refused = false;
        answer = [&](const Seen &v) {
            if (v.headline.startsWith("The program goes off the machine")) {
                refused = true;
                return QStringLiteral("OK");
            }
            if (v.headline.startsWith("Insert tool"))
                return QStringLiteral("Same length: continue");
            return QStringLiteral("\u25b6 Start");
        };
        const bool started = panel.runProgram();
        wait(1500);
        check(refused && !started && !s->isStreaming(), mode == "envxy" ? "envxy: a job past the X/Y travel is refused"
                                                             : "envz: an air cut lifted past the Z switch is refused");
        // Inside the travel the same job runs.
        grbl.wcoX = -200;
        grbl.wcoY = -200;
        grbl.wcoZ = -60;
        panel.m_airCut->setChecked(false);
        wait(800);
        const bool ok = panel.runProgram();
        check(ok, "env: the job inside the travel runs");
        s->stopStream();
        wait(500);
    }
    if (mode == "connrun") {
        // Connected while GRBL still ran moves from before (it answers $$
        // with error:8): the listing was never read, so $13=1 positions
        // stayed 25.4 times too small all session.
        spin(8000, [&] { return s->haveSettings(); });
        check(s->haveSettings(), "connrun: the $$ listing is read once the leftover moves end");
        check(std::abs(s->status().wx - 100) < 0.01, "connrun: and positions read in mm");
    }
    if (mode == "inch13wco") {
        // The offset reported before $$ (in inches) was read as mm: Run saw
        // the job 25 times nearer zero and refused one that fits.
        bool refused = false;
        answer = [&](const Seen &v) {
            if (v.headline.startsWith(QLatin1String("The program goes off the machine"))) {
                refused = true;
                return QStringLiteral("OK");
            }
            return QStringLiteral("\u25b6 Start");
        };
        check(std::abs(s->status().wx - 100) < 0.01 && std::abs(s->status().wz - 30) < 0.01,
              "inch13wco: work position from the early offset in mm");
        const bool ok = panel.runProgram();
        check(ok && !refused, "inch13wco: a job inside the travel runs right after connect");
        if (ok) {
            s->stopStream();
            wait(500);
        }
    }
    if (mode == "inch13") {
        // Read as mm, a jog went 86 mm the wrong way and Run refused a job
        // that fits.
        check(std::fabs(s->status().mx + 100) < 0.01 && std::fabs(s->status().wx - 100) < 0.01,
              "inch13: inch reports read in mm");
        bool refused = false;
        answer = [&](const Seen &v) {
            if (v.headline.startsWith("The program goes off the machine")) {
                refused = true;
                return QStringLiteral("OK");
            }
            return QStringLiteral("\u25b6 Start");
        };
        const bool ok = panel.runProgram();
        check(ok && !refused, "inch13: a job inside the travel runs with $13=1");
        if (ok) {
            s->stopStream();
            wait(500);
        }
        wait(800);
        grbl.events.clear();
        panel.forgetJogPlan();
        panel.jogBy('X', -10, 1000);
        wait(300);
        check(lastJog().contains("X-110"), "inch13: a 10 mm X- jog goes to X-110");
    }
    if (mode == "connhold") {
        // Connected to a controller paused in a job this session never sent:
        // its leftover moves are cleared, so no Resume can run them (GRBL
        // answers $$ with error:8 while held).
        spin(5000, [&] { return grbl.events.contains("^X") && !grbl.held; });
        check(grbl.events.contains("^X") && !grbl.held, "connhold: a held job left from before is cleared on connect");
        check(!panel.m_machineState.startsWith(QLatin1String("Hold")), "connhold: not left in a hold");
    }
    if (mode == "maphigh") {
        // Z zeroed high (tall stock, short bit): the map's travel height,
        // plate top plus travel in work Z, lands past the Z switch.
        panel.m_bzPanel->m_modes->button(c2d::BitZeroPanel::Map)->click();
        grbl.wcoZ = -15;
        wait(600);
        grbl.events.clear();
        panel.bitZeroBegin();
        wait(1500);
        check(grbl.indexOf("G0 Z") < 0 && !panel.m_mapper->isRunning(),
              "maphigh: a surface map that would travel past the Z switch is refused");
        grbl.wcoZ = -60;
        wait(600);
        panel.bitZeroBegin();
        wait(1500);
        check(grbl.indexOf("G0 Z") >= 0, "maphigh: with Z zeroed low enough the map starts");
    }
    if (mode == "mapzeropad") {
        // r45: Zero Z with the map on at work X80 Y80 (surface there 1.75 above X0 Y0),
        // then the pad's start: refused until the map is switched off; off,
        // the job runs with Z0 1.75 mm under the surface that was touched.
        panel.m_bsEnabled->setChecked(false);
        c2d::SurfaceMap m;
        m.x0 = 10; m.y0 = 10; m.x1 = 90; m.y1 = 90; m.cols = 2; m.rows = 2;
        m.z = {-40.0, -39.0, -39.0, -38.0};
        panel.setSurfaceMap(m);
        panel.m_useMap->setChecked(true);
        grbl.mx = grbl.wcoX + 80;
        grbl.my = grbl.wcoY + 80;
        wait(800);
        grbl.events.clear();
        panel.zero(QStringLiteral("Z"));
        wait(800);
        const bool sent175 = grbl.indexOf("G10 L20 P1 Z1.750") >= 0;
        std::printf("mapzeropad: zero sent G10 Z1.750: %d\n", sent175);
        // what the controller now has: work Z reads 1.75 with the bit on the stock
        grbl.wcoZ = grbl.mz - 1.75;
        wait(800);
        QString why;
        bool started = panel.startFromRemote(&why);
        std::printf("mapzeropad: pad start with map on: %d (%s)\n", started, qPrintable(why));
        panel.m_useMap->setChecked(false);      // as the refusal says
        wait(300);
        answer = [&](const Seen &v) { std::printf("prompt: %s\n", qPrintable(v.headline)); return QStringLiteral("\u25b6 Start"); };
        why.clear();
        started = panel.startFromRemote(&why);
        std::printf("mapzeropad: pad start with map off: %d (%s), work Z at the touched surface %.3f\n",
                    started, qPrintable(why), s->status().wz);
        check(!(sent175 && started), "mapzeropad: the pad's start does not run with Z0 1.75 mm under the touched surface");
        if (started) { s->stopStream(); wait(500); }
        // And the other way: zeroed with the map off, then the map on.
        grbl.events.clear();
        panel.zero(QStringLiteral("Z"));
        wait(800);
        grbl.wcoZ = grbl.mz;
        panel.m_useMap->setChecked(true);
        wait(300);
        why.clear();
        started = panel.startFromRemote(&why);
        std::printf("mapzeropad: pad start, zeroed with the map off, map on: %d (%s)\n", started, qPrintable(why));
        check(!started, "mapzeropad: a start with the map on after a zero with it off is refused");
        if (started) { s->stopStream(); wait(500); }
    }
    if (mode == "mapremeasure") {
        // r46: yesterday's map still on, a new board: Zero Z at work X80 Y80
        // takes the old map's offset there (1.75), then the new board is
        // mapped (flat) and the job run with it.
        panel.m_bsEnabled->setChecked(false);
        c2d::SurfaceMap m;
        m.x0 = 10; m.y0 = 10; m.x1 = 90; m.y1 = 90; m.cols = 2; m.rows = 2;
        m.z = {-40.0, -39.0, -39.0, -38.0};
        panel.setSurfaceMap(m);
        panel.m_useMap->setChecked(true);
        grbl.mx = grbl.wcoX + 80;
        grbl.my = grbl.wcoY + 80;
        wait(800);
        grbl.events.clear();
        panel.zero(QStringLiteral("Z"));
        wait(800);
        const bool sent175 = grbl.indexOf("G10 L20 P1 Z1.750") >= 0;
        std::printf("mapremeasure: zero sent Z1.750: %d\n", sent175);
        grbl.wcoZ = grbl.mz - 1.75;
        c2d::SurfaceMap flat = m;
        flat.z = {-40.0, -40.0, -40.0, -40.0};
        panel.setSurfaceMap(flat);           // as SurfaceMapProber::finished does
        panel.m_useMap->setChecked(true);
        wait(300);
        answer = [&](const Seen &v) {
            std::printf("prompt: %s | %s\n", qPrintable(v.headline), qPrintable(v.body));
            return v.headline.startsWith(QStringLiteral("Stream")) ? QStringLiteral("\u25b6 Start") : QStringLiteral("OK");
        };
        const bool started = panel.runProgram();
        std::printf("mapremeasure: run with the new map: %d, work Z of the touched surface %.3f\n",
                    started, s->status().wz);
        check(!started, "mapremeasure: a zero taken from the old map is not run with the new one");
        if (started) { s->stopStream(); wait(500); }
    }
    if (mode == "mapzeroreconn") {
        // r46: Zero Z with the map on at work X80 Y80 (sent Z1.750), then the
        // USB link drops / the app restarts; reconnect (homed resumes), map
        // switched off as the pad's refusal asks, pad start.
        panel.m_bsEnabled->setChecked(false);
        c2d::SurfaceMap m;
        m.x0 = 10; m.y0 = 10; m.x1 = 90; m.y1 = 90; m.cols = 2; m.rows = 2;
        m.z = {-40.0, -39.0, -39.0, -38.0};
        panel.setSurfaceMap(m);
        panel.m_useMap->setChecked(true);
        grbl.mx = grbl.wcoX + 80;
        grbl.my = grbl.wcoY + 80;
        wait(800);
        grbl.events.clear();
        panel.zero(QStringLiteral("Z"));
        wait(800);
        const bool sent175 = grbl.indexOf("G10 L20 P1 Z1.750") >= 0;
        grbl.wcoZ = grbl.mz - 1.75;
        wait(800);
        s->disconnectPort();
        wait(500);
        QSettings().setValue(QStringLiteral("machine/homedAt"), QVariantList{grbl.mx, grbl.my, grbl.mz});
        check(s->connectPort(QString::fromLatin1(name)), "reconnects");
        spin(1500, [&] { return panel.statusJson().value("homed").toBool(); });
        wait(1000);
        std::printf("mapzeroreconn: homed after reconnect: %d, map still on: %d\n",
                    panel.statusJson().value("homed").toBool(), panel.m_useMap->isChecked());
        panel.m_useMap->setChecked(false);
        wait(300);
        answer = [&](const Seen &v) { std::printf("prompt: %s\n", qPrintable(v.headline)); return QStringLiteral("\u25b6 Start"); };
        QString why;
        const bool started = panel.startFromRemote(&why);
        std::printf("mapzeroreconn: sent175 %d, pad start with map off after reconnect: %d (%s), work Z at touched surface %.3f\n",
                    sent175, started, qPrintable(why), s->status().wz);
        check(!(sent175 && started), "mapzeroreconn: pad start after a reconnect does not run 1.75 mm deep");
        if (started) { s->stopStream(); wait(500); }
    }
    if (mode == "maptlo") {
        // r47: Zero Z with the map on at work X80 Y80 (sent Z1.750), then a
        // bit is measured (or a job's tool change measured one): G43.1 Z0.4.
        // GRBL's WCO includes the tool length offset (report.c), so the
        // reported WCO Z is the G54 Z + 0.4. Map off, pad start.
        panel.m_bsEnabled->setChecked(false);
        c2d::SurfaceMap m;
        m.x0 = 10; m.y0 = 10; m.x1 = 90; m.y1 = 90; m.cols = 2; m.rows = 2;
        m.z = {-40.0, -39.0, -39.0, -38.0};
        panel.setSurfaceMap(m);
        panel.m_useMap->setChecked(true);
        grbl.mx = grbl.wcoX + 80;
        grbl.my = grbl.wcoY + 80;
        wait(800);
        grbl.events.clear();
        panel.zero(QStringLiteral("Z"));
        wait(800);
        const bool sent175 = grbl.indexOf("G10 L20 P1 Z1.750") >= 0;
        grbl.wcoZ = grbl.mz - 1.75;
        wait(800);
        s->applyToolLengthOffset(0.4);
        wait(800);
        const bool g431 = grbl.indexOf("G43.1 Z0.400") >= 0;
        grbl.wcoZ = grbl.mz - 1.75 + 0.4;   // as GRBL reports it with G43.1 active
        wait(800);
        panel.m_useMap->setChecked(false);
        wait(300);
        answer = [&](const Seen &v) { std::printf("prompt: %s\n", qPrintable(v.headline)); return QStringLiteral("\u25b6 Start"); };
        QString why;
        const bool started = panel.startFromRemote(&why);
        std::printf("maptlo: sent175 %d, G43.1 sent %d, pad start with map off: %d (%s)\n",
                    sent175, g431, started, qPrintable(why));
        check(!(sent175 && started), "maptlo: a tool length offset does not hide a zero taken with the map on");
        if (started) { s->stopStream(); wait(500); }
    }
    if (mode == "mapzero") {
        // Surface map on, Z zeroed at work X80 Y80 (a re-zero after a tool
        // change): Z0 must stay the surface at X0 Y0 the offsets are from,
        // or the job cuts 1.75 mm off over a 2 mm warp.
        panel.m_bsEnabled->setChecked(false);
        c2d::SurfaceMap m;
        m.x0 = 10; m.y0 = 10; m.x1 = 90; m.y1 = 90; m.cols = 2; m.rows = 2;
        m.z = {-40.0, -39.0, -39.0, -38.0};
        panel.setSurfaceMap(m);
        panel.m_useMap->setChecked(true);
        grbl.mx = grbl.wcoX + 80;
        grbl.my = grbl.wcoY + 80;
        wait(800);
        grbl.events.clear();
        panel.zero(QStringLiteral("Z"));
        wait(800);
        check(grbl.indexOf("G10 L20 P1 Z1.750") >= 0, "mapzero: Zero Z with the map on keeps Z0 at the surface under X0 Y0");
        panel.m_useMap->setChecked(false);
        grbl.events.clear();
        panel.zero(QStringLiteral("Z"));
        wait(800);
        check(grbl.events.contains("G10 L20 P1 Z0"), "mapzero: with the map off it is Z0 there");
        panel.setSurfaceMap(c2d::SurfaceMap());
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
    // Round 16: a settled machine starts; Rehearse, Park, Go to, Measure and
    // BitZero refuse during a tap with the reason, not error:9.
    if (mode == "jogsettle" || mode == "jogsettlerun" || mode == "rerun") {
        grbl.jogMs = 300;
        wait(600);
        if (mode == "rerun") {
            panel.m_endOfJobPending = true;
            s->startStream({QStringLiteral("G21"), QStringLiteral("G1X5F500"), QStringLiteral("G0Z-5")});
            spin(8000, [&] { return !s->isStreaming() && !panel.isBusy(); });
            std::printf("after job: state %s pending %d busy %d\n", qPrintable(panel.m_machineState), s->pendingCommands(), panel.isBusy());
        } else {
            panel.m_step->setCurrentIndex(panel.m_step->count() - 1);
            padPress(4);
            wait(1500);
        }
        std::printf("still %d state %s pending %d\n", panel.standingStill(), qPrintable(panel.m_machineState), s->pendingCommands());
        bool started = false;
        QString why;
        if (mode == "jogsettlerun") {
            answer = [&](const Seen &v) { std::printf("prompt: %s\n", qPrintable(v.headline)); return QStringLiteral("\u25b6 Start"); };
            started = panel.runProgram();
        } else {
            started = panel.startFromRemote(&why);
        }
        std::printf("start after settle: %d (%s) streaming %d\n", started, qPrintable(why), s->isStreaming());
        check(started && s->isStreaming(), "a start after the machine settled is accepted");
        s->stopStream();
        wait(800);
    }
    if (mode == "rehtap" || mode == "parktap" || mode == "bzjog" || mode == "measuretap" || mode == "gototap") {
        grbl.jogMs = 800;
        panel.m_bsEnabled->setChecked(true);
        panel.m_haveRef = true;
        panel.m_refZ = -50;
        grbl.probeZ = -48;
        panel.m_tcX->setValue(-120.0);
        panel.m_tcY->setValue(-120.0);
        panel.m_bzPanel->m_modes->button(c2d::BitZeroPanel::ZOnly)->click();
        wait(600);
        panel.m_step->setCurrentIndex(panel.m_step->count() - 1);
        grbl.events.clear();
        answer = [&](const Seen &v) {
            std::printf("prompt: %s | %s\n", qPrintable(v.headline), qPrintable(v.body.left(120)));
            if (v.headline.contains("Start the rehearsal")) { padPress(4); return QStringLiteral("Start rehearsal"); }
            if (v.headline.startsWith("Insert tool")) return QStringLiteral("Measure and continue");
            return QStringLiteral("OK");
        };
        if (mode == "rehtap") panel.rehearseToolChange();
        else {
            padPress(4);
            if (mode == "parktap") panel.park();
            else if (mode == "bzjog") panel.bitZeroBegin();
            else if (mode == "gototap") panel.goToXY0();
            else panel.measureTool(c2d::MachinePanel::Phase::MeasureManual);
        }
        wait(4000);
        for (const QByteArray &e : grbl.events) std::printf("  sent: %s\n", e.constData());
        for (QPlainTextEdit *t : panel.findChildren<QPlainTextEdit *>())
            std::printf("LOG:\n%s\n", qPrintable(t->toPlainText().right(1500)));
        QString logText;
        for (QPlainTextEdit *t : panel.findChildren<QPlainTextEdit *>())
            logText += t->toPlainText();
        check(!logText.contains("error:9") && logText.contains("stands still"),
              "refused with the reason while a jog tap still moves");
    }
    if (mode == "homepark" || mode == "homepark0") {
        if (mode == "homepark") s->applyToolLengthOffset(1.5);
        wait(600);
        grbl.homeOkLate = true;
        grbl.events.clear();
        panel.initializeMachine();
        wait(3000);
        for (const QByteArray &e : grbl.events) std::printf("  sent: %s\n", e.constData());
        for (QPlainTextEdit *t : panel.findChildren<QPlainTextEdit *>())
            std::printf("LOG:\n%s\n", qPrintable(t->toPlainText().right(800)));
        check(grbl.count("G53 G0 X") == 1, "park after homing goes out (TLO re-sent after $H)");
    }
    if (mode == "homejog") {
        s->applyToolLengthOffset(1.5);
        wait(600);
        panel.m_homed = false;
        grbl.jogMs = 1500;
        grbl.homeOkLate = true;
        panel.m_step->setCurrentIndex(panel.m_step->count() - 1);
        grbl.events.clear();
        panel.initializeMachine();
        wait(150);
        panel.jogStep('X', -1);
        wait(4000);
        for (const QByteArray &e : grbl.events) std::printf("  sent: %s\n", e.constData());
        for (QPlainTextEdit *t : panel.findChildren<QPlainTextEdit *>())
            std::printf("LOG:\n%s\n", qPrintable(t->toPlainText().right(1200)));
        wait(3000);
        std::printf("TLO app=%.3f  G43.1 sent=%d\n", s->toolLengthOffset(), grbl.count("G43.1"));
        check(grbl.count("$J=") == 0, "a jog tapped while homing is refused");
        check(grbl.count("G43.1") >= 1, "the tool offset goes out after homing");
    }
    if (mode == "tloretry") {
        // A G43.1 the controller refuses is sent again once it is Idle.
        wait(600);
        grbl.reject = "G43.1";
        s->applyToolLengthOffset(1.5);
        wait(400);
        const int refused = grbl.count("G43.1");
        grbl.reject.clear();
        wait(1500);
        std::printf("G43.1 refused %d, sent in all %d\n", refused, grbl.count("G43.1"));
        check(refused >= 1 && grbl.count("G43.1") > refused, "a refused tool offset is sent again");
    }
    if (mode == "v1miss") {
        grbl.geo = true;
        grbl.noiseOnce = true;            // one false contact on the X side's fast search
        panel.m_bzPanel->m_plates->button(1)->click();     // V1
        panel.m_bzPanel->m_modes->button(c2d::BitZeroPanel::Corner)->click();
        wait(600);
        const double stockTop = grbl.topZ - 22.0;
        grbl.events.clear();
        panel.bitZeroBegin();
        wait(5000);
        for (const QByteArray &e : grbl.events) std::printf("  1st: %s\n", e.constData());
        std::printf("after the failed side probe: pin at X%.3f Y%.3f Z%.3f (block X %.1f..%.1f, top %.1f, stock top %.1f), busy=%d\n",
                    grbl.mx, grbl.my, grbl.mz, grbl.bx, grbl.bx + 60, grbl.topZ, stockTop, int(panel.m_bzPanel->m_busy));
        check(grbl.mz > grbl.topZ, "v1miss: after a failed side probe the pin goes back up over the block");
        grbl.events.clear();
        grbl.minZ = 1e9;
        panel.bitZeroBegin();   // Begin again, no jog
        wait(4000);
        for (const QByteArray &e : grbl.events) std::printf("  2nd: %s\n", e.constData());
        std::printf("2nd Begin: lowest pin Z %.3f = %.3f mm below the stock top\n", grbl.minZ, stockTop - grbl.minZ);
        check(grbl.minZ >= stockTop, "v1miss: Begin again does not drive the pin below the stock top");
    }

    if (mode == "v1reboot") {
        grbl.geo = true;
        panel.m_bzPanel->m_plates->button(1)->click();     // V1
        panel.m_bzPanel->m_modes->button(c2d::BitZeroPanel::Corner)->click();
        wait(600);
        grbl.rebootOn = "G0 Z-";          // the side stage's descent beside the block
        grbl.events.clear();
        panel.bitZeroBegin();
        wait(3000);
        std::printf("after reboot: onIdle pending=%d busy=%d\n", int(bool(panel.m_onIdle)), int(panel.isBusy()));
        grbl.events.clear();
        panel.unlock();                   // the operator unlocks the rebooted controller
        wait(3000);
        for (const QByteArray &e : grbl.events) std::printf("  after unlock: %s\n", e.constData());
        std::printf("pin now at machine X%.3f Y%.3f Z%.3f (controller believed 0,0,0 after reboot)\n", grbl.mx, grbl.my, grbl.mz);
        check(grbl.count("G53 G0") == 0, "v1reboot: nothing from before the reboot moves the machine after it");
    }
    if (mode == "homerej") {
        // A refused $H must not leave the park pending (it blocked every jog).
        wait(600);
        grbl.rejectHome = true;
        panel.m_step->setCurrentIndex(0);
        grbl.events.clear();
        panel.initializeMachine();
        wait(1500);
        panel.jogStep('X', -1);
        wait(1500);
        check(!panel.m_parkAfterHome && grbl.count("$J=") == 1, "a jog after a refused $H goes out");
    }
    if (mode == "tlospam") {
        // A G43.1 the controller keeps refusing gets a few tries, not one per report.
        wait(600);
        grbl.reject = "G43.1";
        s->applyToolLengthOffset(1.5);
        wait(3000);
        check(grbl.count("G43.1") <= 4, "a refused G43.1 is not retried endlessly");
    }
    if (mode == "remoteset" || mode == "ovrreset") {
        // Run settings made at the computer that a pad cannot see refuse a
        // pad start (an old surface map, a left-on air cut, a tiled design),
        // and every job starts its overrides at 100 %.
        panel.m_tcX->setValue(-120.0);
        panel.m_tcY->setValue(-120.0);
        wait(600);
        answer = [&](const Seen &v) {
            if (v.headline.startsWith("Insert tool")) return QStringLiteral("Same length: continue");
            return QStringLiteral("OK");
        };
        QString why;
        if (mode == "remoteset") {
            panel.m_airCut->setChecked(true);
            check(!panel.startFromRemote(&why) && why.contains("Air cut"), "a pad start with air cut on is refused");
            panel.m_airCut->setChecked(false);
            c2d::SurfaceMap m;
            m.x0 = 0; m.y0 = 0; m.x1 = 100; m.y1 = 100; m.cols = 2; m.rows = 2;
            m.z = {-50.0, -52.0, -50.0, -52.0};
            panel.setSurfaceMap(m);
            panel.m_useMap->setChecked(true);
            check(!panel.startFromRemote(&why) && why.contains("surface map"), "a pad start with the surface map on is refused");
            panel.m_useMap->setChecked(false);
            doc.setParam("tiling_enabled", "1");
            doc.setParam("tile_height", "5");
            check(!panel.startFromRemote(&why) && why.contains("tiled"), "a tiled design that needs several tiles is refused");
        } else {
            grbl.events.clear();
            check(panel.startFromRemote(&why), "the job starts");
            wait(800);
            const int ovr = grbl.events.indexOf("OVR90");
            int firstMove = -1;
            for (int i = 0; i < grbl.events.size() && firstMove < 0; ++i)
                if (grbl.events.at(i).startsWith("G0") || grbl.events.at(i).startsWith("G1"))
                    firstMove = i;
            check(ovr >= 0 && grbl.events.contains("OVR99") && (firstMove < 0 || ovr < firstMove),
                  "feed and spindle overrides are reset to 100 % before the job's first move");
            spin(20000, [&] { return !s->isStreaming() && !panel.isBusy(); });
        }
    }
    check(!mode.isEmpty(), "a scenario was named (R4MODE)");
    std::printf("test_machinestop %s: %d checks OK\n", mode.constData(), g_checks);
    answerer.stop();
    ::close(slave);
    ::close(master);
    QSettings().clear();
    return 0;
}
