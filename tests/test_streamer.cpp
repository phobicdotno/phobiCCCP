// GrblStreamer against a scripted controller on a pseudo-terminal: what is
// written to the machine when a program line is rejected mid-job.
//
// Skips (exit 77) where no pty can be opened as a serial port.
//
// Plain asserts, no framework; exits 0 on success.

#include "../src/grblstreamer.h"
#include "../src/surfacemap.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QSocketNotifier>
#include <QTimer>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <fcntl.h>
#include <pty.h>
#include <termios.h>
#include <unistd.h>

static int g_checks = 0;

static void check(bool cond, const char *what)
{
    ++g_checks;
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

// The far end of the pty: answers `?` with Run until it has seen a feed
// hold (then Hold:0), acks every line, rejects the one containing BAD, and
// notes when the soft reset arrives.
struct FakeGrbl {
    int fd = -1;
    QByteArray line;
    bool held = false;
    bool reset = false;
    bool resetWhileHeld = false;
    int linesAfterReset = 0;
    QList<QByteArray> seen;   // every line received, in order
    // Lines containing HANG are never acked: a G4 still running, or a line
    // GRBL's reset flushes before answering it.
    // For the surface-map prober: report Idle at this machine position (work
    // offset 10, 20, 0), follow absolute G0 X/Y moves, answer each G38.2 with
    // a touch at Z -7 where the head stands.
    bool positional = false;
    double x = 0, y = 0;
    bool relative = false;

    QByteArray pos() const
    {
        return QByteArray::number(x, 'f', 3) + ',' + QByteArray::number(y, 'f', 3) + ",-7.000";
    }
    void follow(const QByteArray &l)
    {
        static const QRegularExpression word(QStringLiteral("([XY])\\s*(-?[0-9.]+)"));
        if (l.contains("G91"))
            relative = true;
        if (l.contains("G90"))
            relative = false;
        if (l.contains("G38.2")) {
            send("[PRB:" + pos() + ":1]\r\n");
            return;
        }
        if (relative || l.contains("G53"))
            return;
        for (auto it = word.globalMatch(QString::fromLatin1(l)); it.hasNext();) {
            const QRegularExpressionMatch m = it.next();
            const bool isX = m.captured(1) == QLatin1String("X");
            (isX ? x : y) = m.captured(2).toDouble() + (isX ? 10 : 20);   // work -> machine
        }
    }

    void send(const QByteArray &s) { (void)!::write(fd, s.constData(), size_t(s.size())); }

    void onRead()
    {
        char buf[512];
        const ssize_t n = ::read(fd, buf, sizeof buf);
        for (ssize_t i = 0; i < n; ++i) {
            const char c = buf[i];
            if (c == '?' && positional) {
                send("<Idle|MPos:" + pos() + "|WCO:10.000,20.000,0.000>\r\n");
            } else if (c == '?') {
                send(held ? "<Hold:0|MPos:0.000,0.000,0.000|FS:0,18000>\r\n"
                          : "<Run|MPos:0.000,0.000,0.000|FS:500,18000|Ov:120,100,90>\r\n");
            } else if (c == '!') {
                held = true;
            } else if (c == 0x18) {
                reset = true;
                resetWhileHeld = held;
                held = false;
                line.clear();
                send("\r\nGrbl 1.1h ['$' for help]\r\n");
            } else if (c == '\n') {
                if (reset)
                    ++linesAfterReset;
                seen << line;
                if (positional)
                    follow(line);
                if (!line.contains("HANG"))
                    send(line.contains("BAD") ? "error:20\r\n" : "ok\r\n");
                line.clear();
            } else if (c != '\r') {
                line += c;
            }
        }
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    int master = -1, slave = -1;
    char name[256] = {};
    if (openpty(&master, &slave, name, nullptr, nullptr) != 0) {
        std::printf("test_streamer: no pty, skipped\n");
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

    c2d::GrblStreamer s;
    if (!s.connectPort(QString::fromLatin1(name))) {
        std::printf("test_streamer: pty will not open as a serial port, skipped\n");
        return 77;
    }

    auto spin = [&](int ms, const std::function<bool()> &until) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms && !until())
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    };

    // A rejected line mid-job: hold, and once the hold has settled, a soft
    // reset - so the lines GRBL still buffers are thrown away and the spindle
    // stops, instead of waiting for a Resume that would run them untracked.
    {
        QStringList errors;
        bool finished = false, finishedOk = true;
        QObject::connect(&s, &c2d::GrblStreamer::errorOccurred,
                         [&](const QString &e) { errors << e; });
        QObject::connect(&s, &c2d::GrblStreamer::streamFinished, [&](bool ok) {
            finished = true;
            finishedOk = ok;
        });
        spin(300, [] { return false; });   // a few status reports: Run
        check(s.status().ovFeed == 120 && s.status().ovRapid == 100 && s.status().ovSpindle == 90,
              "status: the Ov: field is parsed (feed 120, rapid 100, spindle 90)");
        s.startStream({QStringLiteral("G90"), QStringLiteral("M03S18000"),
                       QStringLiteral("G1X1F500"), QStringLiteral("G1X2 BAD"),
                       QStringLiteral("G1X3"), QStringLiteral("G1X4"), QStringLiteral("M05")});
        spin(2000, [&] { return finished; });
        check(finished && !finishedOk, "error: the program is reported stopped");
        check(grbl.held, "error: a feed hold is sent first");
        check(!grbl.reset, "error: no reset while the machine may still be moving");
        check(!s.canSendCommand(), "error: no commands while the reset is pending");
        check(!s.sendCommand(QStringLiteral("$X")), "error: an ad-hoc line is refused meanwhile");

        spin(6000, [&] { return grbl.reset; });
        check(grbl.reset, "error: the controller is soft-reset after the hold");
        check(grbl.resetWhileHeld, "error: the reset comes once GRBL reports Hold:0");
        spin(300, [] { return false; });
        check(s.canSendCommand(), "error: commands are accepted again after the reset");
        bool said = false;
        for (const QString &e : errors)
            said |= e.contains(QLatin1String("reset")) && e.contains(QLatin1String("spindle"));
        check(said, "error: the log says the machine was reset and the spindle stopped");
        check(grbl.linesAfterReset == 0, "error: nothing of the program is sent after the reset");
    }

    // Continue after a tool change with lines in front: they go out as part
    // of the program, ahead of the rest of it, and the program still ends
    // cleanly (their acks are counted as the stream's own).
    {
        // The block above captured locals that are gone now.
        QObject::disconnect(&s, &c2d::GrblStreamer::streamFinished, nullptr, nullptr);
        QObject::disconnect(&s, &c2d::GrblStreamer::errorOccurred, nullptr, nullptr);
        grbl.seen.clear();
        int parked = -1;
        bool finished = false, finishedOk = false;
        QObject::connect(&s, &c2d::GrblStreamer::toolChangeRequested, [&](int t) { parked = t; });
        QObject::connect(&s, &c2d::GrblStreamer::streamFinished, [&](bool ok) {
            finished = true;
            finishedOk = ok;
        });
        // The lines the post writes after a tool change, spindle start included.
        s.startStream({QStringLiteral("G90"), QStringLiteral("G0Z5"), QStringLiteral("M0 ;T2"),
                       QStringLiteral("G90G21"), QStringLiteral("G0Z5"), QStringLiteral("M03S18000"),
                       QStringLiteral("G0X10Y10")});
        spin(2000, [&] { return parked >= 0; });
        check(parked == 2 && s.isParkedForTool(), "tool change: the program parks at the marker");
        s.continueAfterToolChange({QStringLiteral("G90"), QStringLiteral("G53 G0 Z-2.500")});
        spin(3000, [&] { return finished; });
        check(finished && finishedOk, "tool change: the program finishes after the inserted lines");
        const int rise = grbl.seen.indexOf("G53 G0 Z-2.500");
        const int across = grbl.seen.indexOf("G90 G0 X10 Y10");
        const int next = grbl.seen.indexOf("G90G21");
        check(rise >= 2 && across == rise + 1 && next == across + 1,
              "tool change: the rise, then across to the first cut at the top, before the program's own lines");
        check(grbl.seen.size() == 9 && grbl.seen.last() == "G0X10Y10",
              "tool change: then the rest of the program, all of it");
        QObject::disconnect(&s, &c2d::GrblStreamer::toolChangeRequested, nullptr, nullptr);
        QObject::disconnect(&s, &c2d::GrblStreamer::streamFinished, nullptr, nullptr);
    }

    // The look-ahead that keeps a Z-only plunge off the BitSetter.
    {
        using c2d::GrblStreamer;
        check(GrblStreamer::crossToFirstCut({"G90", "G0Z5", "G0X10Y-3.5", "G1Z-1F300"}, 0) == "G90 G0 X10 Y-3.5",
              "cross: first X/Y after a Z-only retract");
        check(GrblStreamer::crossToFirstCut({"G0Z5", "G0X10", "G1Y4"}, 0) == "G90 G0 X10 Y4",
              "cross: X and Y from separate lines");
        check(GrblStreamer::crossToFirstCut({"G0Z5", "M0 ;T2", "G0X10Y10"}, 0).isEmpty(),
              "cross: nothing past a tool change");
        check(GrblStreamer::crossToFirstCut({"G0Z5", "G91", "G0X10Y10"}, 0).isEmpty(),
              "cross: nothing past relative mode");
        check(GrblStreamer::crossToFirstCut({"G0Z5", "G1Z-1F300"}, 0).isEmpty(),
              "cross: no X/Y named, no move");
        // What the post writes after a tool change: the spindle starts
        // between the retract and the first X/Y (2026-10-26: the look-ahead
        // stopped at M03 and the retract came down beside the BitSetter).
        check(GrblStreamer::crossToFirstCut({"G90G21", "G0Z5.000", "(Pocket 1)", "M03S18000", "G4P2.00",
                                             "G0X12.5Y-3", "G0Z1"}, 0) == "G90 G0 X12.5 Y-3",
              "cross: past the spindle start and its dwell");
        check(GrblStreamer::crossToFirstCut({"G0Z5", "M3 S1000", "M05", "M5", "G0X1Y2"}, 0) == "G90 G0 X1 Y2",
              "cross: past M3/M5 in any spelling");
        check(GrblStreamer::crossToFirstCut({"G0Z5", "M00", "G0X1Y2"}, 0).isEmpty()
                  && GrblStreamer::crossToFirstCut({"G0Z5", "M1", "G0X1Y2"}, 0).isEmpty()
                  && GrblStreamer::crossToFirstCut({"G0Z5", "M01 ;x", "G0X1Y2"}, 0).isEmpty(),
              "cross: still nothing past a pause (M0, M00, M1, M01)");
        check(GrblStreamer::crossToFirstCut({"G0Z5", "(Hole X9 Y9)", "G0X1Y2"}, 0) == "G90 G0 X1 Y2",
              "cross: a toolpath name in a comment is not a position");
    }

    // Two bits with the same number: the program changes tools twice at
    // `M0 ;T1`, and each pause says which of the program's tool changes it is,
    // so the prompt can name the bit actually wanted.
    {
        QList<QPair<int, int>> parks;
        bool finished = false;
        QObject::connect(&s, &c2d::GrblStreamer::toolChangeRequested,
                         [&](int t, int k) { parks << qMakePair(t, k); });
        QObject::connect(&s, &c2d::GrblStreamer::streamFinished, [&](bool) { finished = true; });
        s.startStream({QStringLiteral("G90"), QStringLiteral("M0 ;T1"), QStringLiteral("G0Z5"),
                       QStringLiteral("M0 ;T1"), QStringLiteral("G0Z6")});
        spin(2000, [&] { return parks.size() == 1; });
        check(parks.size() == 1 && parks.at(0) == qMakePair(1, 0), "ordinal: first change is #0");
        s.continueAfterToolChange();
        spin(2000, [&] { return parks.size() == 2; });
        check(parks.size() == 2 && parks.at(1) == qMakePair(1, 1),
              "ordinal: the second T1 is change #1, not the first one again");
        s.continueAfterToolChange();
        spin(2000, [&] { return finished; });
        check(finished, "ordinal: the program finishes");
        s.startStream({QStringLiteral("M0 ;T3")});
        spin(2000, [&] { return parks.size() == 3; });
        check(parks.size() == 3 && parks.at(2) == qMakePair(3, 0), "ordinal: counts again per program");
        s.stopStream();
        spin(300, [] { return false; });
        QObject::disconnect(&s, &c2d::GrblStreamer::toolChangeRequested, nullptr, nullptr);
        QObject::disconnect(&s, &c2d::GrblStreamer::streamFinished, nullptr, nullptr);
    }

    // A macro cancelled with a line in flight skips that line's ok. When a
    // reset flushes the line instead, no ok ever comes: the skip must not
    // outlive the reset and eat the first ok of the next macro (which then
    // hangs) or program (which then never finishes).
    {
        QList<bool> done;
        QObject::connect(&s, &c2d::GrblStreamer::macroFinished, [&](bool ok) { done << ok; });
        s.startMacro({QStringLiteral("G38.3 Z-10 HANG")});
        s.cancelMacro();
        check(done.size() == 1 && !done.at(0), "skip: the cancelled macro reports failure");
        s.stopStream();                          // reset before its ok
        spin(400, [] { return false; });
        s.startMacro({QStringLiteral("G0 X1"), QStringLiteral("G0 X2")});
        spin(2000, [&] { return done.size() == 2; });
        check(done.size() == 2 && done.at(1), "skip: the next macro runs to the end after a reset");

        s.startMacro({QStringLiteral("G38.3 Z-10 HANG")});
        s.cancelMacro();
        s.disconnectPort();                      // or the cable goes
        check(s.connectPort(QString::fromLatin1(name)), "skip: reconnects");
        spin(400, [] { return false; });
        s.startMacro({QStringLiteral("G0 X3")});
        spin(2000, [&] { return done.size() == 4; });
        check(done.size() == 4 && done.at(3), "skip: the next macro runs after a reconnect");
        QObject::disconnect(&s, &c2d::GrblStreamer::macroFinished, nullptr, nullptr);
    }

    // Ad-hoc lines report their answer (a zero counts only on its ok), and
    // the $$ settings belong to one connection: a reconnect may be another
    // machine, so the old travel must not be used for it.
    {
        QList<QPair<QString, bool>> answers;
        auto c = QObject::connect(&s, &c2d::GrblStreamer::commandAnswered,
                                  [&](const QString &l, bool ok) { answers.append({l, ok}); });
        s.sendCommand(QStringLiteral("G10 L20 P1 Z0"));
        spin(1000, [&] { return answers.size() == 1; });
        s.sendCommand(QStringLiteral("G10 L20 P1 Z0 BAD"));
        spin(1000, [&] { return answers.size() == 2; });
        QObject::disconnect(c);
        check(answers.size() == 2 && answers.at(0).first == QLatin1String("G10 L20 P1 Z0")
                  && answers.at(0).second && !answers.at(1).second,
              "answers: ok and error reported per command");
        grbl.send("$131=425.000\r\n");
        spin(500, [&] { return s.setting(131, 0) == 425.0; });
        check(s.setting(131, 0) == 425.0, "settings: $131 kept");
        s.disconnectPort();
        check(s.connectPort(QString::fromLatin1(name)), "settings: reconnects");
        check(s.setting(131, 1237.0) == 1237.0, "settings: forgotten on reconnect");
        spin(400, [] { return false; });
    }

    // Surface map: a head jogged sideways while waiting for the plate is
    // taken back over the point before it touches, and the touch is filed
    // with where it really was.
    {
        grbl.positional = true;
        grbl.x = 10;
        grbl.y = 20;
        c2d::SurfaceMapProber prober(&s);
        c2d::SurfaceMapPlan plan;
        plan.x0 = 0; plan.y0 = 0; plan.x1 = 30; plan.y1 = 0;
        plan.cols = 2; plan.rows = 1;
        plan.plate = 0;
        plan.pauseEachPoint = true;
        int waiting = 0, measured = 0;
        double mx = -1, my = -1;
        QStringList fails;
        bool done = false;
        QObject::connect(&prober, &c2d::SurfaceMapProber::waitingForPlate, [&](int, int) { ++waiting; });
        QObject::connect(&prober, &c2d::SurfaceMapProber::pointMeasured,
                         [&](int, int, double px, double py, double) { ++measured; mx = px; my = py; });
        QObject::connect(&prober, &c2d::SurfaceMapProber::failed, [&](const QString &w) { fails << w; });
        QObject::connect(&prober, &c2d::SurfaceMapProber::finished, [&](const c2d::SurfaceMap &) { done = true; });
        spin(400, [] { return false; });     // a status report with the work offset
        prober.start(plan);
        spin(2000, [&] { return waiting == 1; });
        check(waiting == 1, "map: waits over the first point");
        grbl.x += 4;                         // the operator jogs X while waiting
        spin(600, [] { return false; });     // and the status catches up
        prober.probeNext();
        spin(2000, [&] { return waiting == 2; });
        check(measured == 0 && fails.size() == 1 && fails.first().contains(QLatin1String("off point")),
              "map: a jogged head does not touch off its point");
        check(waiting == 2 && std::abs(grbl.x - 10) < 1e-6, "map: it goes back over the point and waits again");
        spin(600, [] { return false; });
        prober.probeNext();
        spin(2000, [&] { return measured == 1; });
        check(measured == 1 && std::abs(mx) < 1e-6 && std::abs(my) < 1e-6,
              "map: the touch is reported where it was, in work XY");
        spin(2000, [&] { return waiting == 3; });
        spin(600, [] { return false; });
        prober.probeNext();
        spin(2000, [&] { return done; });
        check(done && measured == 2 && std::abs(mx - 30) < 1e-6, "map: the run finishes");
        grbl.positional = false;
    }

    s.disconnectPort();
    ::close(slave);
    ::close(master);
    std::printf("test_streamer: %d checks OK\n", g_checks);
    return 0;
}
