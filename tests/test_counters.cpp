// Run-time counters and service reminders (machinecounters.h): a scripted
// run of status reports, each with the time since the one before, adds up
// spindle hours, moving hours and per-bit cutting time; a reminder becomes
// due at its interval and Done starts it again; the counters survive a
// save and load per machine profile.
//
// Plain asserts, no framework; exits 0 on success.

#include "../src/machinecounters.h"

#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdio>
#include <cstdlib>

using namespace c2d;
using namespace c2d::machinecounters;

static int g_checks = 0;

static void check(bool cond, const char *what)
{
    ++g_checks;
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

static bool near(double a, double b) { return std::abs(a - b) < 1e-6; }

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tmp.path());

    Counters c;
    c.reminders = defaultReminders();
    check(c.reminders.size() == 3, "three default reminders");
    check(dueLine(c).isEmpty(), "nothing due on a new machine");

    // A job: 30 simulated hours of Run with the spindle at 18000 (FS:),
    // reports one second apart, cutting with bit "aa9b3a".
    Sample run;
    run.state = QStringLiteral("Run");
    run.spindleRpm = 18000;
    run.spindleReported = true;
    run.bitId = QStringLiteral("aa9b3a");
    run.bitName = QStringLiteral("1/4\" flat");
    const int jobSec = 30 * 3600;
    for (int i = 0; i < jobSec; ++i)
        accumulate(c, 1.0, run);
    check(near(c.spindleSec, jobSec), "spindle seconds of the job");
    check(near(c.motionSec, jobSec), "moving seconds of the job");
    check(near(c.bitSec.value(QStringLiteral("aa9b3a")), jobSec), "bit cutting time");
    check(c.bitName.value(QStringLiteral("aa9b3a")) == QStringLiteral("1/4\" flat"), "bit name kept");
    check(dueLine(c).isEmpty(), "30 spindle hours: nothing due yet");

    // Jogging with the spindle off: motion only.
    Sample jog;
    jog.state = QStringLiteral("Jog");
    jog.spindleReported = true;
    for (int i = 0; i < 12 * 3600; ++i)
        accumulate(c, 1.0, jog);
    check(near(c.spindleSec, jobSec), "a jog adds no spindle time");
    check(near(c.motionSec, 42.0 * 3600), "a jog adds moving time");
    check(dueLine(c) == QStringLiteral("Due: lubricate lead screws (42 h)"), "lead screws due at 40 moving hours");

    // Idle with the spindle running (warm-up): spindle only, no bit time.
    Sample warm;
    warm.state = QStringLiteral("Idle");
    warm.spindleRpm = 10000;
    warm.spindleReported = true;
    warm.bitId = QStringLiteral("aa9b3a");
    for (int i = 0; i < 22 * 3600; ++i)
        accumulate(c, 1.0, warm);
    check(near(c.spindleSec, 52.0 * 3600), "warm-up adds spindle time");
    check(near(c.motionSec, 42.0 * 3600), "standing still adds no moving time");
    check(near(c.bitSec.value(QStringLiteral("aa9b3a")), jobSec), "no cutting time standing still");
    check(dueLine(c) == QStringLiteral("Due: router brushes (52 h) · lubricate lead screws (42 h)"),
          "brushes due at 50 spindle hours");

    // No FS: (no variable spindle): the job's M3 state counts.
    Sample noFs;
    noFs.state = QStringLiteral("Run");
    noFs.jobSpindleOn = true;
    const double before = c.spindleSec;
    accumulate(c, 2.0, noFs);
    check(near(c.spindleSec, before + 2), "job M3 counts without FS:");
    noFs.jobSpindleOn = false;
    accumulate(c, 2.0, noFs);
    check(near(c.spindleSec, before + 2), "no M3, no FS: no spindle time");

    // A stalled link is not machine time; a negative delta is ignored.
    const double m = c.motionSec;
    accumulate(c, 600.0, jog);
    check(near(c.motionSec, m + 5), "a long gap counts 5 s at most");
    accumulate(c, -3.0, jog);
    check(near(c.motionSec, m + 5), "a negative delta adds nothing");

    // Done starts the brushes again; the interval can be changed.
    c.markDone(0);
    check(near(c.sinceHours(c.reminders.at(0)), 0), "Done resets the brushes");
    check(dueLine(c) == QStringLiteral("Due: lubricate lead screws (42 h)"), "only the screws left due");
    c.reminders[2].everyHours = 60;
    check(dueLine(c).isEmpty(), "a longer interval: not due");

    // Saved and loaded per profile; another profile starts at zero.
    {
        QSettings s;
        save(s, QStringLiteral("m1"), c);
        const Counters back = load(s, QStringLiteral("m1"));
        check(near(back.spindleSec, c.spindleSec) && near(back.motionSec, c.motionSec), "totals survive");
        check(near(back.bitSec.value(QStringLiteral("aa9b3a")), jobSec), "bit time survives");
        check(back.bitName.value(QStringLiteral("aa9b3a")) == c.bitName.value(QStringLiteral("aa9b3a")),
              "bit name survives");
        check(near(back.reminders.at(0).doneAtSec, c.reminders.at(0).doneAtSec), "Done survives");
        check(near(back.reminders.at(2).everyHours, 60), "interval survives");
        const Counters other = load(s, QStringLiteral("x2"));
        check(other.spindleSec == 0 && other.bitSec.isEmpty() && other.reminders.size() == 3,
              "another machine has its own counters");
        forget(s, QStringLiteral("m1"));
        check(load(s, QStringLiteral("m1")).spindleSec == 0, "forget clears a machine");
    }
    check(hoursText(90) == QStringLiteral("2 min") && hoursText(5400) == QStringLiteral("1.5 h"),
          "hours text");

    std::printf("counters: %d checks passed\n", g_checks);
    return 0;
}
