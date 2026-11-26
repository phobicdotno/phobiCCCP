// Hole centre, boss centre and touch-plate cycles (probecycles.h): the
// G-code each stage sends and the centre and size from the contacts.

#include "probecycles.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace c2d;

static int g_checks = 0, g_fails = 0;

static void check(bool cond, const char *what)
{
    ++g_checks;
    if (!cond) {
        ++g_fails;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

static bool near(double a, double b) { return std::abs(a - b) < 1e-9; }

int main()
{
    // Hole: walls at x -10/+10, y -6/+14 with a 3.175 mm pin.
    const double tip = 3.175;
    const ProbeCentre h = probeCentre(10, -10, 14, -6, tip, false);
    check(near(h.x, 0) && near(h.y, 4), "hole centre (0, 4)");
    check(near(h.diameterX, 20 + tip) && near(h.diameterY, 20 + tip), "hole size = span + tip");
    check(near(h.diameter(), 20 + tip), "hole diameter averaged");
    // Boss: the same contacts from outside, the tip on the other side.
    const ProbeCentre b = probeCentre(10, -10, 14, -6, tip, true);
    check(near(b.x, 0) && near(b.y, 4), "boss centre (0, 4)");
    check(near(b.diameter(), 20 - tip), "boss size = span - tip");

    ProbeFeeds f;
    f.fastFeed = 300;
    f.slowFeed = 50;
    f.retract = 2;
    const QStringList hole = holeSearchLines('X', 15, f, -100);
    check(hole.at(1) == "G38.3 X15.000 F300", "hole: fast + search");
    check(hole.at(3) == "G53 G0 X-100.000", "hole: back to the start, not a set back-off");
    check(hole.at(5) == "G38.3 X-15.000 F300", "hole: fast - search");
    check(hole.last() == "G53 G0 X-100.000" && hole.filter("G38.2").isEmpty(),
          "hole: search ends at the start, no slow touch yet");
    // Walls 10 mm apart: the 2 mm back-off fits.
    const QStringList touch = holeTouchLines('X', -95, -105, -100, f);
    check(touch.at(1) == "G53 G0 X-97.000" && touch.at(3) == "G38.2 X4.000 F50", "hole: slow + touch");
    check(touch.at(6) == "G53 G0 X-103.000" && touch.at(8) == "G38.2 X-4.000 F50", "hole: slow - touch");
    check(touch.last() == "G53 G0 X-100.000" && touch.filter("G38.2").count() == 2,
          "hole: two slow touches, back at the start");
    // Pin started 1.25 mm off the middle of a 6 mm hole, 3 mm pin: the X
    // chord the pin travels is 1.66 mm. The back-off is half that, not 2 mm
    // into the far wall.
    const QStringList tight = holeTouchLines('X', 0.83, -0.83, 0, f);
    check(tight.at(1) == "G53 G0 X0.000" && tight.at(6) == "G53 G0 X0.000",
          "hole: back-off never past the middle of the chord travelled");

    const QStringList boss = bossProbeLines('Y', 20, 5, 8, f, -100);
    check(boss.at(1) == "G0 Y15.000", "boss: out past + side (r + clearance)");
    check(boss.at(2) == "G0 Z-8.000", "boss: down beside it");
    check(boss.at(3) == "G38.3 Y-12.000 F300", "boss: fast toward it");
    check(boss.at(5) == "G38.2 Y-4.000 F50", "boss: slow toward it");
    check(boss.at(7) == "G0 Z8.000", "boss: back up before crossing");
    check(boss.at(9) == "G53 G0 Y-100.000", "boss: back over the start, not the contact");
    check(boss.at(11) == "G0 Y-15.000", "boss: out past - side");
    check(boss.at(13) == "G38.3 Y12.000 F300", "boss: fast toward it from -");
    check(boss.filter("G53 G0 Y-100.000").count() == 2, "boss: ends over the start");
    check(boss.last() == "G90", "boss: absolute at the end");
    // Every move adds up to nothing: the cycle ends where it began, at the
    // start height (contacts aside, which stop short of the full move).
    double z = 0;
    for (const QString &l : boss)
        if (l.startsWith("G0 Z"))
            z += l.mid(4).toDouble();
    check(near(z, 0), "boss: ends at the start height");
    // Never searches past the middle of a small boss.
    const QStringList small = bossProbeLines('X', 2, 10, 5, f, 0);
    check(small.at(3) == "G38.3 X-11.000 F300", "boss: search no farther than the centre");

    const QStringList plate = plateProbeLines(25, f);
    check(plate.at(1) == "G38.3 Z-25.000 F300" && plate.at(3) == "G38.2 Z-4.000 F50",
          "plate: fast then slow down");

    std::printf("%d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? EXIT_FAILURE : EXIT_SUCCESS;
}
