// Hold, Resume and Stop from a pad: the same paths as the Machine page's
// Hold / Resume and Stop buttons, on a controller faked by a pseudo-terminal
// that only records what it is sent. Hold and Stop are always taken; Resume
// only with Machine > Pads may start cutting on.
//
// Skips (exit 77) where no pty can be opened as a serial port.
// Plain checks, no framework; exits 0 on success.
#define private public
#include "../src/mainwindow.h"
#include "../src/machinepanel.h"
#undef private
#include "../src/grblstreamer.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QPlainTextEdit>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cstdio>
#include <fcntl.h>
#include <pty.h>
#include <termios.h>
#include <unistd.h>

static int g_fails = 0;
static void check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fails;
}

// Everything the panel wrote to the controller since the last call.
static QByteArray drain(int fd)
{
    QElapsedTimer t;
    t.start();
    QByteArray all;
    while (t.elapsed() < 150) {
        QApplication::processEvents();
        char buf[512];
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n > 0)
            all.append(buf, int(n));
        else
            usleep(5000);
    }
    return all;
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir home;
    qputenv("HOME", home.path().toLocal8Bit());
    qunsetenv("XDG_DOCUMENTS_DIR");
    qunsetenv("XDG_DATA_HOME");
    qunsetenv("XDG_CONFIG_HOME");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("phobicccp-test"));
    QCoreApplication::setApplicationName(QStringLiteral("test_padstop"));
    QSettings().clear();

    int master = -1, slave = -1;
    char name[256] = {};
    if (openpty(&master, &slave, name, nullptr, nullptr) != 0) {
        std::printf("test_padstop: no pty, skipped\n");
        return 77;
    }
    termios tio;
    tcgetattr(slave, &tio);
    cfmakeraw(&tio);
    tcsetattr(slave, TCSANOW, &tio);
    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);

    c2d::MainWindow w;
    w.resize(1200, 800);
    w.show();
    w.newUntitled(300, 200, 19);
    QApplication::processEvents();
    c2d::MachinePanel *panel = w.m_machine;
    const QString peer = QStringLiteral("10.0.0.9");
    auto ask = [&](const char *op) {
        w.onPadRequest(1, QJsonObject{{"op", op}, {"pad", "pad-1"}}, peer);
    };
    auto logText = [&] { return panel->m_console->toPlainText(); };

    // Not connected: nothing to hold.
    check(!panel->holdFromRemote(peer, nullptr), "hold refused while not connected");

    check(panel->m_grbl->connectPort(QString::fromLatin1(name)), "connects to the fake controller");
    drain(master);

    ask("hold");
    QByteArray sent = drain(master);
    check(sent.contains('!'), "hold sends a feed hold");
    check(panel->m_held, "hold: the panel is held (its button says Resume)");
    check(logText().contains(QStringLiteral("Hold from pad 10.0.0.9")), "hold is in the machine log");
    check(panel->statusJson().value(QStringLiteral("held")).toBool(), "status says held");

    QSettings().setValue(QStringLiteral("pad/allowStart"), false);
    ask("resume");
    sent = drain(master);
    check(!sent.contains('~') && panel->m_held, "resume refused while pads may not start cutting");
    check(!logText().contains(QStringLiteral("Resume from pad")), "a refused resume is not logged as done");

    QSettings().setValue(QStringLiteral("pad/allowStart"), true);
    ask("resume");
    sent = drain(master);
    check(sent.contains('~') && !panel->m_held, "resume goes through once pads may start cutting");
    check(logText().contains(QStringLiteral("Resume from pad 10.0.0.9")), "resume is in the machine log");

    ask("resume");
    sent = drain(master);
    check(!sent.contains('~'), "resume with nothing held does nothing");

    QSettings().setValue(QStringLiteral("pad/allowStart"), false);
    const quint64 stopSeq = panel->m_stopSeq;
    ask("stop");
    sent = drain(master);
    check(sent.contains(char(0x18)), "stop resets the controller (the Stop button) even with starting off");
    check(panel->m_stopSeq == stopSeq + 1, "stop went through the Stop button");
    check(logText().contains(QStringLiteral("Stop from pad 10.0.0.9")), "stop is in the machine log");

    panel->m_grbl->disconnectPort();
    ::close(master);
    ::close(slave);
    w.m_dirty = false;
    std::printf("%s\n", g_fails ? "FAIL: test_padstop" : "test_padstop: OK");
    return g_fails ? 1 : 0;
}
