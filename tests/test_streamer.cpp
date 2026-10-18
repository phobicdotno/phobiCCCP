// GrblStreamer against a scripted controller on a pseudo-terminal: what is
// written to the machine when a program line is rejected mid-job.
//
// Skips (exit 77) where no pty can be opened as a serial port.
//
// Plain asserts, no framework; exits 0 on success.

#include "../src/grblstreamer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSocketNotifier>
#include <QTimer>

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

    void send(const QByteArray &s) { (void)!::write(fd, s.constData(), size_t(s.size())); }

    void onRead()
    {
        char buf[512];
        const ssize_t n = ::read(fd, buf, sizeof buf);
        for (ssize_t i = 0; i < n; ++i) {
            const char c = buf[i];
            if (c == '?') {
                send(held ? "<Hold:0|MPos:0.000,0.000,0.000|FS:0,18000>\r\n"
                          : "<Run|MPos:0.000,0.000,0.000|FS:500,18000>\r\n");
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

    s.disconnectPort();
    ::close(slave);
    ::close(master);
    std::printf("test_streamer: %d checks OK\n", g_checks);
    return 0;
}
