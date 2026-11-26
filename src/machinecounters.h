#pragma once
#include <QHash>
#include <QString>
#include <QVector>

class QSettings;
class QWidget;

// Run-time counters and service reminders, one set per machine profile
// (machineprofiles.h), kept in QSettings under counters/<profile id>/ so a
// profile switch never mixes two machines' hours.
//
// The Machine page feeds every status report through accumulate() with the
// time since the previous one; the counting itself is a pure function of
// those deltas.
namespace c2d::machinecounters {

struct Reminder {
    QString key;            // "brushes", "wheels", "screws"
    QString name;           // "Router brushes"
    bool spindleBased = true;   // spindle hours, else motion hours
    double everyHours = 50;
    double doneAtSec = 0;   // the basis counter (seconds) when last marked done
};

struct Counters {
    double spindleSec = 0;  // spindle turning
    double motionSec = 0;   // Run or Jog
    QHash<QString, double> bitSec;     // tool library bit id -> seconds cutting
    QHash<QString, QString> bitName;   // last name seen for a bit id
    QVector<Reminder> reminders;

    double basisSec(const Reminder &r) const { return r.spindleBased ? spindleSec : motionSec; }
    double sinceHours(const Reminder &r) const { return (basisSec(r) - r.doneAtSec) / 3600.0; }
    bool due(const Reminder &r) const { return r.everyHours > 0 && sinceHours(r) >= r.everyHours; }
    void markDone(int i);
};

QVector<Reminder> defaultReminders();

// One status report as the counters see it.
struct Sample {
    QString state;               // Idle / Run / Jog / Hold:0 ...
    double spindleRpm = 0;       // FS: speed
    bool spindleReported = false;    // the report had FS: (variable spindle)
    bool jobSpindleOn = false;   // no FS: — a job is cutting a toolpath (its M3 is on)
    QString bitId, bitName;      // the running toolpath's bit, empty when unknown
};

bool spindleRunning(const Sample &s);
bool moving(const QString &state);
// Adds `dtSec` (clamped to 0..5 s: a stalled link is not machine time) to the
// counters that the sample says were running.
void accumulate(Counters &c, double dtSec, const Sample &s);

// "Due: router brushes (52 h)", several joined by " · "; empty when none is due.
QString dueLine(const Counters &c);

Counters load(QSettings &s, const QString &profileId);
void save(QSettings &s, const QString &profileId, const Counters &c);
void forget(QSettings &s, const QString &profileId);

// "12.3 h" / "45 min"
QString hoursText(double sec);

// Totals, per-bit times (top 20) and the reminders with Done and their
// intervals. Edits `c` in place; returns true when something changed.
bool showDialog(QWidget *parent, Counters &c, const QString &machineName);

} // namespace c2d::machinecounters
