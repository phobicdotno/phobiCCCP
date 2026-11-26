#pragma once
#include <QStringList>

namespace c2d {

// Centre-finding and touch-plate probe cycles that need no BitZero: the
// G-code each stage sends and the sums over the contacts it reports. Pure, so
// the numbers can be checked without a machine. Every touch is made twice,
// fast then slow; only the slow contacts count.
struct ProbeFeeds {
    double fastFeed = 300.0;   // first touch, mm/min
    double slowFeed = 50.0;    // measuring touch
    double retract = 2.0;      // back-off between the two touches
};

// Hole, in two steps per axis. Nothing in a hole moves fast except over
// ground the pin has already travelled.
// Search: from inside (machine coordinate `startAbs` on this axis), fast out
// to the + wall, straight back to the start, fast out to the - wall, back to
// the start. `search` is how far each side may travel. Contacts: [fast+,
// fast-].
QStringList holeSearchLines(char axis, double search, const ProbeFeeds &f, double startAbs);
// Touch: the slow touches on the walls the search found at `plus` and
// `minus`. Each starts the back-off short of its wall, never more than half
// the chord between the two (all of it travelled by the search), and ends
// back at the start. Contacts: [slow+, slow-].
QStringList holeTouchLines(char axis, double plus, double minus, double startAbs, const ProbeFeeds &f);

// Boss: from just over its top, out past the + side by diameter/2 +
// clearance, down `depth`, probe back toward the boss, up, back over the
// start (machine coordinate `startAbs` on this axis); then the same on the
// - side. Ends where it began. Contacts as above.
QStringList bossProbeLines(char axis, double diameter, double clearance, double depth,
                           const ProbeFeeds &f, double startAbs);

// Touch plate: straight down at most `travel`, ends 5 mm over the plate.
// Contacts: [fast, slow].
QStringList plateProbeLines(double travel, const ProbeFeeds &f);

// From the slow contacts on both sides of each axis: the centre, and the
// size of the hole (contacts + tip) or the boss (contacts - tip). The
// contact is the tip's centre, so the tip diameter enters once per axis.
struct ProbeCentre {
    double x = 0, y = 0;
    double diameterX = 0, diameterY = 0;
    double diameter() const { return (diameterX + diameterY) / 2.0; }
};
ProbeCentre probeCentre(double xPlus, double xMinus, double yPlus, double yMinus,
                        double tipDiameter, bool boss);

} // namespace c2d
