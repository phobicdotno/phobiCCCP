// Chipload helper: feed / (rpm x flutes), the range from the library's own
// feeds (±40 %) or the built-in table, the verdict and the suggested feed.
#include "../src/chipload.h"
#include "../src/toollibrary.h"
#include <QByteArray>
#include <QMap>
#include <cmath>
#include <cstdio>
using namespace c2d;
static int fails = 0;
static void check(bool ok, const char *what) { std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++fails; }
static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

int main()
{
    check(near(chiploadOf(1800, 18000, 2), 0.05), "1800 mm/min at 18000 rpm, 2 flutes = 0.05");
    check(chiploadOf(1800, 0, 2) == 0 && chiploadOf(0, 18000, 2) == 0 && chiploadOf(1800, 18000, 0) == 0,
          "no chipload without feed, rpm or flutes");

    const ChiploadRange lr = chiploadRangeFrom(1800, 18000, 2);
    check(lr.valid() && lr.fromLibrary && near(lr.mid, 0.05) && near(lr.low, 0.03) && near(lr.high, 0.07),
          "library range is ±40 % around its chipload");

    check(classifyChipload(0.05, lr) == ChiploadVerdict::Ok, "middle is OK");
    check(classifyChipload(0.03, lr) == ChiploadVerdict::Ok, "low edge is OK");
    check(classifyChipload(0.02, lr) == ChiploadVerdict::TooLow, "below range is too low");
    check(classifyChipload(0.08, lr) == ChiploadVerdict::TooHigh, "above range is too high");
    check(classifyChipload(0, lr) == ChiploadVerdict::Unknown, "no chipload is unknown");
    check(classifyChipload(0.05, ChiploadRange()) == ChiploadVerdict::Unknown, "no range is unknown");

    check(chiploadText(0.05, lr) == QLatin1String("Chipload 0.050 mm/tooth: OK"), "OK text");
    check(chiploadText(0.02, lr).endsWith(QLatin1String("too low (rubbing, heat)")), "too low text");
    check(chiploadText(0.08, lr).endsWith(QLatin1String("too high (bit may break)")), "too high text");
    check(chiploadText(0, lr).isEmpty(), "no text when unknown");

    check(near(suggestedFeed(lr, 18000, 2), 1800), "suggested feed is the middle at this rpm");
    check(near(suggestedFeed(lr, 12000, 2), 1200), "suggested feed follows the rpm");
    check(near(suggestedFeed(chiploadRangeFrom(1000, 18000, 3), 18000, 3), 1000), "rounded to 10");
    check(suggestedFeed(lr, 0, 2) == 0, "no suggestion without rpm");

    // Built-in table: by diameter, aluminium lower than wood, plastic above wood.
    const ChiploadRange w3 = chiploadTable(3.175, "mdf");
    check(w3.valid() && !w3.fromLibrary && near(w3.mid, 0.035), "1/8\" in MDF: 0.035");
    check(chiploadTable(6.35, "softwood").mid > w3.mid, "bigger bit, bigger chip");
    check(chiploadTable(1.0, "hardwood").mid < w3.mid, "smaller bit, smaller chip");
    check(chiploadTable(3.175, "aluminium").mid < w3.mid, "aluminium below wood");
    check(chiploadTable(3.175, "acrylic").mid >= w3.mid, "plastic at or above wood");
    check(!chiploadTable(0, "mdf").valid(), "no table without a diameter");
    check(defaultRpmFor("aluminium") == 10000 && defaultRpmFor("mdf") == 18000, "default rpm");

    // From a library: its entry wins; another size or material falls back to the table.
    const QByteArray mats = R"({"version":1,"materials":[{"id":"mdf","name":"MDF","cc_names":["MDF"]},
        {"id":"aluminium","name":"Aluminium","cc_names":["Aluminum"]}]})";
    const QByteArray tool = R"({"id":"00abcd","number":102,"name":"#102","kind":"endmill","diameter":3.175,
        "flutes":2,"feeds":{"mdf":{"rpm":18000,"feed":2160,"plunge":254}}})";
    ToolLibrary lib;
    QMap<QString, QByteArray> files{{"materials.json", mats}, {"endmill/00abcd.json", tool}};
    const bool loaded = lib.loadFiles(files);
    check(loaded && lib.byNumber(102), "test library loads");
    const ChiploadRange a = chiploadRangeFor(&lib, 102, 3.175, 2, "mdf");
    check(a.fromLibrary && near(a.mid, 0.06), "library entry gives 2160/(18000*2) = 0.06");
    check(!chiploadRangeFor(&lib, 102, 3.175, 2, "aluminium").fromLibrary, "no entry for material: table");
    check(!chiploadRangeFor(&lib, 102, 6.35, 2, "mdf").fromLibrary, "same number, other size: table");
    check(!chiploadRangeFor(nullptr, 102, 3.175, 2, "mdf").fromLibrary, "no library: table");

    std::printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
