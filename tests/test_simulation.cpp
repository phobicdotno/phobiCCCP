// Headless checks for the material-removal simulation (src/simulation.*) and
// the tool-geometry table (gcodeexport.h toolGeometry()). Plain asserts, no
// framework; exits 0 on success.

#include "../src/c2ddocument.h"
#include "../src/gcodeexport.h"
#include "../src/simulation.h"

#include <QElapsedTimer>
#include <QJsonObject>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using c2d::Op;
using c2d::ToolGeom;

static int g_checks = 0;

static void check(bool cond, const char *what)
{
    ++g_checks;
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

static bool approx(double a, double b, double eps)
{
    return std::fabs(a - b) <= eps;
}

static QHash<int, ToolGeom> tools()
{
    QHash<int, ToolGeom> t;
    ToolGeom flat;  flat.number = 201; flat.kind = ToolGeom::Flat; flat.diameter = 6.0;
    ToolGeom vee;   vee.number = 302;  vee.kind = ToolGeom::VBit; vee.diameter = 12.7; vee.angle = 90;
    ToolGeom ball;  ball.number = 102; ball.kind = ToolGeom::Ball; ball.diameter = 6.0; ball.cornerRadius = 3.0;
    t.insert(201, flat);
    t.insert(302, vee);
    t.insert(102, ball);
    return t;
}

static c2d::SimSettings stock(double w, double h, double t, double cell = 0.1)
{
    c2d::SimSettings s;
    s.stockW = w; s.stockH = h; s.stockT = t;
    s.minCell = cell;
    return s;
}

// Distance from (x, y) to the axis-aligned rectangle [x0,x1]×[y0,y1] (0 inside).
static double rectDist(double x, double y, double x0, double y0, double x1, double y1)
{
    const double dx = x < x0 ? x0 - x : (x > x1 ? x - x1 : 0);
    const double dy = y < y0 ? y0 - y : (y > y1 ? y - y1 : 0);
    return std::hypot(dx, dy);
}

int main()
{
    // --- flat 6 mm end mill pocket: raster over [13,37]×[13,27] at Z -4 ------
    {
        QVector<Op> ops;
        ops << Op::tool(201) << Op::rapid(13, 13, 5) << Op::feedTo(13, 13, -4, 300);
        bool fwd = true;
        for (double y = 13; y <= 27 + 1e-9; y += 1.0) {
            ops << Op::feedTo(fwd ? 13 : 37, y, -4, 1000);
            ops << Op::feedTo(fwd ? 37 : 13, y, -4, 1000);
            fwd = !fwd;
        }
        ops << Op::rapid(fwd ? 13 : 37, 27, 5);   // rapid at safe Z: never cuts
        ops << Op::feedTo(50, 35, 2, 1000);        // feed above the stock: never cuts

        const c2d::SimResult r = c2d::simulate(ops, tools(), stock(60, 40, 10));
        const c2d::HeightMap &m = r.map;
        check(!m.isNull() && m.width() == 600 && m.height() == 400, "pocket: 0.1 mm map 600×400");
        check(approx(m.cellSize(), 0.1, 1e-12), "pocket: cell size 0.1");
        int inside = 0, outside = 0;
        for (int iy = 0; iy < m.height(); ++iy) {
            for (int ix = 0; ix < m.width(); ++ix) {
                const double d = rectDist(m.cellCenterX(ix), m.cellCenterY(iy), 13, 13, 37, 27);
                if (std::fabs(d - 3.0) < 0.15)
                    continue;   // cells straddling the pocket wall
                const double z = m.at(ix, iy);
                if (d < 3.0) {
                    ++inside;
                    if (!approx(z, -4.0, 0.01)) {
                        std::fprintf(stderr, "pocket inside (%g,%g) z=%g\n",
                                     m.cellCenterX(ix), m.cellCenterY(iy), z);
                        check(false, "pocket: floor at -4 ± 0.01 everywhere inside");
                    }
                } else {
                    ++outside;
                    if (z != 0.0f) {
                        std::fprintf(stderr, "pocket outside (%g,%g) z=%g\n",
                                     m.cellCenterX(ix), m.cellCenterY(iy), z);
                        check(false, "pocket: stock untouched outside");
                    }
                }
            }
        }
        check(inside > 30000 && outside > 150000, "pocket: both regions sampled");
        check(approx(r.minZ, -4.0, 1e-6), "pocket: min Z -4");
        check(!r.throughCut, "pocket: not a through-cut");
        check(r.missingTools.isEmpty(), "pocket: tool 201 known");
        check(approx(m.sample(25, 20), -4.0, 0.01), "pocket: sample() inside");
        check(m.sample(5, 5) == 0.0, "pocket: sample() outside");
        check(std::isnan(m.sample(-1, 5)), "pocket: sample() off-map is NaN");
        std::printf("pocket: %d segments, %.1f ms\n", r.segments, r.elapsedMs);
    }

    // --- 90° V-bit line at Z -2: 4 mm wide at the surface -------------------
    {
        QVector<Op> ops;
        // y = 20.05 sits on a cell centre row, so the tip depth is sampled exactly.
        ops << Op::tool(302) << Op::rapid(10, 20.05, 5) << Op::feedTo(10, 20.05, -2, 300)
            << Op::feedTo(40, 20.05, -2, 1000);
        const c2d::SimResult r = c2d::simulate(ops, tools(), stock(60, 40, 10));
        const c2d::HeightMap &m = r.map;
        int ix, iy;
        check(m.cellOf(25.0, 20.05, &ix, &iy), "vbit: cell lookup");
        int cut = 0;
        for (int j = 0; j < m.height(); ++j)
            if (m.at(ix, j) < -0.005)
                ++cut;
        const double width = cut * m.cellSize();
        std::printf("vbit: surface width %.2f mm\n", width);
        check(approx(width, 4.0, 0.2), "vbit: 90° at -2 mm is ~4 mm wide at the surface");
        check(approx(m.at(ix, iy), -2.0, 0.01), "vbit: tip depth -2 on the line");
        check(approx(m.sample(25.0, 21.05), -1.0, 0.01), "vbit: -1 at 1 mm off the line");
        check(approx(m.sample(25.0, 19.05), -1.0, 0.01), "vbit: symmetric");
        check(m.sample(25.0, 23.0) == 0.0, "vbit: untouched at 3 mm");
        check(approx(m.sample(10.0, 20.05), -2.0, 0.01), "vbit: plunge point at -2");
        check(!r.throughCut, "vbit: no through-cut");
    }

    // --- ball 6 mm plunge at Z -2: spherical bottom ----------------------------
    {
        QVector<Op> ops;
        ops << Op::tool(102) << Op::rapid(30.05, 20.05, 5) << Op::feedTo(30.05, 20.05, -2, 300);
        const c2d::SimResult r = c2d::simulate(ops, tools(), stock(60, 40, 10));
        const c2d::HeightMap &m = r.map;
        check(approx(m.sample(30.05, 20.05), -2.0, 0.01), "ball: tip depth");
        // rho = 2.0 → z = -2 + 3 - sqrt(9 - 4) = -1.236
        check(approx(m.sample(32.05, 20.05), -2.0 + 3.0 - std::sqrt(9.0 - 4.0), 0.02), "ball: rho 2");
        // surface reached at rho = sqrt(9 - 1) = 2.83
        check(m.sample(33.05, 20.05) == 0.0, "ball: untouched at rho 3");
    }

    // --- full-circle G2 arc with the 6 mm end mill at Z -1 -----------------
    {
        QVector<Op> ops;
        ops << Op::tool(201) << Op::rapid(40, 20, 5) << Op::feedTo(40, 20, -1, 300)
            << Op::arcTo(40, 20, -1, -10, 0, true, 1000);   // centre (30,20), r 10
        const c2d::SimResult r = c2d::simulate(ops, tools(), stock(60, 40, 10));
        const c2d::HeightMap &m = r.map;
        check(approx(m.sample(40, 20), -1.0, 1e-6), "arc: start point cut");
        check(approx(m.sample(30, 30.05), -1.0, 1e-6), "arc: top of the circle cut");
        check(approx(m.sample(20.05, 20), -1.0, 1e-6), "arc: left of the circle cut");
        check(approx(m.sample(30, 9.95), -1.0, 1e-6), "arc: bottom of the circle cut");
        check(m.sample(30, 20) == 0.0, "arc: centre untouched");
        check(m.sample(30, 25.05) == 0.0, "arc: inside the ring untouched");
        check(r.segments >= 300, "arc: tessellated to ≤ 0.2 mm chords");
    }

    // --- through-cut: slot at the full stock thickness ---------------------
    {
        QVector<Op> ops;
        ops << Op::tool(201) << Op::rapid(10, 10, 5) << Op::feedTo(10, 10, -10, 300)
            << Op::feedTo(50, 10, -10, 1000);
        const c2d::SimResult r = c2d::simulate(ops, tools(), stock(60, 40, 10));
        check(r.throughCut, "through: flagged");
        check(approx(r.minZ, -10.0, 1e-6), "through: min Z at stock bottom");
        const QImage img = c2d::renderHeightMap(r.map, 10);
        check(img.width() == 600 && img.height() == 400, "render: one pixel per cell");
        // Image row 0 is the top of the stock (Y = 40); the slot at Y 10 → row 300.
        check(img.pixel(300, 400 - 1 - 100) == c2d::simColorThrough().rgb(), "render: through colour");
        check(img.pixel(300, 400 - 1 - 300) != c2d::simColorThrough().rgb(), "render: uncut elsewhere");
    }

    // --- unknown tool number falls back to a default and is reported --------
    {
        QVector<Op> ops;
        ops << Op::tool(999) << Op::rapid(10, 10, 5) << Op::feedTo(10, 10, -1, 300)
            << Op::feedTo(20, 10, -1, 1000);
        const c2d::SimResult r = c2d::simulate(ops, tools(), stock(60, 40, 10));
        check(r.missingTools.size() == 1 && r.missingTools.first() == 999, "missing tool reported");
        check(approx(r.map.sample(15, 10.05), -1.0, 1e-6), "missing tool still cuts (default)");
    }

    // --- cancellation ------------------------------------------------------
    {
        QVector<Op> ops;
        ops << Op::tool(201) << Op::rapid(10, 10, 5) << Op::feedTo(10, 10, -1, 300)
            << Op::feedTo(20, 10, -1, 1000);
        std::atomic<bool> cancel{true};
        const c2d::SimResult r = c2d::simulate(ops, tools(), stock(60, 40, 10), &cancel);
        check(r.cancelled, "cancel: flagged");
        check(!r.map.isNull() && r.minZ == 0.0, "cancel: nothing cut");
    }

    // --- toolGeometry(): built from the toolpaths' tool JSON ------------------
    {
        c2d::Document doc;
        auto add = [&](const char *type, const QJsonObject &tool, const QJsonObject &pocketTool) {
            QJsonObject j;
            j.insert("type", QLatin1String(type));
            j.insert("tool", tool);
            if (!pocketTool.isEmpty())
                j.insert("tool_pocket", pocketTool);
            c2d::Toolpath tp;
            tp.uuid = QStringLiteral("{tp-%1}").arg(doc.toolpaths().size());
            tp.type = QLatin1String(type);
            tp.json = j;
            doc.addToolpath(tp);
        };
        add("pocket_toolpath",
            QJsonObject{{"number", 201}, {"diameter", 6.35}, {"angle", 0}, {"type", 0}, {"corner_radius", 0}},
            {});
        add("advanced_vcarve_toolpath",
            QJsonObject{{"number", 302}, {"diameter", 12.7}, {"angle", 90}, {"type", 2}},
            QJsonObject{{"number", 102}, {"diameter", 3.175}, {"type", 1}, {"corner_radius", 1.5875}});
        add("contour",
            QJsonObject{{"number", 201}, {"diameter", 99}, {"type", 0}},   // duplicate: first wins
            {});
        add("contour",
            QJsonObject{{"number", 7}, {"diameter", 6}, {"angle", 60}},    // no type: angle → V-bit
            {});
        const QHash<int, ToolGeom> t = c2d::toolGeometry(doc);
        check(t.size() == 4, "toolGeometry: four tools");
        check(t.value(201).kind == ToolGeom::Flat && approx(t.value(201).diameter, 6.35, 1e-9),
              "toolGeometry: flat end mill, first definition wins");
        check(t.value(302).kind == ToolGeom::VBit && approx(t.value(302).angle, 90, 1e-9),
              "toolGeometry: V-bit");
        check(t.value(102).kind == ToolGeom::Ball && approx(t.value(102).cornerRadius, 1.5875, 1e-9),
              "toolGeometry: ball from tool_pocket");
        check(t.value(7).kind == ToolGeom::VBit, "toolGeometry: angle without type is a V-bit");
    }

    // --- throughput: 100k short cuts on a 2000×2000 map ---------------------
    {
        QVector<Op> ops;
        ops << Op::tool(201) << Op::rapid(100, 100, 5) << Op::feedTo(100, 100, -3, 300);
        double x = 100, y = 100, a = 0;
        unsigned s = 12345;
        for (int i = 0; i < 100000; ++i) {
            s = s * 1103515245u + 12345u;
            a += ((s >> 16) % 1000) / 1000.0 - 0.5;
            x += 0.5 * std::cos(a); y += 0.5 * std::sin(a);
            if (x < 10 || x > 190) { x = std::fmin(190.0, std::fmax(10.0, x)); a += 3.1416; }
            if (y < 10 || y > 190) { y = std::fmin(190.0, std::fmax(10.0, y)); a += 3.1416; }
            ops << Op::feedTo(x, y, -3, 1000);
        }
        c2d::SimSettings st = stock(200, 200, 10);
        const c2d::SimResult r = c2d::simulate(ops, tools(), st);
        std::printf("throughput: %d segments on %dx%d in %.0f ms\n",
                    r.segments, r.map.width(), r.map.height(), r.elapsedMs);
        check(r.map.width() == 2000 && r.map.height() == 2000, "throughput: 2000×2000");
        check(r.elapsedMs < 30000, "throughput: 100k segments well under 30 s");
    }

    std::printf("test_simulation: %d checks OK\n", g_checks);
    return 0;
}
