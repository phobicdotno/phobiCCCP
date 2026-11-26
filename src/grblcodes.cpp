#include "grblcodes.h"

#include <QRegularExpression>

#include <iterator>

namespace c2d {
namespace grblcodes {

namespace {

struct Row {
    int code;
    const char *meaning;
    const char *brief;
    const char *cause;
    const char *fix;
};

// GRBL 1.1 error codes. 18 and 19 are unused in GRBL 1.1; grblHAL uses them.
const Row kErrors[] = {
    {1, "Command letter missing",
     "a line has a number without a command letter",
     "A G-code line has a number where a letter (G, X, F...) should be. Usually a damaged or hand-edited program file.",
     "Open the program in a text editor at the line in the log, or make the toolpath again and re-export it."},
    {2, "Bad number",
     "a number in the program could not be read",
     "A value is missing or not a number (two dots, a stray character). Usually a damaged or hand-edited program file.",
     "Make the toolpath again and re-export, or fix the line shown in the log."},
    {3, "Unknown $ command",
     "the controller did not know a $ command",
     "A $ command the controller does not know was sent, often a typo in the console or a command meant for another firmware.",
     "Check the command spelling. $$ lists settings, $H homes, $X unlocks."},
    {4, "Negative value not allowed",
     "a value that must be positive was negative",
     "A setting or command that needs a positive number got a negative one.",
     "Send the value again without the minus sign."},
    {5, "Homing is turned off",
     "homing is turned off in the controller",
     "Homing ($H) was asked for but homing is off in the controller ($22=0). On a Shapeoko it should be on.",
     "Turn homing on with $22=1, then Initialize."},
    {6, "Step pulse too short",
     "the step pulse setting is too short",
     "The step pulse time ($0) was set below 3 microseconds.",
     "Set $0 back to its default (10 on a Shapeoko)."},
    {7, "Settings memory read failed",
     "the controller could not read its settings",
     "The controller could not read its stored settings and went back to defaults. The board may have lost power while saving.",
     "Check $$ against your machine's settings (steps/mm, travel, homing) before you cut. Restore them if they reset."},
    {8, "Machine not idle",
     "the machine was not idle for a $ command",
     "A $ command was sent while the machine was moving, holding or running a job.",
     "Wait until the machine is Idle, then send it again."},
    {9, "Locked: alarm or jog",
     "the controller is locked by an alarm",
     "G-code was sent while the controller is in Alarm (after an end stop, a reset or at power-on) or while a jog runs.",
     "Press Initialize to home the machine, then send it again."},
    {10, "Soft limits need homing",
     "soft limits need homing turned on",
     "Soft limits ($20=1) were turned on while homing is off.",
     "Turn homing on ($22=1) first, then soft limits."},
    {11, "Line too long",
     "a program line is too long for the controller",
     "A G-code line has more characters than the controller can take (about 80). Long comments or very precise numbers can do it.",
     "Re-export the program; remove long comments from the line in the log."},
    {12, "Step rate too high",
     "a setting asks for a step rate above the controller's limit",
     "A max rate setting ($110-$112) with the steps/mm gives more than 30 000 steps a second.",
     "Lower the max rate setting you just changed."},
    {13, "Safety door open",
     "the safety door input is open",
     "The safety door input says the door is open. On a Shapeoko this input is often not wired, so check for a loose wire.",
     "Close the door (or fix the door input wire), then resume."},
    {14, "Text too long to store",
     "a startup line or build note is too long",
     "A startup line ($N) or build info ($I) was longer than the controller can store.",
     "Make the text shorter."},
    {15, "Jog past the travel",
     "a jog would go past the machine's travel",
     "A jog asked to go beyond the machine's limits (soft limits are on). Often jogging from an unhomed position.",
     "Jog a shorter step, or Initialize so the machine knows where it is."},
    {16, "Bad jog command",
     "a jog command was not valid",
     "A $J= jog line was badly formed or used a word jogs do not allow.",
     "Jog again with the buttons; if typed in the console, check the command."},
    {17, "Laser mode needs PWM",
     "laser mode needs a PWM spindle output",
     "Laser mode ($32=1) was asked for but the spindle output is not PWM.",
     "Leave laser mode off ($32=0) on a router."},
    {18, "Reset asserted (grblHAL)",
     "the controller's reset was held",
     "grblHAL only: a reset or e-stop input is active. GRBL 1.1 does not use this code.",
     "Release the e-stop or reset input, then reset the controller and Initialize."},
    {19, "Value must be above zero (grblHAL)",
     "a value that must be above zero was zero or less",
     "grblHAL only: a setting or word that must be above zero got zero or less. GRBL 1.1 does not use this code.",
     "Send the value again above zero."},
    {20, "Unsupported G-code",
     "the program uses a G-code the controller does not know",
     "The program has a G or M code GRBL does not support (G41/G42 cutter compensation, G81 drill cycles, a macro).",
     "Re-export with a GRBL post-processor; this app's own export only uses supported codes."},
    {21, "Two codes from one group",
     "one line has two codes that cannot go together",
     "One G-code line has two commands from the same group, like G0 and G1 together.",
     "Fix the line shown in the log, or re-export the program."},
    {22, "Feed rate not set",
     "a cutting move came before any feed rate",
     "A G1/G2/G3 cutting move came before any F feed rate was given. Often a program started from the middle, or a hand-written one.",
     "Add a feed rate (F...) before the first cutting move, or run the whole program from the start."},
    {23, "Needs a whole number",
     "a code that needs a whole number got a decimal",
     "A command that needs a whole number got a decimal (a tool number T1.5, M3.5).",
     "Fix the line shown in the log."},
    {24, "Two codes both want axis words",
     "two commands on one line both want X/Y/Z",
     "One line has two commands that both use X/Y/Z, like G10 with G0.",
     "Split the line in two, or re-export the program."},
    {25, "Repeated word",
     "a word is repeated on one line",
     "The same word appears twice on one line (two X values, two F values).",
     "Fix the line shown in the log, or re-export the program."},
    {26, "Axis words missing",
     "a command needs X/Y/Z but has none",
     "A command that needs X, Y or Z has none, like a G1 with only a feed rate in some cases, or a G38 probe with no target.",
     "Fix the line shown in the log, or re-export the program."},
    {27, "Bad line number",
     "a line number is out of range",
     "An N line number is above what GRBL accepts (9 999 999).",
     "Re-export without line numbers, or with smaller ones."},
    {28, "Value missing",
     "a command is missing a required value",
     "A command is missing a value it needs, like G10 without P or L, or a G4 dwell without P.",
     "Fix the line shown in the log, or re-export the program."},
    {29, "G59.1-G59.3 not supported",
     "the program uses G59.1-G59.3",
     "The program uses work offsets G59.1, G59.2 or G59.3, which GRBL does not have.",
     "Use G54-G59 instead."},
    {30, "G53 only with G0/G1",
     "G53 was used with a move other than G0 or G1",
     "A machine-coordinate move (G53) was combined with an arc or other motion. GRBL only allows G53 with G0 or G1.",
     "Use G53 G0 or G53 G1 only."},
    {31, "Axis words not used",
     "a line has X/Y/Z that no command uses",
     "A line has X/Y/Z values but no command that uses them, for example after a G80 cancel.",
     "Fix the line shown in the log, or re-export the program."},
    {32, "Arc missing an axis word",
     "an arc has no end point in its plane",
     "A G2/G3 arc has no X/Y (in the XY plane) end point.",
     "Re-export the program; check the arc settings of the post-processor."},
    {33, "Bad move target",
     "a move or arc has an impossible target",
     "A move or arc target cannot be reached: an arc whose start and end do not fit its centre, or a probe move to where the probe already is.",
     "Re-export the program. If it was a probe, jog the probe away a little and try again."},
    {34, "Bad arc radius",
     "an arc radius does not fit its ends",
     "An R-style arc's radius is too small for its start and end points, often from rounding in a program with tiny arcs.",
     "Re-export with arcs as I/J centres, or with more decimals."},
    {35, "Arc missing centre offset",
     "an arc has no centre offset",
     "A G2/G3 arc with I/J/K style has no offset in its plane.",
     "Re-export the program."},
    {36, "Unused words",
     "a line has values no command uses",
     "A line has extra words that no command on it uses (a stray P or an S where it does not belong).",
     "Fix the line shown in the log, or re-export the program."},
    {37, "Tool length offset axis wrong",
     "a tool length offset was not on Z",
     "A G43.1 tool length offset used an axis other than Z.",
     "Use G43.1 with Z only."},
    {38, "Tool number too high",
     "the tool number is too high",
     "A T tool number is above what the controller allows (255).",
     "Use a lower tool number in the tool library and re-export."},
};

// GRBL 1.1 alarms; 10 is the dual-switch squaring failure on GRBL builds
// that auto-square (grblHAL uses it for the e-stop).
const Row kAlarms[] = {
    {1, "Hard limit: an end stop fired",
     "an end stop fired",
     "An end stop (homing sensor) was hit during a move: the program or a jog went past the travel, the work offset is wrong, or a chip or loose wire tripped a sensor. Spindle noise on the sensor wires can also do it.",
     "Reset the controller, then Initialize to home. Check the job fits the table from the work zero and that no sensor LED is lit when nothing is near it."},
    {2, "Soft limit: move past the travel",
     "a move went past the travel",
     "A move in the program would go beyond the machine's travel. Usually the work zero is set too close to an edge, or the job is bigger than expected, or the Z zero is too low for the program's depth.",
     "Reset the controller, Initialize, then check the X/Y/Z zero and the job size against the machine's travel."},
    {3, "Reset while moving",
     "the controller was reset while moving",
     "The controller was reset (Stop, a USB glitch or a power dip) while the machine was moving, so the steps were lost.",
     "Initialize to home again; the position is no longer trusted. If it happens on its own, check the USB cable and grounding."},
    {4, "Probe already touching",
     "the probe was already touching before probing",
     "Probing started with the probe already triggered: the BitSetter switch stuck down, the BitZero touching the bit or its magnet clip on the block, or a shorted probe wire.",
     "Jog away so nothing touches, check that the probe light is off, then probe again. The position is kept."},
    {5, "Probe found nothing",
     "the probe did not touch anything within its travel",
     "The probe moved its full distance without touching: on the BitSetter the bit missed the button (wrong BitSetter spot or a short bit), on the BitZero the magnet clip was not on the bit or the probe was too far away.",
     "Put the clip on the bit, jog closer (BitZero) or check the BitSetter position, then probe again. The position is kept."},
    {6, "Homing stopped by a reset",
     "homing was stopped by a reset",
     "The controller was reset while it was homing.",
     "Initialize again."},
    {7, "Homing stopped: door opened",
     "the safety door opened during homing",
     "The safety door input opened while homing.",
     "Close the door (or check the door input wire), then Initialize again."},
    {8, "Homing could not leave the switch",
     "homing could not move off a limit switch",
     "After touching a switch, homing backed off but the sensor stayed on: a sensor stuck on, metal in front of it, or a pull-off ($27) too short.",
     "Check every homing sensor's LED goes out when nothing is in front of it, then Initialize again."},
    {9, "Homing did not find a switch",
     "homing could not find a limit switch",
     "Homing moved its full search distance without a sensor firing: a sensor or its cable is out (on the Shapeoko 5 often the back-left Y sensor), or the axis is stalled.",
     "Check that every homing sensor's LED lights when metal is in front of it, including both Y sensors, then Initialize again."},
    {10, "Homing: second switch not found",
     "homing could not find the second Y switch",
     "On a machine that squares its gantry while homing, the second switch on that axis did not fire. A loose sensor cable or a stalled side. (grblHAL uses alarm 10 for the e-stop.)",
     "Check both sensors on that axis and their cables, release any e-stop, then Initialize again."},
};

Explained fromTable(const Row *rows, int n, int code)
{
    for (int i = 0; i < n; ++i) {
        if (rows[i].code == code) {
            Explained e;
            e.known = true;
            e.meaning = QString::fromUtf8(rows[i].meaning);
            e.brief = QString::fromUtf8(rows[i].brief);
            e.cause = QString::fromUtf8(rows[i].cause);
            e.fix = QString::fromUtf8(rows[i].fix);
            return e;
        }
    }
    return {};
}

} // namespace

Explained error(int code)
{
    Explained e = fromTable(kErrors, int(std::size(kErrors)), code);
    if (!e.known) {
        e.meaning = QStringLiteral("Unknown error %1").arg(code);
        e.brief = QStringLiteral("the controller reported error %1").arg(code);
        e.cause = QStringLiteral("The controller refused a line with a code this app does not know. "
                                 "It may be a newer or different firmware.");
        e.fix = QStringLiteral("Look up error %1 for your controller's firmware, fix the line shown "
                               "in the log, and run again.").arg(code);
    }
    return e;
}

Explained alarm(int code)
{
    Explained e = fromTable(kAlarms, int(std::size(kAlarms)), code);
    if (!e.known) {
        e.meaning = QStringLiteral("Unknown alarm %1").arg(code);
        e.brief = QStringLiteral("the controller raised alarm %1").arg(code);
        e.cause = QStringLiteral("The controller stopped with an alarm code this app does not know. "
                                 "It may be a newer or different firmware.");
        e.fix = QStringLiteral("Reset the controller and Initialize. Look up alarm %1 for your "
                               "controller's firmware.").arg(code);
    }
    return e;
}

bool find(const QString &line, bool *isAlarm, int *code)
{
    static const QRegularExpression re(QStringLiteral("\\b(error|ALARM):(\\d+)"));
    const QRegularExpressionMatch m = re.match(line);
    if (!m.hasMatch())
        return false;
    if (isAlarm)
        *isAlarm = m.captured(1) == QLatin1String("ALARM");
    if (code)
        *code = m.captured(2).toInt();
    return true;
}

QString label(bool isAlarm, int code)
{
    const Explained e = isAlarm ? alarm(code) : error(code);
    return QStringLiteral("%1:%2 — %3")
        .arg(isAlarm ? QStringLiteral("ALARM") : QStringLiteral("error"))
        .arg(code)
        .arg(e.meaning);
}

QString details(bool isAlarm, int code)
{
    const Explained e = isAlarm ? alarm(code) : error(code);
    return QStringLiteral("Likely cause: %1\n\nWhat to do: %2").arg(e.cause, e.fix);
}

} // namespace grblcodes
} // namespace c2d
