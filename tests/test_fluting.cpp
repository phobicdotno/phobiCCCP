// Fluting: a groove down an open vector that ramps from the top to full
// depth and back, in passes that each take a share of the whole profile.
// Plain asserts, no test framework; exits 0 on success.

#include "../src/c2ddocument.h"
#include "../src/element.h"
#include "../src/gcodeexport.h"
#include "../src/post_grbl.h"
#include "../src/sketch.h"
#include "../src/toolpathfactory.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace c2d;

static int g_checks = 0;

static void check(bool cond, const char *what)
{
    ++g_checks;
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

static GcodeResult exportFlute(const Element &e, const QJsonObject &extra)
{
    Document doc;
    doc.addElement(e);
    QJsonObject j{{"type", QStringLiteral("fluting_toolpath")}, {"name", QStringLiteral("f")},
                  {"enabled", true}, {"uuid", QStringLiteral("{tp-1}")},
                  {"start_depth", QStringLiteral("0.000")}, {"end_depth", QStringLiteral("-3.000")},
                  {"ramp_ends", QStringLiteral("both")}, {"ramp_length", 25.0},
                  {"ramp_unit", QStringLiteral("mm")}, {"ramp_shape", QStringLiteral("linear")},
                  {"passes", 1},
                  {"speeds", QJsonObject{{"feedrate", 1000}, {"plungerate", 300}, {"rpm", 10000}}},
                  {"tool", QJsonObject{{"diameter", 12.7}, {"angle", 90}, {"type", 2}, {"number", 301}}},
                  {"elements", QJsonArray{QJsonObject{{"uuid", e.id}}}}};
    for (auto it = extra.begin(); it != extra.end(); ++it)
        j.insert(it.key(), it.value());
    Toolpath tp;
    tp.uuid = j.value("uuid").toString();
    tp.type = QStringLiteral("fluting_toolpath");
    tp.json = j;
    doc.addToolpath(tp);
    return exportGcode(doc);
}

// Feed and arc moves below the safe height, split into passes at each rapid.
static QVector<QVector<Op>> passesOf(const GcodeResult &g)
{
    QVector<QVector<Op>> out;
    bool inCut = false;
    for (const Op &o : g.ops) {
        if (o.kind == Op::Rapid) {
            inCut = false;
            continue;
        }
        if (o.kind != Op::Feed && o.kind != Op::Arc)
            continue;
        if (o.z > 1.0)   // retract feeds, if any
            continue;
        if (!inCut) {
            out.append(QVector<Op>());
            inCut = true;
        }
        out.last().append(o);
    }
    return out;
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    const QJsonObject layer = Document().defaultLayer();
    const Element line = Element::makePath({{0, 0}, {100, 0}}, false, layer);

    // 100 mm line, 3 deep, 25 mm straight ramps at both ends.
    {
        const GcodeResult g = exportFlute(line, {});
        check(g.done.size() == 1 && g.skipped.isEmpty(), "a line flute exports");
        const auto passes = passesOf(g);
        check(passes.size() == 1, "one pass");
        auto want = [](double x) { return x < 25 ? -3 * x / 25 : x > 75 ? -3 * (100 - x) / 25 : -3.0; };
        bool onProfile = true, sawStart = false, sawEnd = false, sawMid = false;
        double minZ = 0;
        for (const Op &o : passes.first()) {
            onProfile = onProfile && std::abs(o.z - want(o.x)) < 1e-4 && std::abs(o.y) < 1e-6;
            sawStart = sawStart || (std::abs(o.x) < 1e-6 && std::abs(o.z) < 1e-6);
            sawEnd = sawEnd || (std::abs(o.x - 100) < 1e-6 && std::abs(o.z) < 1e-6);
            sawMid = sawMid || (std::abs(o.x - 75) < 1e-6 && std::abs(o.z + 3) < 1e-6);
            minZ = std::min(minZ, o.z);
        }
        check(onProfile, "every move ends on the ramp-level-ramp profile (straight lines between)");
        check(sawStart && sawEnd, "Z is 0 at both ends");
        check(sawMid && std::abs(minZ + 3) < 1e-6, "full 3 mm depth from 25 to 75 mm, never deeper");
    }

    // Two passes: the first never goes below half the depth.
    {
        const GcodeResult g = exportFlute(line, {{"passes", 2}});
        const auto passes = passesOf(g);
        check(passes.size() == 2, "two passes");
        double min1 = 0, min2 = 0;
        for (const Op &o : passes.at(0))
            min1 = std::min(min1, o.z);
        for (const Op &o : passes.at(1))
            min2 = std::min(min2, o.z);
        check(min1 >= -1.5 - 1e-6 && std::abs(min1 + 1.5) < 1e-6, "pass 1 reaches half depth, no more");
        check(std::abs(min2 + 3) < 1e-6, "pass 2 reaches full depth");
    }

    // Percent ramps, start only, smooth.
    {
        const GcodeResult g = exportFlute(line, {{"ramp_unit", QStringLiteral("percent")},
                                                 {"ramp_length", 10.0},
                                                 {"ramp_ends", QStringLiteral("start")},
                                                 {"ramp_shape", QStringLiteral("smooth")}});
        const auto passes = passesOf(g);
        check(passes.size() == 1, "smooth flute exports");
        bool onProfile = true;
        int inRamp = 0;
        for (const Op &o : passes.first()) {
            const double u = std::min(1.0, o.x / 10.0);
            const double z = -3 * (0.5 - 0.5 * std::cos(M_PI * u));
            onProfile = onProfile && std::abs(o.z - z) < 1e-4;
            if (o.x > 0.1 && o.x < 9.9)
                ++inRamp;
        }
        check(onProfile, "smooth ramp follows half a cosine over 10 % of the length");
        check(inRamp >= 10, "a smooth ramp is sampled finely");
        const Op &last = passes.first().last();
        check(std::abs(last.x - 100) < 1e-6 && std::abs(last.z + 3) < 1e-6,
              "start-only: the end stays at full depth");
    }

    // An arc: arcs only where Z is level.
    {
        const Element arc = Element::makeBezierPath(sketch::arc3({0, 0}, {100, 0}, {50, 30}), layer);
        const GcodeResult g = exportFlute(arc, {});
        check(g.done.size() == 1, "an arc flute exports");
        bool arcsLevel = true;
        int arcs = 0;
        for (const Op &o : g.ops)
            if (o.kind == Op::Arc) {
                ++arcs;
                arcsLevel = arcsLevel && std::abs(o.z + 3) < 1e-6;
            }
        check(arcs > 0 && arcsLevel, "the level middle is fitted with arcs, every G2/G3 at the flute depth");
        double minZ = 0;
        for (const Op &o : g.ops)
            if (o.kind == Op::Feed || o.kind == Op::Arc)
                minZ = std::min(minZ, o.z);
        check(std::abs(minZ + 3) < 1e-6, "the arc flute reaches 3 mm");
    }

    // Closed vectors are refused with a note.
    {
        const Element box = Element::makeRectangle({50, 50}, 20, 10, layer);
        const GcodeResult g = exportFlute(box, {});
        check(g.done.isEmpty() && g.skipped.size() == 1
                  && g.skipped.first().contains(QLatin1String("open vectors")),
              "a closed vector alone is refused");
    }

    // Deeper than the 12.7 mm 90-degree V-bit's 6.35 mm cone: refused.
    {
        const GcodeResult g = exportFlute(line, {{"end_depth", QStringLiteral("-10.000")}});
        check(g.done.isEmpty() && g.skipped.size() == 1
                  && g.skipped.first().contains(QLatin1String("cone")),
              "a flute deeper than the V-bit's cone is refused");
        const GcodeResult ok = exportFlute(line, {{"end_depth", QStringLiteral("-6.000")}});
        check(ok.done.size() == 1 && ok.skipped.isEmpty(), "a flute within the cone exports");
    }

    // The factory: label, V-bit, defaults.
    {
        Document doc;
        const Element e = Element::makePath({{0, 0}, {50, 0}}, false, doc.defaultLayer());
        doc.addElement(e);
        const Toolpath t = makeToolpath(doc, QStringLiteral("fluting_toolpath"), {e.id});
        check(toolpathLabel(QStringLiteral("fluting_toolpath")) == QLatin1String("Fluting"), "label");
        check(t.json.value("ramp_ends").toString() == QLatin1String("both")
                  && t.json.value("ramp_length").toDouble() == 25.0
                  && t.json.value("ramp_unit").toString() == QLatin1String("percent")
                  && t.json.value("passes").toInt() == 1,
              "defaults: both ends, 25 %, one pass");
        check(t.json.value("tool").toObject().value("angle").toDouble() > 0, "a V-bit by default");
        doc.addToolpath(t);
        const GcodeResult g = exportGcode(doc);
        check(g.done.size() == 1 && g.skipped.isEmpty(), "the default fluting toolpath exports");
    }

    std::printf("fluting: %d checks passed\n", g_checks);
    return 0;
}
