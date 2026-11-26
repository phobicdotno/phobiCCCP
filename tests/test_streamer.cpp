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
                // GRBL 1.1h: a G2/G3 block without X, Y or Z is error:26.
                static const QRegularExpression arc(QStringLiteral("(?:^|[^A-Z0-9.])G0*[23](?![0-9.])"));
                static const QRegularExpression axis(QStringLiteral("[XYZ]"));
                const QString l = QString::fromLatin1(line).toUpper();
                const bool noAxis = arc.match(l).hasMatch() && !axis.match(l).hasMatch();
                if (!line.contains("HANG"))
                    send(noAxis ? "error:26\r\n" : line.contains("BAD") ? "error:20\r\n" : "ok\r\n");
                line.clear();
            } else if (c != '\r') {
                line += c;
            }
        }
    }
};

// Follows the work X/Y and motion mode through the lines a controller was
// sent and checks every arc as GRBL would run it: from where the machine
// stands, the centre at start + I/J, the end as far from the centre as the
// start. A machine-coordinate or stored-spot move with X/Y leaves the start
// unknown, and an arc from an unknown start fails too.
static bool arcsConsistent(const QList<QByteArray> &lines)
{
    static const QRegularExpression word(QStringLiteral("([GXYIJ])\\s*(-?[0-9.]+)"));
    double x = 0, y = 0;
    bool known = false;
    int motion = 0;
    for (const QByteArray &raw : lines) {
        QString l = QString::fromLatin1(raw).toUpper();
        const int c = l.indexOf(QLatin1Char(';'));
        if (c >= 0)
            l.truncate(c);
        l.remove(QRegularExpression(QStringLiteral("\\([^)]*\\)")));
        bool machine = false, hasX = false, hasY = false, hasI = false, hasJ = false;
        double nx = x, ny = y, i = 0, j = 0;
        for (auto it = word.globalMatch(l); it.hasNext();) {
            const auto m = it.next();
            const double v = m.captured(2).toDouble();
            switch (m.captured(1).at(0).toLatin1()) {
            case 'G':
                if (v == 53 || v == 28 || v == 30) machine = true;
                else if (v >= 0 && v <= 3 && v == std::floor(v)) motion = int(v);
                break;
            case 'X': nx = v; hasX = true; break;
            case 'Y': ny = v; hasY = true; break;
            case 'I': i = v; hasI = true; break;
            case 'J': j = v; hasJ = true; break;
            }
        }
        if (machine) {
            if (hasX || hasY)
                known = false;
            continue;
        }
        if ((motion == 2 || motion == 3) && (hasX || hasY || hasI || hasJ)) {
            if (!known)
                return false;
            const double cx = x + i, cy = y + j;
            if (std::abs(std::hypot(x - cx, y - cy) - std::hypot(nx - cx, ny - cy)) > 0.005)
                return false;
        }
        if (hasX && hasY)
            known = true;
        x = nx;
        y = ny;
    }
    return true;
}

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

    // A program cutting in G1 across a tool change on a line of its own: the
    // rise and the crossing leave GRBL in G0, so the program's G1 and feed go
    // back in before its next line (a bare "X20Y3" must not run as a rapid).
    {
        QObject::disconnect(&s, &c2d::GrblStreamer::streamFinished, nullptr, nullptr);
        grbl.seen.clear();
        int parked = -1;
        bool finished = false;
        QObject::connect(&s, &c2d::GrblStreamer::toolChangeRequested, [&](int t) { parked = t; });
        QObject::connect(&s, &c2d::GrblStreamer::streamFinished, [&](bool) { finished = true; });
        s.startStream({QStringLiteral("G90"), QStringLiteral("G0Z5"), QStringLiteral("G0X0Y0"),
                       QStringLiteral("G1Z-1F300"), QStringLiteral("G1X5F900"), QStringLiteral("M0 ;T2"),
                       QStringLiteral("X20Y3")});
        spin(2000, [&] { return parked >= 0; });
        s.continueAfterToolChange({QStringLiteral("G90"), QStringLiteral("G53 G0 Z-2.500")});
        spin(3000, [&] { return finished; });
        const int across = grbl.seen.indexOf("G90 G0 X20 Y3");
        check(finished && across > 0 && grbl.seen.value(across + 1) == "G1 F900.0"
                  && grbl.seen.value(across + 2) == "X20Y3",
              "tool change: the program's G1 and feed back in before its next line");
        QObject::disconnect(&s, &c2d::GrblStreamer::toolChangeRequested, nullptr, nullptr);
        QObject::disconnect(&s, &c2d::GrblStreamer::streamFinished, nullptr, nullptr);
    }

    // The same after an arc: GRBL refuses a bare "G2" (error:26), so the arc
    // mode goes on the program's next move and the feed on a G1 line.
    {
        grbl.seen.clear();
        int parked = -1, added = -1;
        bool finished = false, finishedOk = false;
        QObject::connect(&s, &c2d::GrblStreamer::toolChangeRequested, [&](int t) { parked = t; });
        QObject::connect(&s, &c2d::GrblStreamer::streamFinished, [&](bool ok) {
            finished = true;
            finishedOk = ok;
        });
        s.startStream({QStringLiteral("G90"), QStringLiteral("G0Z5"), QStringLiteral("G0X10Y0"),
                       QStringLiteral("G1Z-1F300"), QStringLiteral("G2X0Y10I-10J0F800"), QStringLiteral("M0 ;T2"),
                       QStringLiteral("(next)"), QStringLiteral("X-10Y0I0J-10")});
        spin(2000, [&] { return parked >= 0; });
        added = s.continueAfterToolChange({QStringLiteral("G90"), QStringLiteral("G53 G0 Z-2.500")});
        spin(3000, [&] { return finished; });
        // Back to where the arc starts (where the machine stood before the
        // change), not to its end: from there I0 J-10 is the programmed centre.
        const int across = grbl.seen.indexOf("G90 G0 X0.000 Y10.000");
        check(finished && finishedOk && across > 0 && grbl.seen.value(across + 1) == "G1 F800.0"
                  && grbl.seen.value(across + 2) == "G2 X-10Y0I0J-10" && !grbl.seen.contains("G2")
                  && !grbl.seen.contains("G90 G0 X-10 Y0"),
              "tool change: a modal arc goes back on its own block, from its own start");
        check(arcsConsistent(grbl.seen), "tool change: every arc sent runs from its start, end on its circle");
        check(added == 4, "tool change: every line put in is counted (rise, crossing, feed)");
        QObject::disconnect(&s, &c2d::GrblStreamer::toolChangeRequested, nullptr, nullptr);
        QObject::disconnect(&s, &c2d::GrblStreamer::streamFinished, nullptr, nullptr);
    }

    // The same arc, but the program moved in machine coordinates before the
    // change: where the arc starts in work X/Y is not known, so it is refused
    // rather than sent from a guess.
    {
        grbl.seen.clear();
        int parked = -1;
        QString err;
        QObject::connect(&s, &c2d::GrblStreamer::toolChangeRequested, [&](int t) { parked = t; });
        QObject::connect(&s, &c2d::GrblStreamer::errorOccurred, [&](const QString &e) { err = e; });
        s.startStream({QStringLiteral("G90"), QStringLiteral("G0Z5"), QStringLiteral("G0X10Y0"),
                       QStringLiteral("G1Z-1F300"), QStringLiteral("G2X0Y10I-10J0F800"),
                       QStringLiteral("G53G0X-50"), QStringLiteral("M0 ;T2"), QStringLiteral("X-10Y0I0J-10")});
        spin(2000, [&] { return parked >= 0; });
        const int before = grbl.seen.size();
        const int added = s.continueAfterToolChange({QStringLiteral("G90"), QStringLiteral("G53 G0 Z-2.500")});
        spin(500, [] { return false; });
        check(added == 0 && err.contains(QLatin1String("arc")) && grbl.seen.size() == before
                  && !grbl.seen.contains("G2 X-10Y0I0J-10"),
              "tool change: an arc from an unknown start is refused, nothing sent");
        s.stopStream();
        spin(300, [] { return false; });
        QObject::disconnect(&s, &c2d::GrblStreamer::toolChangeRequested, nullptr, nullptr);
        QObject::disconnect(&s, &c2d::GrblStreamer::errorOccurred, nullptr, nullptr);
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
        // In the queue a tool change is a marker by now, not `M0 ;T<n>`, and
        // a hold point is one too: the look-ahead ends there all the same.
        check(GrblStreamer::crossToFirstCut({"G0Z5", "@TOOL:2", "G0X10Y10"}, 0).isEmpty(),
              "cross: nothing past a tool-change marker");
        check(GrblStreamer::crossToFirstCut({"G0X10", "@TOOL:2", "G0Y4"}, 0).isEmpty(),
              "cross: no Y borrowed from behind a tool-change marker");
        check(GrblStreamer::crossToFirstCut({"G0Z5", "@HOLD", "G0X1Y2"}, 0).isEmpty(),
              "cross: nothing past a hold marker");
        check(GrblStreamer::crossToFirstCut({"G0Z5", "G0X1Y2", "@TOOL:2", "G0X8Y9"}, 0) == "G90 G0 X1 Y2",
              "cross: a marker after the first X/Y changes nothing");
        check(GrblStreamer::crossToFirstCut({"G0Z5", "@TOOL:2", "G0X10Y10"}, 2) == "G90 G0 X10 Y10",
              "cross: from behind the marker, that section's own X/Y");
    }

    // The same look-ahead where it really runs, over the queue: a program
    // with two tool changes, and between them a section that names no X/Y
    // (2026-11-12: the resume after the first change read on past the second
    // one's marker and crossed to the first cut of the bit not yet fitted).
    {
        QList<int> parks;
        bool finished = false, finishedOk = false;
        grbl.seen.clear();
        QObject::connect(&s, &c2d::GrblStreamer::toolChangeRequested, [&](int t) { parks << t; });
        QObject::connect(&s, &c2d::GrblStreamer::streamFinished, [&](bool ok) {
            finished = true;
            finishedOk = ok;
        });
        const QStringList rise = {QStringLiteral("G90"), QStringLiteral("G53 G0 Z-2.500")};
        s.startStream({QStringLiteral("G90"), QStringLiteral("M0 ;T1"), QStringLiteral("G0Z5"),
                       QStringLiteral("M05"), QStringLiteral("M0 ;T2"), QStringLiteral("G0Z5"),
                       QStringLiteral("G0X10Y10"), QStringLiteral("G1Z-1F300")});
        spin(2000, [&] { return parks.size() == 1; });
        check(parks.size() == 1 && parks.at(0) == 1, "queue cross: parks for the first bit");
        s.continueAfterToolChange(rise);
        spin(2000, [&] { return parks.size() == 2; });
        check(parks.size() == 2 && parks.at(1) == 2, "queue cross: parks for the second bit");
        check(grbl.seen.contains("G53 G0 Z-2.500") && grbl.seen.last() == "M05",
              "queue cross: the first section ran, behind its rise");
        check(!grbl.seen.contains("G90 G0 X10 Y10"),
              "queue cross: no crossing to the next bit's first cut before that bit is in");
        const int before = int(grbl.seen.size());
        s.continueAfterToolChange(rise);
        spin(3000, [&] { return finished; });
        check(finished && finishedOk, "queue cross: the program finishes");
        check(grbl.seen.size() == before + 6 && grbl.seen.at(before + 1) == "G53 G0 Z-2.500"
                  && grbl.seen.at(before + 2) == "G90 G0 X10 Y10" && grbl.seen.at(before + 3) == "G0Z5"
                  && grbl.seen.count("G90 G0 X10 Y10") == 1,
              "queue cross: after the second change the crossing comes, once, ahead of the way down");
        QObject::disconnect(&s, &c2d::GrblStreamer::toolChangeRequested, nullptr, nullptr);
        QObject::disconnect(&s, &c2d::GrblStreamer::streamFinished, nullptr, nullptr);
    }

    // Hold with a lift: what the program has sent is read back (motion mode,
    // feed, spindle, work Z), and the lift, the wait and the way down go in
    // the stream in order.
    {
        using c2d::GrblStreamer;
        const GrblStreamer::Modal m = GrblStreamer::scanModal(
            {"G90G21", "G0Z2.540", "M03S16000", "G4P3.00", "X73.875Y61.125", "G1Z0.000F600.0",
             "Y68.875Z-1.367", "G53 G0 Z-5", "X66.125Z-2.733"}, 9);
        check(m.valid && m.motion == 0, "modal: the last motion mode (G53 G0 counts)");
        check(m.haveFeed && m.feed == 600.0 && m.spindle == QChar('3') && m.speed == 16000,
              "modal: feed, spindle and speed");
        check(m.haveZ && m.z == -2.733, "modal: work Z, a G53 line leaves it alone");
        check(!GrblStreamer::scanModal({"G0Z5", "G91", "G1Z-1F300"}, 3).valid, "modal: G91 cannot be followed");
        check(GrblStreamer::scanModal({"G0Z5", "M05"}, 2).spindle == QChar('5'), "modal: M05 is spindle off");
        {
            const GrblStreamer::Modal c = GrblStreamer::scanModal({"G1Z-3F400", "G1X5 (back to Z9 F99 G0)"}, 2);
            check(c.z == -3 && c.feed == 400 && c.motion == 1, "modal: words inside a ( ) comment set nothing");
        }
        check(GrblStreamer::scanModal({"G90G21", "G1Z0F600"}, 2).motion == 1,
              "modal: G21 is not a motion mode");

        QObject::disconnect(&s, &c2d::GrblStreamer::streamFinished, nullptr, nullptr);
        QObject::disconnect(&s, &c2d::GrblStreamer::errorOccurred, nullptr, nullptr);
        QObject::disconnect(&s, &c2d::GrblStreamer::toolChangeRequested, nullptr, nullptr);
        QStringList prog = {QStringLiteral("G90G21"), QStringLiteral("G0Z2.540"), QStringLiteral("M03S16000"),
                            QStringLiteral("G1Z0.000F600.0")};
        for (int i = 0; i < 300; ++i)
            prog << QStringLiteral("X%1Y%2Z-1.500").arg(i).arg(i % 7);
        grbl.seen.clear();
        bool reached = false, finished = false;
        QObject::connect(&s, &c2d::GrblStreamer::holdPointReached, [&] { reached = true; });
        QObject::connect(&s, &c2d::GrblStreamer::streamFinished, [&](bool) { finished = true; });
        s.startStream(prog);
        check(s.canLiftHold(), "hold: possible mid-program");
        check(s.queueHoldPoint({QStringLiteral("G53 G0 Z-5"), QStringLiteral("M5")}), "hold: the lines go in");
        spin(3000, [&] { return reached; });
        check(reached && s.isHeldAtPoint(), "hold: the program stands at the point");
        const int up = grbl.seen.indexOf("G53 G0 Z-5"), off = grbl.seen.indexOf("M5");
        check(up >= 0 && off > up, "hold: Z up first, then the spindle off");
        const int sentAtHold = int(grbl.seen.size());
        spin(400, [] { return false; });
        check(int(grbl.seen.size()) == sentAtHold && !finished, "hold: nothing more is sent while held");
        const QStringList down = s.resumeLinesAfterHold();
        check(down.contains(QStringLiteral("M03S16000")) && down.contains(QStringLiteral("G1F600.0"))
                  && down.contains(QStringLiteral("G4P3.00")),
              "hold: the way back starts the spindle and restores the feed");
        s.continueAfterHold(down);
        spin(6000, [&] { return finished; });
        check(finished, "hold: the program carries on to its end");
        check(grbl.seen.lastIndexOf("M03S16000") > off, "hold: the spindle comes on again after the lift");
        // The flags above end with this block; the streamer lives on.
        QObject::disconnect(&s, &c2d::GrblStreamer::holdPointReached, nullptr, nullptr);
        QObject::disconnect(&s, &c2d::GrblStreamer::streamFinished, nullptr, nullptr);
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

    // Start again from a chosen step: a two-toolpath job started from the
    // second toolpath streams the way in (rise, across at the top, spindle
    // and its wait, down), then exactly the second toolpath and nothing of
    // the first. With another bit wanted, the tool change comes first.
    {
        using c2d::GrblStreamer;
        QObject::disconnect(&s, &c2d::GrblStreamer::streamFinished, nullptr, nullptr);
        QObject::disconnect(&s, &c2d::GrblStreamer::toolChangeRequested, nullptr, nullptr);
        const QStringList job = {
            "G90G21", "(Outline)", "G0Z5.000", "M03S18000", "G4P3.00", "G0X10.000Y10.000",
            "G1Z-1.000F300.0", "G1X20.000F1200.0", "G0Z5.000",
            "(Pocket)", "G0X30.000Y40.000", "G1Z-2.000F250.0", "G1X35.000F1000.0", "G0Z5.000", "M05"};
        const QStringList rise = {QStringLiteral("G90"), QStringLiteral("G53 G0 Z-2.500")};
        // Queue line of the second toolpath's first move: comments take no index.
        const int fromQ = 8;
        check(GrblStreamer::rawIndexOfQueued(job, fromQ) == 10, "start here: queue to raw line");
        QString why;
        const QStringList run = GrblStreamer::startFromLines(job, fromQ, rise, -1, QString(), &why);
        const QStringList want = {"G21", "G90", "G17", "G90", "G53 G0 Z-2.500", "G90 G0 X20.000 Y10.000",
                                  "M3 S18000", "G4P3.00", "G0 Z5.000", "G1 F1200.0", "G0"};
        check(run.mid(0, want.size()) == want, "start here: rise, across at the top, spindle, wait, down");
        check(run.mid(want.size()) == job.mid(9), "start here: then the second toolpath, its name and nothing else");
        check(!run.contains("G1X20.000F1200.0") && !run.contains("(Outline)"), "start here: nothing of the first toolpath");
        // Started on a pass that begins below the top: the last bit goes slowly.
        {
            const QStringList deep = {"G90G21", "G0Z5", "M03S10000", "G0X1Y2", "G1Z-1F200", "G1X5F900",
                                      "G1Z-2F200", "G1X1F900"};
            const QStringList d = GrblStreamer::startFromLines(deep, 6, rise, -1, QStringLiteral("M8"), &why);
            check(d.contains("G0 Z2.500") && d.contains("G1 Z-1.000 F200") && d.contains("M8")
                      && d.indexOf("G1 Z-1.000 F200") < d.indexOf("G1Z-2F200")
                      && d.indexOf("G90 G0 X5.000 Y2.000") < d.indexOf("M3 S10000"),
                  "start here: down to the pass's Z at the plunge feed, coolant with the spindle");
        }
        // Started on a block that continues a modal arc: G2 goes back in.
        {
            const QStringList arc = {"G90G21", "G0Z5", "M03S10000", "G0X10Y0", "G1Z-1F200",
                                     "G2X0Y10I-10J0F800", "X-10Y0I0J-10"};
            const QStringList a = GrblStreamer::startFromLines(arc, 6, rise, -1, QString(), &why);
            check(a.size() > 2 && !a.contains("G2") && a.last() == "G2 X-10Y0I0J-10"
                      && a.contains("G90 G0 X0.000 Y10.000"),
                  "start here: a modal arc keeps its G2, on the arc block itself");
            QList<QByteArray> sent;
            for (const QString &l : a)
                sent << l.toLatin1();
            check(arcsConsistent(sent), "start here: the arc runs from its own start, end on its circle");
            const QStringList own = GrblStreamer::startFromLines(
                {"G90G21", "G0Z5", "M03S10000", "G0X10Y0", "G1Z-1F200", "G2X0Y10I-10J0F800", "G1X-10Y0"},
                6, rise, -1, QString(), &why);
            check(own.last() == "G1X-10Y0" && !own.contains("G2"),
                  "start here: a move with its own G word is left alone");
        }
        check(GrblStreamer::startFromLines({"G0Z5", "G91", "G1Z-1F300", "G1X1"}, 3, rise, -1, QString(), &why).isEmpty()
                  && why.contains(QLatin1String("G91")),
              "start here: refused after G91");
        check(GrblStreamer::startFromLines(job, fromQ, {}, -1, QString(), &why).isEmpty(),
              "start here: refused without a way up");
        int last = 0;
        check(GrblStreamer::toolMarkersBefore({"G0Z5", "M0 ;T3", "G1X1F100", "M0 ;T4", "G1X2"}, 4, &last) == 2
                  && last == 4, "start here: tool markers before a line");

        // Streamed: the program as sent to the controller.
        bool finished = false, finishedOk = false;
        QList<int> parks;
        QObject::connect(&s, &c2d::GrblStreamer::streamFinished, [&](bool ok) {
            finished = true;
            finishedOk = ok;
        });
        QObject::connect(&s, &c2d::GrblStreamer::toolChangeRequested, [&](int t) { parks << t; });
        grbl.seen.clear();
        s.startStream(run);
        spin(3000, [&] { return finished; });
        QList<QByteArray> expect;
        for (const QString &l : run)
            if (!l.startsWith(QLatin1Char('(')))
                expect << l.toLatin1();
        check(finished && finishedOk && grbl.seen == expect, "start here: streamed as built, nothing else");

        // Another bit: spindle off, park at the marker, and only after the
        // change the way in to the second toolpath.
        finished = false;
        grbl.seen.clear();
        const QStringList tc = GrblStreamer::startFromLines(job, fromQ, rise, 2, QString(), &why);
        check(tc.indexOf("M5") < tc.indexOf("M0 ;T2") && tc.indexOf("M0 ;T2") < tc.indexOf("G90 G0 X20.000 Y10.000"),
              "start here: tool change first");
        s.startStream(tc);
        spin(2000, [&] { return parks.size() == 1; });
        check(parks.size() == 1 && parks.at(0) == 2 && !grbl.seen.contains("M3 S18000")
                  && grbl.seen.contains("M5"),
              "start here: parks for the bit before the spindle starts");
        s.continueAfterToolChange(rise);
        spin(3000, [&] { return finished; });
        check(finished && finishedOk && grbl.seen.contains("M3 S18000") && grbl.seen.last() == "M05"
                  && grbl.seen.contains("G1X35.000F1000.0") && !grbl.seen.contains("G1X20.000F1200.0"),
              "start here: after the change, the second toolpath runs");
        QObject::disconnect(&s, &c2d::GrblStreamer::streamFinished, nullptr, nullptr);
        QObject::disconnect(&s, &c2d::GrblStreamer::toolChangeRequested, nullptr, nullptr);
    }

    s.disconnectPort();
    ::close(slave);
    ::close(master);
    std::printf("test_streamer: %d checks OK\n", g_checks);
    return 0;
}
