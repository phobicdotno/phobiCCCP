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
#include "../src/jobview.h"
#include "../src/machinepanel.h"
#include "../src/toollibrary.h"
#include "../src/toolstrip.h"

#include <QApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSocketNotifier>
#include <QSpinBox>
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
// G38.2 touches at machine Z `probeZ`. A feed hold reports Hold:1 (still
// slowing down) while `holdSettles` is off. Everything it receives goes into
// `events`, realtime bytes as "!", "~", "^X" and "JOGCANCEL".
struct FakeGrbl {
    int fd = -1;
    QByteArray line;
    bool held = false;
    bool holdSettles = true;
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
    double wcoZ = -40;   // work Z offset in the status reports (zero under the switch)
    // A jog reports Jog for `jogMs` after its line (it does not move the
    // position: tests set that), or until a jog cancel.
    int jogMs = 300;
    QElapsedTimer jogClock, homeClock;
    int homeReports = 0;      // Home is reported at least this often, however slow the poll
    QByteArray travel = "$130=425.000\r\n$131=425.000\r\n$132=75.000\r\n$27=1.500\r\n";
    QList<QByteArray> events;
    int ovRapid = 100, ovSpindle = 100;   // reported as Ov:100,rapid,spindle
    bool flood = false;                   // M8 on, M9 off; reported as A:F

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
                send("<" + QByteArray(locked || alarm ? "Alarm" : held ? (holdSettles ? "Hold:0" : "Hold:1") : home ? "Home" : jogging() ? "Jog" : "Idle")
                     + "|MPos:" + num(mx) + ',' + num(my) + ',' + num(mz)
                     + "|FS:0,0|WCO:-200.000,-200.000," + num(wcoZ)   // XY zero inside the travel
                     + "|Ov:100," + QByteArray::number(ovRapid) + ',' + QByteArray::number(ovSpindle)
                     + (flood ? "|A:F" : "") + ">\r\n");
            } else if (c == '!') {
                held = true;
                events << "!";
            } else if (c == '~') {
                held = false;
                events << "~";
            } else if (uchar(c) >= 0x90 && uchar(c) <= 0x9D) {   // overrides
                const uchar b = uchar(c);
                if (b == 0x95) ovRapid = 100;
                else if (b == 0x96) ovRapid = 50;
                else if (b == 0x97) ovRapid = 25;
                else if (b == 0x99) ovSpindle = 100;
                else if (b == 0x9A) ovSpindle = qMin(200, ovSpindle + 10);
                else if (b == 0x9B) ovSpindle = qMax(10, ovSpindle - 10);
                events << "OV:" + QByteArray::number(b, 16);
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
                if (line == "M8")
                    flood = true;
                else if (line == "M9")
                    flood = false;
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

    // ---- the job plan: toolpaths, passes and times read back from the program
    {
        const QStringList lines = {
            QStringLiteral("G21"), QStringLiteral("G90"), QStringLiteral("G53 G0 Z-2.500"),
            QStringLiteral("(Pocket A)"), QStringLiteral("G0Z5.000"), QStringLiteral("M03S18000"),
            QStringLiteral("G4P3.00"), QStringLiteral("X10.000Y10.000"), QStringLiteral("G1Z-1.500F300.0"),
            QStringLiteral("X20.000F1000.0"), QStringLiteral("Y20.000"), QStringLiteral("X10.000"),
            QStringLiteral("Y10.000"), QStringLiteral("Z-3.000F300.0"), QStringLiteral("X20.000F1000.0"),
            QStringLiteral("Y20.000"), QStringLiteral("X10.000"), QStringLiteral("Y10.000"),
            QStringLiteral("G0Z5.000"), QStringLiteral("M05"), QStringLiteral("M0 ;T2"),
            QStringLiteral("G90G21"), QStringLiteral("G0Z5.000"), QStringLiteral("(Drill B)"),
            QStringLiteral("M03S18000"), QStringLiteral("G4P3.00"), QStringLiteral("X30.000Y30.000"),
            QStringLiteral("G1Z-2.000F200.0"), QStringLiteral("G0Z5.000"), QStringLiteral("X40.000"),
            QStringLiteral("G1Z-2.000F200.0"), QStringLiteral("G0Z5.000")};
        const c2d::JobPlan plan = c2d::planJob(lines, {}, nullptr, 5000);
        check(plan.total == lines.size() - 2, "plan: comments take no queue index");
        check(plan.cumSec.size() == plan.total + 1, "plan: one cumulative time per queued line");
        check(plan.sections.size() == 3, "plan: two toolpaths and a tool change");
        if (plan.sections.size() == 3) {
            const c2d::JobSection &a = plan.sections.at(0), &tc = plan.sections.at(1), &b = plan.sections.at(2);
            check(a.name == QLatin1String("Pocket A") && a.kind == c2d::JobSection::Toolpath, "plan: first toolpath named from its comment");
            check(a.passes.size() == 2 && std::abs(a.passes.at(0).z + 1.5) < 1e-6 && std::abs(a.passes.at(1).z + 3.0) < 1e-6,
                  "plan: a pocket cut in two levels is two passes, at their floors");
            // Pass 1: plunge 6.5 mm at F300 (1.3 s) + 40 mm at F1000 (2.4 s); pass 2: 1.5 mm
            // plunge + the lap + the 8 mm retract rapid at 5000 that ends the toolpath (0.1 s).
            // Speeding up and slowing down at the corners adds a little on top.
            check(a.passes.size() == 2 && a.passes.at(0).estSec > 3.7 && a.passes.at(0).estSec < 3.7 + 0.35
                      && a.passes.at(1).estSec > 2.7 + 8.0 / 5000 * 60
                      && a.passes.at(1).estSec < 2.7 + 8.0 / 5000 * 60 + 0.6,
                  "plan: pass times from feed and distance, with acceleration");
            check(a.estSec > 3.7 + 2.7 + 2.9, "plan: the toolpath's own time counts the spin-up dwell too");
            check(tc.kind == c2d::JobSection::ToolChange && tc.detail.startsWith(QLatin1String("T2")), "plan: the tool change is its own step");
            check(b.name == QLatin1String("Drill B") && b.passes.size() == 1 && std::abs(b.passes.at(0).z + 2.0) < 1e-6,
                  "plan: pecks that only go deeper stay one pass");
            check(a.qStart < a.qEnd && a.qEnd <= tc.qStart && tc.qEnd <= b.qStart && b.qEnd == plan.total,
                  "plan: sections tile the program in order");
        }
        bool monotone = true;
        for (int i = 1; i < plan.cumSec.size(); ++i)
            monotone = monotone && plan.cumSec.at(i) >= plan.cumSec.at(i - 1);
        check(monotone, "plan: time never runs backwards");
    }

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
        // The running job's rows: each toolpath's own cut for its preview
        // box, the bit for its outline, and the pad's copy of the list.
        const c2d::JobPlan plan = c2d::planJob(g.gcode.split(QLatin1Char('\n')), g.ops, &doc, 5000);
        QVector<const c2d::JobSection *> cuts;
        for (const c2d::JobSection &s : plan.sections)
            if (s.kind == c2d::JobSection::Toolpath)
                cuts << &s;
        check(cuts.size() == 2 && cuts.at(0)->type == QLatin1String("pocket_toolpath")
                  && cuts.at(0)->bit.kind == QLatin1String("endmill"),
              "job rows: a toolpath knows its type and bit");
        const QRectF box = cuts.size() == 2 ? cuts.at(0)->cut.boundingRect() : QRectF();
        check(!box.isEmpty() && box.left() >= 10 - 1e-6 && box.right() <= 50 + 1e-6
                  && box.top() >= 10 - 1e-6 && box.bottom() <= 30 + 1e-6,
              "job rows: the cut preview is the pocket's own moves, inside its rectangle");
        bool changeBit = false;
        for (const c2d::JobSection &s : plan.sections)
            changeBit = changeBit || (s.kind == c2d::JobSection::ToolChange && s.bit.diameter > 0);
        check(changeBit, "job rows: a tool change shows the bit it changes to");
        c2d::JobView view;
        view.setPlan(plan, QStringLiteral("job"));
        view.setProgress(cuts.size() == 2 ? (cuts.at(0)->qStart + cuts.at(0)->qEnd) / 2 : 0, plan.total);
        const QJsonArray steps = view.stepsJson();
        bool previewOk = false, runOk = false;
        for (const QJsonValue &v : steps) {
            const QJsonObject o = v.toObject();
            if (o.value("kind").toString() == QLatin1String("toolpath"))
                previewOk = previewOk || !QByteArray::fromBase64(o.value("preview").toString().toLatin1()).isEmpty();
            if (o.value("state").toString() == QLatin1String("run"))
                runOk = o.value("done").toDouble() > 0 && o.value("done").toDouble() < 1;
        }
        check(steps.size() == plan.sections.size() && previewOk && runOk,
              "job rows: the pad gets every step, a preview picture and the running share done");
    }

    // ---- one toolpath on many shapes: a row per shape ---------------------
    // Five squares pocketed 3 mm deep at 1 mm a pass, each cut to full depth
    // before the next: the rows used to be the first square's three passes,
    // with the other four squares all inside "Pass 3 of 3".
    for (const QString type : {QStringLiteral("pocket_toolpath"), QStringLiteral("contour")}) {
        c2d::Document many;
        QJsonArray els;
        for (int i = 0; i < 5; ++i) {
            const c2d::Element sq = c2d::Element::makeRectangle({20.0 + 30 * i, 20}, 20, 20, QJsonObject());
            many.addElement(sq);
            els.append(QJsonObject{{"uuid", sq.id}});
        }
        QJsonObject j;
        j.insert("type", type);
        j.insert("name", QStringLiteral("squares"));
        j.insert("enabled", true);
        j.insert("uuid", QStringLiteral("{tp-many}"));
        j.insert("start_depth", QStringLiteral("0.000"));
        j.insert("end_depth", QStringLiteral("3.000"));
        j.insert("stepdown", 1.0);
        j.insert("stepover", 1.5);
        j.insert("stock_to_leave", 0.0);
        j.insert("speeds", QJsonObject{{"feedrate", 1000}, {"plungerate", 300}, {"rpm", 10000}});
        j.insert("tool", QJsonObject{{"diameter", 3.175}, {"number", 1}, {"name", QStringLiteral("1/8 endmill")}});
        j.insert("elements", els);
        c2d::Toolpath tp;
        tp.uuid = QStringLiteral("{tp-many}");
        tp.type = type;
        tp.json = j;
        many.addToolpath(tp);
        const c2d::GcodeResult g = c2d::exportGcode(many);
        const c2d::JobPlan plan = c2d::planJob(g.gcode.split(QLatin1Char('\n')), g.ops, &many, 5000);
        const c2d::JobSection *sq = nullptr;
        for (const c2d::JobSection &s : plan.sections)
            if (s.kind == c2d::JobSection::Toolpath)
                sq = &s;
        const QString tag = QStringLiteral("many shapes (%1): ").arg(type);
        check(sq && sq->passes.size() == 5, qPrintable(tag + QStringLiteral("one row per square, got %1").arg(sq ? sq->passes.size() : -1)));
        if (!sq || sq->passes.size() != 5)
            continue;
        bool rows = true, tiled = sq->passes.first().qStart >= sq->qStart && sq->passes.last().qEnd == sq->qEnd;
        for (int i = 0; i < 5; ++i) {
            const c2d::JobPass &p = sq->passes.at(i);
            rows = rows && p.what.startsWith(QStringLiteral("Shape %1 of 5").arg(i + 1)) && p.levels.size() == 3
                   && std::abs(p.z + 3.0) < 1e-6 && p.estSec > 0;
            if (i > 0)
                tiled = tiled && p.qStart == sq->passes.at(i - 1).qEnd;
        }
        check(rows, qPrintable(tag + QStringLiteral("each square: its three passes, to Z -3, a time of its own")));
        check(tiled, qPrintable(tag + QStringLiteral("the squares tile the toolpath")));
        // Halfway through the third square's second pass the header says so,
        // and the third square's row is the running one.
        const c2d::JobLevel &l = sq->passes.at(2).levels.at(1);
        c2d::JobView view;
        view.setPlan(plan, QStringLiteral("job"));
        view.setProgress((l.qStart + l.qEnd) / 2, plan.total);
        bool said = false;
        for (const QLabel *lab : view.findChildren<QLabel *>())
            said = said || lab->text().contains(QStringLiteral("Shape 3 of 5  ·  pass 2 of 3"));
        check(said, qPrintable(tag + QStringLiteral("the header names the square and its pass")));
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
    QSettings().setValue(QStringLiteral("gamepad/10"), QStringLiteral("spindle+"));
    QSettings().setValue(QStringLiteral("gamepad/11"), QStringLiteral("spindle-"));
    QSettings().setValue(QStringLiteral("gamepad/12"), QStringLiteral("rapid"));
    QSettings().setValue(QStringLiteral("ui/padStyle"), QStringLiteral("xbox"));

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
        for (QToolButton *b : panel.findChildren<QToolButton *>())
            if (b->text() == QLatin1String("10"))
                b->click();               // the step starts at 1 mm
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

        // An Xbox 360 pad has its D-pad on axes 6 and 7. A tap there is one
        // jog step in X, and the left stick twitching at rest meanwhile does
        // not cancel it. The triggers (axes 2 and 5) do nothing.
        wait(800);
        grbl.events.clear();
        emit pad->axisChanged(6, 32767);
        emit pad->axisChanged(0, 300);
        wait(100);
        emit pad->axisChanged(6, 0);
        wait(1500);
        check(grbl.count("$J=") == 1 && lastJog().contains('X'),
              "gamepad: the D-pad of an Xbox pad jogs X");
        grbl.events.clear();
        emit pad->axisChanged(5, 32767);
        emit pad->axisChanged(2, 32767);
        wait(600);
        emit pad->axisChanged(5, -32767);
        emit pad->axisChanged(2, -32767);
        wait(600);
        check(grbl.count("$J=") == 0, "gamepad: the triggers of an Xbox pad do nothing");
    }

    // Spindle and rapid overrides beside the feed ones, and on the gamepad:
    // each sends its GRBL realtime byte, nothing queued. The pad's rapid
    // button steps 100 -> 50 -> 25 -> 100 from what the controller reports.
    {
        auto ovs = [&] {
            QList<QByteArray> o;
            for (const QByteArray &e : grbl.events)
                if (e.startsWith("OV:"))
                    o << e;
            return o;
        };
        grbl.events.clear();
        button(QStringLiteral("+10%"), QStringLiteral("Spindle 10 % faster"))->click();
        button(QStringLiteral("−10%"), QStringLiteral("Spindle 10 % slower"))->click();
        button(QStringLiteral("100%"), QStringLiteral("Spindle back"))->click();
        button(QStringLiteral("25%"), QStringLiteral("Rapid moves at 25"))->click();
        button(QStringLiteral("50%"), QStringLiteral("Rapid moves at 50"))->click();
        button(QStringLiteral("100%"), QStringLiteral("Rapid moves back"))->click();
        button(QStringLiteral("+10%"), QStringLiteral("Feed 10 % faster"))->click();
        wait(300);
        check(ovs() == QList<QByteArray>({"OV:9a", "OV:9b", "OV:99", "OV:97", "OV:96", "OV:95", "OV:91"}),
              "overrides: spindle, rapid and feed buttons send their realtime bytes");
        check(grbl.count("$") == 0 && grbl.count("G") == 0, "overrides: nothing queued");
        grbl.ovRapid = 100;
        grbl.ovSpindle = 100;
        wait(400);
        grbl.events.clear();
        padPress(10);
        padPress(11);
        wait(300);
        check(ovs() == QList<QByteArray>({"OV:9a", "OV:9b"}),
              "gamepad: spindle + and - send 0x9A and 0x9B");
        grbl.events.clear();
        for (int i = 0; i < 3; ++i) {
            padPress(12);
            wait(500);                       // the next report carries the new rapid
        }
        check(ovs() == QList<QByteArray>({"OV:96", "OV:97", "OV:95"}),
              "gamepad: rapid cycles 50, 25, back to 100");
        QLabel *ov = panel.findChild<QLabel *>(QStringLiteral("ovLabel"));
        wait(400);
        check(ov && ov->text() == QLatin1String("feed 100 %"), "overrides: all back to 100 % on the readout");
    }

    // Spindle by hand in the "More" fold: On sends M3 at the field's speed
    // (kept under the top speed), after asking when Z is not up; Off sends
    // M5. Warm-up steps through half, three quarters and full speed, then M5
    // (step lengths from machine/warmupSeconds, here 1 s each).
    {
        spin(1500, [&] { return !panel.isBusy() && s->pendingCommands() == 0
                                && panel.statusJson().value("state").toString() == "Idle"; });
        wait(500);
        QSpinBox *rpm = nullptr;
        for (QSpinBox *b : panel.findChildren<QSpinBox *>())
            if (b->suffix() == QLatin1String(" rpm"))
                rpm = b;
        check(rpm && rpm->value() == 10000, "spindle: the speed field starts at the top speed");
        rpm->setValue(50000);
        check(rpm->value() == 10000, "spindle: the speed field stays under the top speed");
        rpm->setValue(8000);
        answer = [](const Seen &v) {
            return v.headline.contains(QStringLiteral("clear of the work")) ? QStringLiteral("Spin up")
                                                                             : QString();
        };
        seen.clear();
        grbl.events.clear();
        QPushButton *on = button(QStringLiteral("On"), QStringLiteral("M3"));
        check(on->isEnabled(), "spindle: On is ready on an idle, homed machine");
        on->click();
        spin(1500, [&] { return grbl.count("M3") > 0; });
        check(grbl.events.contains("M3 S8000"), "spindle: On sends M3 at the field's speed");
        check(seen.size() == 1, "spindle: with Z down it asks whether the bit is clear");
        button(QStringLiteral("Off"), QStringLiteral("M5"))->click();
        spin(1500, [&] { return grbl.count("M5") > 0; });
        check(grbl.count("M5") == 1, "spindle: Off sends M5");

        const double zWas = grbl.mz;
        grbl.mz = -1.5;                       // up at the top: no question
        wait(600);
        QSettings().setValue(QStringLiteral("machine/warmupSeconds"),
                             QStringList{QStringLiteral("1"), QStringLiteral("1"), QStringLiteral("1")});
        seen.clear();
        grbl.events.clear();
        QPushButton *warm = button(QStringLiteral("Warm-up"));
        warm->click();
        spin(500, [&] { return grbl.count("M3") > 0; });
        check(warm->text().startsWith(QStringLiteral("Stop")), "warm-up: the button counts down and stops it");
        spin(8000, [&] { return grbl.count("M5") > 0; });
        QList<QByteArray> sent;
        for (const QByteArray &e : grbl.events)
            if (e.startsWith("M3") || e.startsWith("M5"))
                sent << e;
        check(sent == QList<QByteArray>({"M3 S5000", "M3 S7500", "M3 S10000", "M5"}),
              "warm-up: half, three quarters, full speed, then off");
        check(seen.isEmpty(), "warm-up: Z at the top, no question");
        wait(300);
        check(warm->text() == QLatin1String("Warm-up"), "warm-up: the button is back when done");

        // Stopped early with the same button: M5 at once.
        grbl.events.clear();
        QSettings().setValue(QStringLiteral("machine/warmupSeconds"),
                             QStringList{QStringLiteral("60"), QStringLiteral("60"), QStringLiteral("60")});
        warm->click();
        spin(500, [&] { return grbl.count("M3") > 0; });
        warm->click();
        spin(1000, [&] { return grbl.count("M5") > 0; });
        check(grbl.count("M3") == 1 && grbl.count("M5") == 1 && warm->text() == QLatin1String("Warm-up"),
              "warm-up: pressed again, the spindle stops");
        QSettings().remove(QStringLiteral("machine/warmupSeconds"));
        grbl.mz = zWas;
        answer = [](const Seen &) { return QString(); };
        wait(500);
    }

    // Dust/Coolant by hand: M8 on, M9 off, and the button follows the A:
    // field of the controller's report (here switched behind its back too).
    {
        QPushButton *cool = panel.findChild<QPushButton *>(QStringLiteral("coolant"));
        check(cool && !cool->isChecked(), "coolant: the button is there, off");
        grbl.events.clear();
        cool->click();
        wait(600);
        check(grbl.events.contains("M8") && cool->isChecked() && grbl.flood,
              "coolant: on sends M8 and the report keeps it on");
        cool->click();
        wait(600);
        check(grbl.events.contains("M9") && !cool->isChecked() && !grbl.flood,
              "coolant: off sends M9");
        grbl.flood = true;
        wait(600);
        check(cool->isChecked() && panel.statusJson().value("coolant").toBool(),
              "coolant: the button and the pad status follow A:F");
        grbl.flood = false;
        wait(600);
        check(!cool->isChecked(), "coolant: no A: with Ov: means off");
    }

    // A pad's Zero X/Y or Zero Z bumped while a question of this page is
    // open (here Run's): what the question says was worked out from the zero
    // as it was, so the press is ignored and logged. With nothing open the
    // same button zeroes.
    {
        spin(1500, [&] { return !panel.isBusy() && s->pendingCommands() == 0
                                && panel.statusJson().value("state").toString() == "Idle"; });
        wait(800);
        auto logText = [&] {
            QString text;
            for (QPlainTextEdit *t : panel.findChildren<QPlainTextEdit *>())
                text += t->toPlainText();
            return text;
        };
        const int ignoredBefore = int(logText().count(QLatin1String("ignored while a question is open")));
        grbl.events.clear();
        seen.clear();
        bool asked = false;
        answer = [&](const Seen &v) {
            if (v.caption == QLatin1String("RUN PROGRAM")) {
                asked = true;
                padPress(0);                 // zeroxy
                padPress(3);                 // zeroz
                return QStringLiteral("Cancel");
            }
            return QStringLiteral("OK");
        };
        runBtn->click();
        spin(5000, [&] { return asked && !QApplication::activeModalWidget(); });
        wait(400);
        check(asked && !s->isStreaming(), "pad zero under a question: Run asked, and was cancelled");
        check(grbl.count("G10") == 0, "pad zero under a question: nothing zeroed");
        check(logText().count(QLatin1String("ignored while a question is open")) == ignoredBefore + 2,
              "pad zero under a question: both presses logged as ignored");
        answer = [&](const Seen &) { return QStringLiteral("OK"); };
        padPress(0);
        spin(1500, [&] { return grbl.count("G10") > 0; });
        wait(300);
        check(grbl.count("G10 L20 P1 X0 Y0") == 1, "pad zero with no question open: zeroes X/Y");

        // Run's question answered Start after the open design was reloaded
        // under it (same file, same object: the load count could not tell):
        // refused, nothing streamed.
        spin(1500, [&] { return !panel.isBusy() && s->pendingCommands() == 0; });
        wait(400);
        seen.clear();
        grbl.events.clear();
        answer = [&](const Seen &v) {
            if (v.caption == QLatin1String("RUN PROGRAM")) {
                panel.setDocument(&doc);
                return QStringLiteral("▶ Start");
            }
            return QStringLiteral("OK");
        };
        runBtn->click();
        spin(3000, [&] { return seen.size() >= 2 && !QApplication::activeModalWidget(); });
        wait(400);
        bool refused = false;
        for (const Seen &v : seen)
            refused = refused || (v.headline + v.body).contains(QLatin1String("Another design was opened meanwhile"));
        check(refused && !s->isStreaming() && grbl.count("G0") == 0 && grbl.count("G1") == 0,
              "run: a design reloaded under its question is not streamed");
        answer = [&](const Seen &) { return QStringLiteral("OK"); };
    }

    // Hover 5 cm: the outline it asks about is the design's at the time it
    // asked. A design swapped in while the question is open (a pad sent a new
    // version) is not traced with the old outline; the user is told instead.
    {
        spin(1500, [&] { return !panel.isBusy() && s->pendingCommands() == 0
                                && panel.statusJson().value("state").toString() == "Idle"; });
        wait(800);
        QPushButton *hover = button(QStringLiteral("Hover 5 cm"));
        check(hover != nullptr, "hover: button found");
        const double wcoWas = grbl.wcoZ;
        grbl.wcoZ = -70;                     // work Z0 low enough for 5 cm above it to be in the travel
        wait(600);
        grbl.events.clear();
        seen.clear();
        answer = [&](const Seen &v) {
            if (v.headline.startsWith(QLatin1String("Trace the job outline"))) {
                panel.setDocument(&doc);     // the same object, reloaded under the question
                return QStringLiteral("▶ Trace");
            }
            return QStringLiteral("OK");
        };
        hover->click();
        spin(3000, [&] { return seen.size() >= 2 && !QApplication::activeModalWidget(); });
        wait(400);
        check(seen.size() == 2 && seen.at(1).headline == QLatin1String("The design changed meanwhile."),
              "hover: a design swapped in under the question is reported");
        check(grbl.count("G1") == 0 && !s->isMacroRunning(), "hover: and nothing is traced");
        // The same without a swap traces.
        seen.clear();
        answer = [&](const Seen &v) {
            return v.headline.startsWith(QLatin1String("Trace the job outline")) ? QStringLiteral("▶ Trace")
                                                                                  : QStringLiteral("OK");
        };
        bool traced = false;
        auto c = QObject::connect(s, &c2d::GrblStreamer::macroFinished, [&](bool) { traced = true; });
        hover->click();
        spin(5000, [&] { return traced; });
        QObject::disconnect(c);
        check(seen.size() == 1 && grbl.count("G1 F") == 1, "hover: with the design as asked, it traces");
        {
            const int m5 = grbl.events.indexOf("M5");
            int firstMove = -1;
            for (int i = 0; i < grbl.events.size() && firstMove < 0; ++i)
                if (grbl.events.at(i).startsWith("G0") || grbl.events.at(i).startsWith("G53"))
                    firstMove = i;
            check(m5 >= 0 && m5 < firstMove, "hover: M5 goes out before the first move (a hand-started spindle)");
        }
        answer = [&](const Seen &) { return QStringLiteral("OK"); };
        grbl.wcoZ = wcoWas;
        wait(800);
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

    // A G-code file runs on the same path as a design: tidied, its T/M6
    // turned into the tool-change pause, a job row per tool, and back to
    // the design afterwards. Unsupported words are refused when loading.
    {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        auto writeFile = [&](const QString &fn, const QByteArray &text) {
            const QString path = dir + QLatin1Char('/') + fn;
            QFile f(path);
            f.open(QIODevice::WriteOnly | QIODevice::Truncate);
            f.write(text);
            return path;
        };
        QString why;
        const QString inch = writeFile(QStringLiteral("inch.nc"), "G20 G90\nG0 X1 Y1\n");
        check(!panel.loadProgramFile(inch, &why) && why.contains(QLatin1String("inches"))
                  && why.contains(QLatin1String("Line 1")) && panel.programFile().isEmpty(),
              "nc file: G20 refused, with the line and why");
        const QString comp = writeFile(QStringLiteral("comp.nc"), "G21\nG41 D1\n");
        check(!panel.loadProgramFile(comp, &why) && why.contains(QLatin1String("compensation")),
              "nc file: cutter compensation refused");
        const QString path = writeFile(QStringLiteral("tile1.nc"),
            "%\n(made elsewhere)\nN10 G21 G90 G17\nT1 M6\nS12000 M3\nG0 X10 Y10 Z5\n"
            "G1 Z-1 F300 ; plunge\nG2 X20 Y10 I5 J0 F800\nG0 Z5\nT2 M6\nM3 S10000\n"
            "G0X30Y30\nG1Z-0.5F200\nG0Z5\nM5\nM30\n%\n");
        check(panel.loadProgramFile(path, &why), qPrintable(QStringLiteral("nc file: loads (%1)").arg(why)));
        bool named = false;
        for (QLabel *l : panel.findChildren<QLabel *>())
            named = named || l->text() == QLatin1String("File: tile1.nc");
        check(named, "nc file: the job card names the file");
        check(panel.statusJson().value("job").toString() == QLatin1String("tile1.nc"),
              "nc file: the pad's status names the file");

        seen.clear();
        int prompts = 0;
        answer = [&](const Seen &v) {
            if (v.caption == QLatin1String("RUN PROGRAM"))
                return QStringLiteral("▶ Start");
            if (v.caption == QLatin1String("TOOL CHANGE")) {
                ++prompts;
                return QStringLiteral("Same length: continue");
            }
            return QStringLiteral("OK");
        };
        bool finished = false, finishedOk = false;
        QObject::connect(s, &c2d::GrblStreamer::streamFinished, [&](bool ok) {
            finished = true;
            finishedOk = ok;
        });
        grbl.events.clear();
        runBtn->click();
        check(s->isStreaming(), "nc file: started");
        bool asked = false;
        for (const Seen &v : seen)
            asked = asked || (v.caption == QLatin1String("RUN PROGRAM") && v.headline.contains(QLatin1String("tile1.nc")));
        check(asked, "nc file: Run asks about the file");
        const QJsonArray steps = panel.statusJson().value("steps").toArray();
        int changes = 0, cuts = 0;
        for (const QJsonValue &v : steps) {
            const QJsonObject o = v.toObject();
            changes += o.value("kind").toString() == QLatin1String("change");
            cuts += o.value("kind").toString() == QLatin1String("toolpath")
                    && o.value("name").toString().startsWith(QLatin1String("Tool "));
        }
        check(changes == 2 && cuts == 2, qPrintable(QStringLiteral("nc file: a row per tool and per change in the job list (%1)")
                                           .arg(QString::fromUtf8(QJsonDocument(steps).toJson(QJsonDocument::Compact)))));
        spin(20000, [&] { return finished; });
        QObject::disconnect(s, &c2d::GrblStreamer::streamFinished, nullptr, nullptr);
        check(finished && finishedOk, "nc file: runs to the end");
        check(prompts == 2, "nc file: T/M6 parks for each tool change");
        check(grbl.count("G1 Z-1 F300") == 1 && grbl.count("G2 X20 Y10 I5 J0 F800") == 1
                  && grbl.count("G1 Z-0.5 F200") == 1 && grbl.count("M03 S12000") == 1 && grbl.count("M03 S10000") == 1,
              "nc file: its moves are streamed, comments and spaces tidied");
        check(grbl.count("N10") == 0 && grbl.count("G21 G90 G17") == 1 && grbl.count("%") == 0
                  && grbl.count("M6") == 0 && grbl.count("T1") == 0 && grbl.count("M30") == 0,
              "nc file: line numbers, %, T/M6 and M30 are not sent");
        check(grbl.count("G0 X10 Y10 Z5") == 0, "nc file: rapids go across and down apart");
        spin(3000, [&] { return !panel.isBusy(); });
        check(panel.programFile() == path, "nc file: stays loaded for the next run");
        panel.backToDesign();
        check(panel.programFile().isEmpty() && panel.statusJson().value("job").toString().isEmpty(),
              "nc file: back to the design");
        answer = [&](const Seen &) { return QStringLiteral("OK"); };
        wait(500);
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

    // Resume after a BitSetter measurement: the spindle stands over the
    // button, and the program's first line is a retract to ITS safe height
    // in work Z - below the button when the stock top is. The resume must
    // rise to the top and go back over where the program parked (machine
    // X/Y of the last report) BEFORE any program line goes out. 2026-10-24:
    // it did not, and the rapid drove the bit 16.7 mm into the BitSetter.
    {
        wait(800);
        QToolButton *bs = nullptr;
        for (QToolButton *b : panel.findChildren<QToolButton *>())
            if (b->text() == QLatin1String("BitSetter on"))
                bs = b;
        bs->setChecked(true);
        grbl.mx = -123.5;
        grbl.my = -234.25;
        wait(800);                           // the panel sees the new position
        // A fresh reference with the BitSetter on (Zero Z measures it).
        grbl.events.clear();
        button(QStringLiteral("Zero"), QStringLiteral("work Z0"))->click();
        spin(4000, [&] { return grbl.count("G38.2") >= 2 && !s->isMacroRunning(); });
        wait(600);
        seen.clear();
        answer = [&](const Seen &v) {
            if (v.caption == QLatin1String("TOOL CHANGE"))
                return QStringLiteral("Measure and continue");
            return QStringLiteral("OK");
        };
        bool finished = false, finishedOk = false;
        auto c = QObject::connect(s, &c2d::GrblStreamer::streamFinished, [&](bool ok) {
            finished = true;
            finishedOk = ok;
        });
        grbl.events.clear();
        s->startStream({QStringLiteral("G90"), QStringLiteral("M0 ;T2"), QStringLiteral("G90G21"),
                        QStringLiteral("G0Z5.000"), QStringLiteral("X25.100Y24.900")});
        spin(8000, [&] { return finished; });
        check(finished && finishedOk, "resume: the job runs to the end");
        const int measured = grbl.indexOf("G43.1");
        const int back = grbl.indexOf("G53 G0 X-123.500 Y-234.250");
        const int program = grbl.indexOf("G90G21");
        const int retract = grbl.indexOf("G0Z5.000");
        check(measured >= 0 && back > measured,
              "resume: after the measurement it goes back over where the program parked");
        check(back >= 0 && program > back && retract > back,
              "resume: the return move goes out BEFORE the program's own retract");
        int rise = -1;
        for (int i = grbl.events.size() - 1; i >= 0; --i)
            if (i < back && grbl.events.at(i).startsWith("G53 G0 Z")) {
                rise = i;
                break;
            }
        check(rise >= 0 && rise > measured, "resume: and rises to the top first");
        QObject::disconnect(c);
        bs->setChecked(false);
        spin(1500, [&] { return !panel.isBusy(); });
        grbl.mx = -100;
        grbl.my = -100;
        wait(800);
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

    // A probe that found nothing says ALARM:5 and then the ok of the probe's
    // own line. A tool length offset waiting for the macro to end (Clear
    // reference pressed while it ran) must not be written in between: GRBL
    // refuses G-code in Alarm, that ok passed for the G43.1's, and a zero
    // offset was then never sent again. It goes out after the unlock.
    {
        spin(1500, [&] { return !panel.isBusy() && s->pendingCommands() == 0
                                && panel.statusJson().value("state").toString() == "Idle"; });
        const double tloWas = s->toolLengthOffset();
        QList<QPair<QString, bool>> answers;
        int macroEnds = 0;
        bool macroOk = true;
        auto c1 = QObject::connect(s, &c2d::GrblStreamer::commandAnswered,
                                   [&](const QString &l, bool ok) { answers.append({l, ok}); });
        auto c2 = QObject::connect(s, &c2d::GrblStreamer::macroFinished, [&](bool ok) {
            ++macroEnds;
            macroOk = ok;
        });
        s->applyToolLengthOffset(1.5);       // an offset on the controller, so zero is a change
        spin(1500, [&] { return answers.size() == 1; });
        check(answers.size() == 1 && answers.at(0).first == QLatin1String("G43.1") && answers.at(0).second,
              "probe miss: an offset is on the controller first");
        grbl.hangDwell = true;               // the dwell stands in for the probe: in flight, unanswered
        grbl.events.clear();
        s->startMacro({QStringLiteral("G4P0.01"), QStringLiteral("G0X1")});
        spin(1500, [&] { return grbl.count("G4P0.01") == 1; });
        grbl.hangDwell = false;              // (it would hang the G43.1 as well)
        s->applyToolLengthOffset(0);
        check(s->isMacroRunning() && grbl.count("G43.1") == 0, "probe miss: the offset waits for the macro");
        grbl.alarm = true;                   // Alarm until $X: G-code is answered error:9
        grbl.send("ALARM:5\r\nok\r\n");
        spin(1500, [&] { return macroEnds > 0; });
        check(macroEnds == 1 && !macroOk && !s->isMacroRunning() && !s->needsReset(),
              "probe miss: the macro ends once, failed, and no reset is needed");
        // The panel unlocks by itself after a probe miss.
        spin(3000, [&] { return answers.size() >= 3 && s->pendingCommands() == 0; });
        wait(300);
        const int unlock = grbl.indexOf("$X"), tlo = grbl.indexOf("G43.1");
        check(unlock >= 0 && tlo > unlock && grbl.count("G43.1") == 1
                  && grbl.events.at(tlo) == "G43.1 Z0.000",
              "probe miss: the offset is written once, after the unlock, not into the alarm");
        check(grbl.count("G0X1") == 0, "probe miss: the rest of the macro is not sent");
        check(answers.size() == 3 && answers.at(1).first == QLatin1String("$X") && answers.at(1).second
                  && answers.at(2).first == QLatin1String("G43.1") && answers.at(2).second,
              "probe miss: the probe's own ok is not taken for the offset's");

        // The same macro line in flight and an end stop (ALARM:1): the macro
        // ends at once, and nothing is written into the locked controller.
        spin(1500, [&] { return panel.statusJson().value("state").toString() == "Idle"; });
        macroEnds = 0;
        macroOk = true;
        grbl.hangDwell = true;
        s->startMacro({QStringLiteral("G4P0.01"), QStringLiteral("G0X1")});
        spin(1500, [&] { return grbl.count("G4P0.01") == 2; });
        grbl.hangDwell = false;
        s->applyToolLengthOffset(1.5);       // waits for the macro again
        grbl.locked = true;
        grbl.events.clear();
        grbl.send("ALARM:1\r\n[MSG:Reset to continue]\r\n");
        spin(1500, [&] { return macroEnds > 0; });
        wait(300);
        QObject::disconnect(c1);
        QObject::disconnect(c2);
        check(macroEnds == 1 && !macroOk && !s->isMacroRunning() && s->needsReset(),
              "limit in a macro: the macro ends once, failed, and a reset is needed");
        bool wrote = false;
        for (const QByteArray &e : grbl.events)
            wrote = wrote || (!e.isEmpty() && e != "^X" && e != "!" && e != "~" && e != "JOGCANCEL");
        check(!wrote, "limit in a macro: neither the next line nor the offset is written");
        spin(1500, [&] { return panel.statusJson().value("state").toString() == "Alarm"; });
        initialize();
        check(!s->needsReset() && panel.statusJson().value("homed").toBool(), "limit in a macro: recovered");
        answers.clear();
        auto c3 = QObject::connect(s, &c2d::GrblStreamer::commandAnswered,
                                   [&](const QString &l, bool ok) { answers.append({l, ok}); });
        s->applyToolLengthOffset(tloWas);    // as it was for the tests after this
        spin(1500, [&] { return !answers.isEmpty() && s->pendingCommands() == 0; });
        QObject::disconnect(c3);
        wait(300);
    }

    // The same with lines sent on their own: $X, and straight behind it a
    // line that ends in an alarm and then its own ok (a $H that fails says
    // ALARM:9, then ok). The ok of $X asks for the offset again; it must not
    // go out on the back of that alarm, where the trailing ok passed for its
    // answer. It goes out with the next unlock.
    {
        spin(1500, [&] { return !panel.isBusy() && s->pendingCommands() == 0
                                && panel.statusJson().value("state").toString() == "Idle"; });
        const double tloWas = s->toolLengthOffset();
        QList<QPair<QString, bool>> answers;
        auto c = QObject::connect(s, &c2d::GrblStreamer::commandAnswered,
                                  [&](const QString &l, bool ok) { answers.append({l, ok}); });
        s->applyToolLengthOffset(2.5);
        spin(1500, [&] { return answers.size() == 1 && s->pendingCommands() == 0; });
        check(answers.size() == 1 && answers.at(0).first == QLatin1String("G43.1") && answers.at(0).second,
              "alarm after unlock: an offset is on the controller first");
        answers.clear();
        grbl.events.clear();
        grbl.hangDwell = true;               // the dwell stands in for the $H: in flight, unanswered
        check(s->sendCommand(QStringLiteral("$X")) && s->sendCommand(QStringLiteral("G4P0.01")),
              "alarm after unlock: both lines sent");
        spin(1500, [&] { return answers.size() == 1; });
        grbl.hangDwell = false;
        check(answers.size() == 1 && answers.at(0).first == QLatin1String("$X") && s->pendingCommands() == 1
                  && grbl.count("G43.1") == 0,
              "alarm after unlock: the offset waits for the line still out");
        grbl.alarm = true;                   // Alarm until $X: G-code is answered error:9
        grbl.send("ALARM:9\r\nok\r\n");
        spin(1500, [&] { return answers.size() >= 2 && s->pendingCommands() == 0; });
        wait(400);
        check(answers.size() == 2 && answers.at(1).first == QLatin1String("G4P0.01") && !answers.at(1).second,
              "alarm after unlock: the alarm is that line's answer");
        check(grbl.count("G43.1") == 0, "alarm after unlock: no offset written into the alarm");
        grbl.events.clear();
        check(s->sendCommand(QStringLiteral("$X")), "alarm after unlock: unlocked again");
        spin(1500, [&] { return answers.size() >= 4 && s->pendingCommands() == 0; });
        wait(300);
        const int unlock = grbl.indexOf("$X"), tlo = grbl.indexOf("G43.1");
        check(unlock >= 0 && tlo > unlock && grbl.count("G43.1") == 1 && grbl.events.at(tlo) == "G43.1 Z2.500",
              "alarm after unlock: the offset is written once, after the unlock");
        check(answers.size() == 4 && answers.at(3).first == QLatin1String("G43.1") && answers.at(3).second,
              "alarm after unlock: and answered for itself");
        answers.clear();
        s->applyToolLengthOffset(tloWas);    // as it was for the tests after this
        spin(1500, [&] { return !answers.isEmpty() && s->pendingCommands() == 0; });
        QObject::disconnect(c);
        wait(300);
    }

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

    // A second direction pressed while the first is still held (a pad stick
    // flipped from - to +): the held jog ends the way a release ends it, with
    // the second cancel for a $J= GRBL has not read yet, before anything goes
    // the other way. One cancel alone let that jog run on.
    {
        spin(1500, [&] { return !panel.isBusy() && s->pendingCommands() == 0
                                && panel.statusJson().value("state").toString() == "Idle"; });
        // Homed jogs name a machine position, others a distance.
        auto dirOf = [&](const QByteArray &jog) {
            const double x = jog.mid(jog.indexOf('X') + 1).split(' ').first().toDouble();
            return jog.contains("G91") ? (x > 0 ? 1 : -1) : (x > grbl.mx ? 1 : -1);
        };
        grbl.events.clear();
        key(Qt::Key_Left, true);             // hold X-: nothing moves before the hold delay
        spin(3000, [&] { return grbl.count("$J=") > 0; });
        check(grbl.count("$J=") > 0 && dirOf(lastJog()) < 0, "flip: X- runs while held");
        key(Qt::Key_Right, true);            // X+, with X- never let go
        spin(3000, [&] { return dirOf(lastJog()) > 0; });
        key(Qt::Key_Right, false);
        key(Qt::Key_Left, false);
        int cancels = 0, plus = -1;
        for (int i = 0; i < grbl.events.size() && plus < 0; ++i) {
            if (grbl.events.at(i) == "JOGCANCEL")
                ++cancels;
            else if (grbl.events.at(i).startsWith("$J=") && dirOf(grbl.events.at(i)) > 0)
                plus = i;
        }
        check(plus >= 0, "flip: X+ runs after the flip");
        check(cancels == 2, "flip: the held jog is cancelled twice before the first jog the other way");
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

    // Cancel job, and Disconnect confirmed while the cancel still waits for
    // the hold to settle: one wait and one reset for both, then the port is
    // closed. (Each had its own wait: the second reset the controller again,
    // into whatever the first had gone on to do.)
    {
        check(s->connectPort(QString::fromLatin1(name)), "cancel then disconnect: connects");
        spin(1500, [&] { return panel.statusJson().value("state").toString() == "Idle"; });
        wait(800);   // $$ answered
        grbl.hangDwell = true;
        grbl.holdSettles = false;            // Hold:1 until told otherwise
        s->startStream({QStringLiteral("M3S1000"), QStringLiteral("G4P3.00"), QStringLiteral("G0X1")});
        wait(400);
        grbl.events.clear();
        seen.clear();
        answer = [&](const Seen &v) {
            return v.headline.contains(QLatin1String("Disconnect")) ? QStringLiteral("Stop and disconnect")
                                                                    : QStringLiteral("Cancel job");
        };
        button(QStringLiteral("✕ Cancel job"))->click();
        button(QStringLiteral("Disconnect"))->click();
        wait(600);
        check(seen.size() == 2 && s->isConnected() && grbl.count("^X") == 0,
              "cancel then disconnect: both asked, and both wait for the hold to settle");
        grbl.holdSettles = true;
        spin(4000, [&] { return !s->isConnected(); });
        wait(1200);  // past the rise the cancel would start, were the port still open
        grbl.hangDwell = false;
        check(!s->isConnected(), "cancel then disconnect: the port is closed in the end");
        check(grbl.count("^X") == 1, "cancel then disconnect: one reset, not one each");
        check(grbl.count("G53 G0 Z") == 0, "cancel then disconnect: nothing rises on the way out");
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
