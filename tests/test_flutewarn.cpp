// fluteLengthWarning(): a toolpath deeper than its bit's flutes gets a
// sentence; a short one, an unknown length and V-bit work get none.
#include "../src/gcodeexport.h"
#include <QJsonObject>
#include <cstdio>
using namespace c2d;
static int fails = 0;
static void check(bool ok, const char *what) { std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++fails; }

static QJsonObject tool(double length, int type = 0, double angle = 0)
{
    QJsonObject t{{"number", 2}, {"diameter", 3.175}, {"type", type}, {"angle", angle}};
    if (length >= 0)
        t.insert("length", length);
    return t;
}

int main()
{
    // Pocket: end depth against the flutes, either sign convention, strings too.
    QJsonObject p{{"end_depth", "-15"}, {"tool", tool(12.7)}};
    const QString w = fluteLengthWarning("pocket_toolpath", p);
    check(!w.isEmpty(), "15 mm pocket with 12.7 mm flutes warns");
    check(w.contains("15.0") && w.contains("12.7") && w.contains("T2"), "warning names depth, flutes and tool");
    p["end_depth"] = 12.0;
    check(fluteLengthWarning("pocket_toolpath", p).isEmpty(), "12 mm pocket fits 12.7 mm flutes");
    p["end_depth"] = 12.7;
    check(fluteLengthWarning("pocket_toolpath", p).isEmpty(), "exactly the flute length is fine");

    // Cutout: cut depth plus break-through.
    QJsonObject c{{"cut_depth", "12"}, {"break_through", "1"}, {"tool", tool(12.7)}};
    check(!fluteLengthWarning("cutout", c).isEmpty(), "cutout's break-through counts");
    c["break_through"] = 0.5;
    check(fluteLengthWarning("cutout", c).isEmpty(), "cutout within the flutes");

    // 3D: start plus max depth.
    QJsonObject r{{"start_depth", 5.0}, {"max_depth", 10.0}, {"tool", tool(12.7, 1)}};
    check(!fluteLengthWarning("3d_rough_toolpath", r).isEmpty(), "3D start + max depth counts (ball)");
    r["start_depth"] = 0.0;
    check(fluteLengthWarning("3d_rough_toolpath", r).isEmpty(), "3D within the flutes");

    // Unknown length, V-bits, V-carve and an unreadable depth: no warning.
    QJsonObject u{{"end_depth", 30.0}, {"tool", tool(-1)}};
    check(fluteLengthWarning("contour", u).isEmpty(), "missing length: no warning");
    u["tool"] = tool(0);
    check(fluteLengthWarning("contour", u).isEmpty(), "zero length: no warning");
    QJsonObject v{{"end_depth", 30.0}, {"tool", tool(5, 2, 60)}};
    check(fluteLengthWarning("engrave_toolpath", v).isEmpty(), "V-bit: cone has its own check");
    QJsonObject vc{{"end_depth", 30.0}, {"tool", tool(5)}};
    check(fluteLengthWarning("advanced_vcarve_toolpath", vc).isEmpty(), "V-carve skipped");
    QJsonObject bad{{"end_depth", "nan"}, {"tool", tool(5)}};
    check(fluteLengthWarning("pocket_toolpath", bad).isEmpty(), "unreadable depth: no warning (export refuses it)");

    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
