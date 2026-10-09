// The gamepad reader (src/gamepad.cpp) without a gamepad.
//
// /dev/input/js* is a stream of fixed 8-byte records, so a FIFO carrying
// synthetic ones exercises the real code path: the same open, the same
// QSocketNotifier, the same parsing. What cannot be checked here is which
// physical button a given clone calls "A" — that needs the hardware.

#include "../src/gamepad.h"
#include "../src/gamepadmapdialog.h"

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QSettings>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>

#include <fcntl.h>
#include <linux/joystick.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

static int g_checks = 0;

static void check(bool cond, const char *what)
{
    ++g_checks;
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

// Let the notifier run until `spy` has `want` rows, or we give up.
static void pump(QSignalSpy &spy, int want, int msMax = 2000)
{
    QElapsedTimer t;
    t.start();
    while (spy.count() < want && t.elapsed() < msMax)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

static void writeEvent(int fd, quint8 type, quint8 number, qint16 value)
{
    js_event e{};
    e.time = 0;
    e.value = value;
    e.type = type;
    e.number = number;
    const ssize_t n = ::write(fd, &e, sizeof(e));
    check(n == sizeof(e), "wrote one event record");
}

// The action shown in the mapping dialog's row for `button`, or "" if no row.
static QString dialogAction(const c2d::GamepadMapDialog &dlg, int button)
{
    const auto *t = dlg.findChild<QTableWidget *>();
    for (int r = 0; t && r < t->rowCount(); ++r)
        if (t->item(r, 0)->data(Qt::UserRole).toInt() == button)
            return qobject_cast<QComboBox *>(t->cellWidget(r, 1))->currentData().toString();
    return QString();
}

static void setDialogAction(c2d::GamepadMapDialog &dlg, int button, const QString &action)
{
    auto *t = dlg.findChild<QTableWidget *>();
    for (int r = 0; r < t->rowCount(); ++r)
        if (t->item(r, 0)->data(Qt::UserRole).toInt() == button) {
            auto *c = qobject_cast<QComboBox *>(t->cellWidget(r, 1));
            c->setCurrentIndex(c->findData(action));
            return;
        }
    check(false, "the row to change exists");
}

static void mappingTests(const QTemporaryDir &dir, c2d::Gamepad &pad, int writer)
{
    using namespace c2d;
    const QString ini = dir.filePath(QStringLiteral("map.ini"));

    // Nothing stored: the SNES defaults.
    {
        QSettings s(ini, QSettings::IniFormat);
        check(gamepadmap::load(s) == gamepadmap::defaults(), "no settings loads the defaults");
    }
    // Every default action is one the panel knows how to perform.
    for (const QString &a : gamepadmap::defaults())
        check(gamepadmap::actions().contains(a), "default actions are real actions");
    check(!gamepadmap::actions().contains(QStringLiteral("run"))
              && !gamepadmap::actions().contains(QStringLiteral("start")),
          "starting a program is never a pad action");

    // Round trip, including a default button switched off: it must stay off,
    // not come back from the defaults on the next load.
    GamepadMap m = gamepadmap::defaults();
    m.remove(1);                                  // B no longer stops
    m.insert(1 + 10, QStringLiteral("stop"));     // button 11 does
    m.insert(0, QStringLiteral("x+"));            // A jogs instead of zeroing
    {
        QSettings s(ini, QSettings::IniFormat);
        gamepadmap::save(s, m);
    }
    {
        QSettings s(ini, QSettings::IniFormat);
        check(gamepadmap::load(s) == m, "a saved mapping loads back unchanged");
        check(s.value(QStringLiteral("gamepad/1")).toString() == QLatin1String("none"),
              "an unmapped default button is stored as none");
    }
    // Saving again replaces, rather than merges with, the old mapping.
    {
        QSettings s(ini, QSettings::IniFormat);
        gamepadmap::save(s, gamepadmap::defaults());
    }
    {
        QSettings s(ini, QSettings::IniFormat);
        check(gamepadmap::load(s) == gamepadmap::defaults(), "re-saving drops old entries");
    }

    // The dialog: it shows the mapping it was given and hands it back as is.
    GamepadMapDialog dlg(m, &pad);
    check(dlg.mapping() == m, "the dialog returns the mapping it was given");
    check(dialogAction(dlg, 0) == QLatin1String("x+"), "and shows each button's action");

    // Pressing a pad button that has no row adds one, mapped to nothing until
    // an action is picked, so finding a button never changes the mapping.
    writeEvent(writer, JS_EVENT_BUTTON, 13, 1);
    QElapsedTimer t;
    t.start();
    while (dialogAction(dlg, 13).isEmpty() && t.elapsed() < 2000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    check(dialogAction(dlg, 13) == QLatin1String("none"), "a pad press adds a row for its button");
    check(dlg.mapping() == m, "which maps to nothing until an action is chosen");
    setDialogAction(dlg, 13, QStringLiteral("unlock"));
    check(dlg.mapping().value(13) == QLatin1String("unlock"), "choosing an action maps it");
    setDialogAction(dlg, 0, QStringLiteral("none"));
    check(!dlg.mapping().contains(0), "choosing nothing unmaps a button");

    // A hand-edited setting that is not an action is shown, not rewritten.
    GamepadMap odd;
    odd.insert(7, QStringLiteral("launch"));
    GamepadMapDialog dlg2(odd, nullptr);
    check(dlg2.mapping() == odd, "an unknown action survives a trip through the dialog");
}

int main(int argc, char *argv[])
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QTemporaryDir dir;
    check(dir.isValid(), "scratch directory");
    const QString fifo = dir.filePath(QStringLiteral("js-test"));
    check(::mkfifo(fifo.toLocal8Bit().constData(), 0600) == 0, "made a fifo");

    c2d::Gamepad pad;
    QSignalSpy buttons(&pad, &c2d::Gamepad::buttonChanged);
    QSignalSpy axes(&pad, &c2d::Gamepad::axisChanged);
    QSignalSpy gone(&pad, &c2d::Gamepad::disconnected);

    // The reader opens first: a fifo opened read-only, non-blocking, succeeds
    // with no writer yet, which is also how a real device behaves when idle.
    check(pad.openPath(fifo, false), "opened the device");
    check(pad.isConnected(), "and reports itself connected");

    const int w = ::open(fifo.toLocal8Bit().constData(), O_WRONLY);
    check(w >= 0, "opened the writing end");

    // The driver replays the resting state of every control when the device is
    // opened, flagged JS_EVENT_INIT. Acting on those would fire a press for
    // every button the instant a pad is plugged in.
    writeEvent(w, JS_EVENT_BUTTON | JS_EVENT_INIT, 0, 1);
    writeEvent(w, JS_EVENT_AXIS | JS_EVENT_INIT, 0, -32767);
    pump(buttons, 1, 300);
    check(buttons.count() == 0, "the initial state replay is not reported as input");
    check(axes.count() == 0, "neither for axes");

    // A real press and release.
    writeEvent(w, JS_EVENT_BUTTON, 4, 1);
    pump(buttons, 1);
    check(buttons.count() == 1, "a button press arrives");
    check(buttons.at(0).at(0).toInt() == 4, "with its number");
    check(buttons.at(0).at(1).toBool(), "and is a press");

    writeEvent(w, JS_EVENT_BUTTON, 4, 0);
    pump(buttons, 2);
    check(buttons.count() == 2, "the release arrives too");
    check(!buttons.at(1).at(1).toBool(), "and is a release");

    // A D-pad: full scale one way, back to rest, full scale the other.
    writeEvent(w, JS_EVENT_AXIS, 1, -32767);
    writeEvent(w, JS_EVENT_AXIS, 1, 0);
    writeEvent(w, JS_EVENT_AXIS, 0, 32767);
    pump(axes, 3);
    check(axes.count() == 3, "three axis readings arrive");
    check(axes.at(0).at(0).toInt() == 1 && axes.at(0).at(1).toInt() == -32767,
          "the first is axis 1 at full negative");
    check(axes.at(2).at(0).toInt() == 0 && axes.at(2).at(1).toInt() == 32767,
          "the last is axis 0 at full positive");

    // Several records in one write must all be delivered: the reader drains
    // the device rather than taking one event per wake-up.
    const int before = buttons.count();
    for (int i = 0; i < 8; ++i)
        writeEvent(w, JS_EVENT_BUTTON, i, 1);
    pump(buttons, before + 8);
    check(buttons.count() == before + 8, "a burst of events is drained in one go");

    mappingTests(dir, pad, w);

    // Unplugging is not an error: the writer closing looks exactly like it.
    ::close(w);
    pump(gone, 1);
    check(gone.count() == 1, "closing the device reports a disconnect");
    check(!pad.isConnected(), "and it is no longer connected");

    // A device that cannot answer "how many buttons have you got?" is not a
    // gamepad. On a MacBook js0 is the lid accelerometer -- two axes, no
    // buttons -- and jogging a machine from that would be memorable.
    {
        c2d::Gamepad strict;
        check(!strict.openPath(fifo, true), "a device with no buttons is turned down");
        check(!strict.isConnected(), "and is not left open");
    }

    std::printf("test_gamepad: %d checks OK\n", g_checks);
    return 0;
}
