// Headless checks for the 3D toolpaths (src/cam3d.*): a synthetic
// hemisphere relief is roughed and finished through exportGcode(), and the
// material-removal simulation verifies the result against the model — the
// cuts reach the model (+ stock to leave) and never go below it. Plain
// asserts, no framework; exits 0 on success.

#include "../src/c2ddocument.h"
#include "../src/element.h"
#include "../src/cam3d.h"
#include "../src/gcodeexport.h"
#include "../src/heightmodel.h"
#include "../src/simulation.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using c2d::Op;

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

// Hemisphere of radius R centred in a size×size floor, cell-centre sampled.
// Floor = baseZ = -R, dome peak = 0 (stock top), cells off the dome NoModel.
static c2d::HeightModel hemisphere(double R = 20, double cell = 0.5, double size = 60)
{
    c2d::HeightModel m;
    m.cell = cell;
    m.baseZ = -R;
    m.resize(int(size / cell), int(size / cell));
    for (int r = 0; r < m.rows; ++r)
        for (int c = 0; c < m.cols; ++c) {
            const double x = (c + 0.5) * cell - size / 2, y = (r + 0.5) * cell - size / 2;
            const double rho2 = x * x + y * y;
            if (rho2 <= R * R)
                m.ref(c, r) = float(-R + std::sqrt(R * R - rho2));
        }
    return m;
}

static QJsonObject toolpath(const char *type, const QJsonObject &tool, const QJsonObject &extra,
                            const char *name = "t3d")
{
    QJsonObject j;
    j.insert("type", QLatin1String(type));
    j.insert("name", QLatin1String(name));
    j.insert("enabled", true);
    j.insert("uuid", QStringLiteral("{%1}").arg(QLatin1String(name)));
    j.insert("elements", QJsonArray());
    j.insert("tool", tool);
    j.insert("speeds", QJsonObject{{"feedrate", 1000}, {"plungerate", 300}, {"rpm", 10000}});
    for (const QString &k : extra.keys())
        j.insert(k, extra.value(k));
    return j;
}

static void addToolpath(c2d::Document &doc, const QJsonObject &j)
{
    c2d::Toolpath tp;
    tp.uuid = j.value("uuid").toString();
    tp.type = j.value("type").toString();
    tp.json = j;
    doc.addToolpath(tp);
}

static const QJsonObject kFlat{{"number", 201}, {"diameter", 6.35}, {"type", 0}};
static const QJsonObject kBall{{"number", 202}, {"diameter", 3.0}, {"type", 1}, {"corner_radius", 1.5}};

static c2d::SimSettings stock(double w, double h, double t)
{
    c2d::SimSettings s;
    s.stockW = w; s.stockH = h; s.stockT = t;
    s.minCell = 0.1;
    s.maxDz = 0.005;   // sloped pieces stamped at their low end: keep that error tiny
    return s;
}

// Lowest model height within `delta` of (x, y): a lateral tolerance so a
// vertical wall does not turn a micron of XY error into millimetres of Z.
static double modelMin(const c2d::HeightModel &m, double x, double y, double delta)
{
    double z = m.sample(x, y);
    for (int k = 0; k < 8; ++k) {
        const double a = k * M_PI / 4;
        z = std::fmin(z, m.sample(x + delta * std::cos(a), y + delta * std::sin(a)));
    }
    return z;
}

static QSet<qint64> feedLevels(const QVector<Op> &ops)
{
    QSet<qint64> levels;
    for (const Op &o : ops)
        if (o.kind == Op::Feed && o.z < -1e-6)
            levels.insert(qRound64(o.z * 1e4));
    return levels;
}

// Square terraces (a stepped pyramid) centred in a size×size grid: the
// floor at -9 out to the edge, a tier at -6.4 within ±20 mm, -3.3 within
// ±12 mm and the top at 0 within ±5 mm. The tier heights are deliberately
// not multiples of any stepdown used below.
static c2d::HeightModel terraces(double cell = 0.5, double size = 60)
{
    c2d::HeightModel m;
    m.cell = cell;
    m.baseZ = -9;
    m.resize(int(size / cell), int(size / cell));
    for (int r = 0; r < m.rows; ++r)
        for (int c = 0; c < m.cols; ++c) {
            const double x = (c + 0.5) * cell - size / 2, y = (r + 0.5) * cell - size / 2;
            const double d = std::fmax(std::fabs(x), std::fabs(y));
            m.ref(c, r) = float(d <= 5 ? 0.0 : d <= 12 ? -3.3 : d <= 20 ? -6.4 : -9.0);
        }
    return m;
}

int main()
{
    const c2d::HeightModel model = hemisphere();
    const c2d::HeightModel *current = &model;
    c2d::setHeightModelProvider([&](const c2d::Document &) { return current; });

    // --- rough: 6.35 mm end mill, stepdown 3, 0.5 mm stock to leave ----------
    {
        c2d::Document doc;
        addToolpath(doc, toolpath("3d_rough_toolpath", kFlat,
                                  {{"stepdown", 3.0}, {"stepover", 2.5}, {"stock_to_leave", 0.5}}));
        const c2d::GcodeResult g = c2d::exportGcode(doc);
        check(g.done.size() == 1 && g.skipped.isEmpty(), "rough: exported");
        const QSet<qint64> levels = feedLevels(g.ops);
        std::printf("rough: %d ops, %d levels\n", int(g.ops.size()), int(levels.size()));
        check(levels.size() == 7, "rough: ceil((20 - 0.5) / 3) = 7 levels");
        check(levels.contains(qRound64(-19.5 * 1e4)), "rough: last level at floor + stock to leave");
        for (const Op &o : g.ops)
            if (o.kind == Op::Feed || o.kind == Op::Arc)
                check(o.z >= -19.5 - 1e-6, "rough: no op below floor + stock to leave");

        const c2d::SimResult sim = c2d::simulate(g.ops, c2d::toolGeometry(doc), stock(60, 60, 25));
        const c2d::HeightMap &hm = sim.map;
        // The map stores one depth per cell while the model is sampled at the
        // cell centre, so on a near-vertical wall the model legitimately drops
        // by slope x half a cell inside the very cell being checked: compare
        // against the model's minimum over the cell, not at its centre.
        const double halfCell = 0.5 * M_SQRT2 * hm.cellSize();
        check(!hm.isNull() && sim.missingTools.isEmpty(), "rough: simulated");
        double worst = 1e9;
        for (int iy = 0; iy < hm.height(); ++iy)
            for (int ix = 0; ix < hm.width(); ++ix) {
                const double x = hm.cellCenterX(ix), y = hm.cellCenterY(iy);
                // Target surface = model + leave, but never above the stock top.
                const double margin = hm.at(ix, iy) - std::fmin(0.0, modelMin(model, x, y, halfCell) + 0.5);
                if (margin < -0.05 && qgetenv("TEST_DUMP") == "1")
                    std::fprintf(stderr, "rough gouge at (%.2f, %.2f): sim %.4f model %.4f\n",
                                 x, y, hm.at(ix, iy), model.sample(x, y));
                worst = std::fmin(worst, margin);
            }
        std::printf("rough: closest approach to model + leave = %.4f mm\n", worst);
        check(worst >= -0.01, "rough: never below model + stock to leave (0.01 mm)");
        check(approx(hm.sample(5, 5), -19.5, 0.01), "rough: floor cleared in the corner");
        check(approx(hm.sample(30, 4), -19.5, 0.01), "rough: floor cleared beside the dome");
        // Level -6 covers rho >= 14.76 (surface + 0.5 <= -6); the tool centre
        // stays 3.675 mm (radius + leave) further out, so its edge reaches
        // rho 15.26 and level -9 (rho >= 17.5) does not reach rho 16 yet.
        std::printf("rough: dome at rho 16 = %.3f, rho 15 = %.3f\n", hm.sample(46, 30), hm.sample(45, 30));
        check(approx(hm.sample(46, 30), -6.0, 0.02), "rough: dome stepped at level -6 at rho 16");
        check(approx(hm.sample(45, 30), -3.0, 0.02), "rough: dome stepped at level -3 at rho 15");
        check(hm.sample(30, 30) == 0.0, "rough: dome peak untouched");
        check(!sim.throughCut, "rough: no through-cut");
    }

    // --- rough level count without stock to leave ---------------------------
    {
        c2d::Document doc;
        addToolpath(doc, toolpath("3d_rough_toolpath", kFlat,
                                  {{"stepdown", 2.5}, {"stepover", 2.5}, {"stock_to_leave", 0.0}}));
        const c2d::GcodeResult g = c2d::exportGcode(doc);
        check(g.done.size() == 1 && feedLevels(g.ops).size() == 8, "rough: ceil(20 / 2.5) = 8 levels");
        // max_depth stops the levels early.
        c2d::Document d2;
        addToolpath(d2, toolpath("3d_rough_toolpath", kFlat,
                                 {{"stepdown", 2.5}, {"stepover", 2.5}, {"max_depth", QStringLiteral("7.000")}}));
        const c2d::GcodeResult g2 = c2d::exportGcode(d2);
        const QSet<qint64> lv = feedLevels(g2.ops);
        check(lv.size() == 3 && lv.contains(qRound64(-7.0 * 1e4)), "rough: max_depth 7 → -2.5, -5, -7");
    }

    // --- finish: 3 mm ball, 0.5 mm stepover, zigzag along X ---------------------
    double lenAlongX = 0;
    {
        c2d::Document doc;
        addToolpath(doc, toolpath("3d_finish_toolpath", kBall,
                                  {{"stepover", 0.5}, {"raster_angle", 0.0},
                                   {"direction", QStringLiteral("zigzag")}}));
        const c2d::GcodeResult g = c2d::exportGcode(doc);
        check(g.done.size() == 1 && g.skipped.isEmpty(), "finish: exported");
        const c2d::JobStats st = c2d::computeStats(g.ops);
        std::printf("finish: %d ops, cut %.0f mm, rapid %.0f mm\n", int(g.ops.size()), st.cutLen, st.rapidLen);
        check(st.cutLen > 60.0 * 121 * 0.9, "finish: 121 passes over 60 mm");
        // Zigzag: pass ends are linked on the surface, so rapids stay short.
        check(st.rapidLen < st.cutLen * 0.05, "finish: zigzag links, few rapids");
        for (const Op &o : g.ops)
            if (o.kind == Op::Feed)
                check(o.z <= 1e-9 && o.z >= -20.0 - 1e-6, "finish: Z within the model range");
        double along = 0, across = 0;
        double px = 0, py = 0;
        for (const Op &o : g.ops) {
            if (o.kind == Op::Feed) {
                along += std::fabs(o.x - px);
                across += std::fabs(o.y - py);
            }
            px = o.x; py = o.y;
        }
        lenAlongX = along;
        check(along > 20 * across, "finish: raster 0 runs along X");

        const c2d::SimResult sim = c2d::simulate(g.ops, c2d::toolGeometry(doc), stock(60, 60, 25));
        const c2d::HeightMap &hm = sim.map;
        // The map stores one depth per cell while the model is sampled at the
        // cell centre, so on a near-vertical wall the model legitimately drops
        // by slope x half a cell inside the very cell being checked: compare
        // against the model's minimum over the cell, not at its centre.
        const double halfCell = 0.5 * M_SQRT2 * hm.cellSize();
        check(!hm.isNull() && sim.missingTools.isEmpty(), "finish: simulated");
        double worst = 1e9, maxDev = 0, maxFloor = 0;
        double wx = 0, wy = 0, wz = 0, wm = 0;
        int interior = 0, floor = 0;
        for (int iy = 0; iy < hm.height(); ++iy)
            for (int ix = 0; ix < hm.width(); ++ix) {
                const double x = hm.cellCenterX(ix), y = hm.cellCenterY(iy);
                const double z = hm.at(ix, iy);
                {
                    const double m = z - modelMin(model, x, y, halfCell);
                    if (m < worst) {
                        worst = m;
                        wx = x; wy = y; wz = z; wm = model.sample(x, y);
                    }
                }
                const double rho = std::hypot(x - 30, y - 30);
                if (rho <= 15) {          // dome interior: slopes up to 44 degrees
                    ++interior;
                    maxDev = std::fmax(maxDev, std::fabs(z - model.sample(x, y)));
                } else if (rho >= 24 && x > 3 && x < 57 && y > 3 && y < 57) {
                    ++floor;              // floor, clear of the wall fillet
                    maxFloor = std::fmax(maxFloor, std::fabs(z + 20.0));
                }
            }
        std::printf("finish: closest approach %.4f mm, max deviation dome %.4f / floor %.4f mm "
                    "(%d / %d cells)\n", worst, maxDev, maxFloor, interior, floor);
        std::printf("finish: worst cell at (%.3f, %.3f) rho %.3f: cut z %.4f, model %.4f\n",
                    wx, wy, std::hypot(wx - 30, wy - 30), wz, wm);
        check(interior > 60000 && floor > 100000, "finish: regions sampled");
        check(worst >= -0.02, "finish: never below the model (0.02 mm)");
        check(maxDev <= 0.15, "finish: dome interior within 0.15 mm of the model");
        check(maxFloor <= 0.15, "finish: floor within 0.15 mm");
        check(approx(hm.sample(30, 30), 0.0, 0.03), "finish: dome peak just kissed");
        check(!sim.throughCut, "finish: no through-cut");
    }

    // --- raster angle 90 swaps the axes; one-way directions ---------------------
    {
        c2d::Document doc;
        addToolpath(doc, toolpath("3d_finish_toolpath", kBall,
                                  {{"stepover", 0.5}, {"raster_angle", 90.0},
                                   {"direction", QStringLiteral("climb")}}));
        const c2d::GcodeResult g = c2d::exportGcode(doc);
        double along = 0, across = 0, px = 0, py = 0;
        bool allDown = true;
        for (const Op &o : g.ops) {
            if (o.kind == Op::Feed) {
                along += std::fabs(o.y - py);
                across += std::fabs(o.x - px);
                if (std::fabs(o.y - py) > 1.0 && o.y > py)
                    allDown = false;
            }
            px = o.x; py = o.y;
        }
        check(along > 20 * across, "finish: raster 90 runs along Y");
        check(approx(along, lenAlongX, lenAlongX * 0.02), "finish: raster 90 is the 0 pass set rotated");
        check(allDown, "finish: climb one-way passes all travel -Y (material on the right)");
        const c2d::JobStats st = c2d::computeStats(g.ops);
        check(st.rapidLen > st.cutLen * 0.5, "finish: one-way returns by rapid");

        c2d::Document d2;
        addToolpath(d2, toolpath("3d_finish_toolpath", kBall,
                                 {{"stepover", 0.5}, {"raster_angle", 90.0},
                                  {"direction", QStringLiteral("conventional")}}));
        const c2d::GcodeResult g2 = c2d::exportGcode(d2);
        bool allUp2 = true;
        py = 0;
        for (const Op &o : g2.ops) {
            if (o.kind == Op::Feed && std::fabs(o.y - py) > 1.0 && o.y < py)
                allUp2 = false;
            py = o.y;
        }
        check(allUp2, "finish: conventional one-way passes all travel +Y");
    }

    // --- both_directions adds the cross pass set ------------------------------
    {
        c2d::Document doc;
        addToolpath(doc, toolpath("3d_finish_toolpath", kBall,
                                  {{"stepover", 1.0}, {"raster_angle", 0.0}, {"both_directions", true}}));
        const c2d::GcodeResult g = c2d::exportGcode(doc);
        double along = 0, across = 0, px = 0, py = 0;
        for (const Op &o : g.ops) {
            if (o.kind == Op::Feed) {
                along += std::fabs(o.x - px);
                across += std::fabs(o.y - py);
            }
            px = o.x; py = o.y;
        }
        check(approx(along, across, 0.1 * along), "finish: cross pass set equals the first");
    }

    // --- boundary vectors + offset, percent stepover, flat finish tool -----------
    {
        c2d::Document doc;
        const QJsonObject layer;
        const c2d::Element e = c2d::Element::makeRectangle({30, 30}, 20, 20, layer);
        doc.addElement(e);
        QJsonObject j = toolpath("3d_finish_toolpath", kFlat,
                                 {{"stepover", QStringLiteral("20%")}, {"boundary_offset", 2.0}});
        j.insert("elements", QJsonArray{QJsonObject{{"uuid", e.id}}});
        addToolpath(doc, j);
        const c2d::GcodeResult g = c2d::exportGcode(doc);
        check(g.done.size() == 1, "finish: boundary exports");
        const c2d::JobStats st = c2d::computeStats(g.ops);
        check(st.minX >= 18 - 0.01 && st.maxX <= 42 + 0.01 && st.minY >= 18 - 0.01 && st.maxY <= 42 + 0.01,
              "finish: tool centre stays inside the boundary + offset");
        // Measure the stepover itself: the distinct Y offsets of the passes.
        // (Total cut length is a poor proxy — it also counts the Y links
        // between passes and the Z following of the surface.)
        // A pass move is a pure travel along X; the links that walk around the
        // boundary's rounded corners change Y too and are not passes.
        QVector<double> ys;
        double px = 0, py = 0;
        for (const Op &o : g.ops) {
            if (o.kind == Op::Feed && std::fabs(o.x - px) > 0.5
                && std::fabs(o.y - py) < 1e-6) {
                bool seen = false;
                for (double v : ys)
                    if (std::fabs(v - o.y) < 1e-6) { seen = true; break; }
                if (!seen)
                    ys.append(o.y);
            }
            px = o.x;
            py = o.y;
        }
        std::sort(ys.begin(), ys.end());
        double minGap = 1e9, maxGap = 0;
        for (int i = 1; i < ys.size(); ++i) {
            const double gap = ys.at(i) - ys.at(i - 1);
            minGap = std::fmin(minGap, gap);
            maxGap = std::fmax(maxGap, gap);
        }
        std::printf("finish/boundary: %d passes, gaps %.3f..%.3f mm, cutLen %.1f mm\n",
                    int(ys.size()), minGap, maxGap, st.cutLen);
        check(ys.size() >= 18 && ys.size() <= 21, "finish: 24 mm / 1.27 mm gives ~19 passes");
        // The last pass is snapped onto the far edge, so it may sit closer.
        check(approx(maxGap, 1.27, 0.02), "finish: 20% of 6.35 = 1.27 mm stepover");
        // The flat tool rides on the exact disc maximum: never into the dome.
        const c2d::SimResult sim = c2d::simulate(g.ops, c2d::toolGeometry(doc), stock(60, 60, 25));
        double worst = 1e9;
        for (int iy = 0; iy < sim.map.height(); ++iy)
            for (int ix = 0; ix < sim.map.width(); ++ix)
                worst = std::fmin(worst, sim.map.at(ix, iy)
                                  - modelMin(model, sim.map.cellCenterX(ix), sim.map.cellCenterY(iy), 0.03));
        std::printf("finish flat: closest approach %.4f mm\n", worst);
        check(worst >= -0.01, "finish: flat end mill never below the model");
        check(approx(sim.map.sample(30, 30), 0.0, 0.01), "finish: flat tool skims the peak");
    }

    // --- 3D toolpaths are not pooled with 2D ring jobs around them --------------
    {
        c2d::Document doc;
        const QJsonObject layer;
        const c2d::Element a = c2d::Element::makeRectangle({10, 10}, 8, 8, layer);
        const c2d::Element b = c2d::Element::makeRectangle({50, 50}, 8, 8, layer);
        doc.addElement(a);
        doc.addElement(b);
        auto pocket = [&](const c2d::Element &e, const char *name) {
            QJsonObject j = toolpath("pocket_toolpath", kFlat,
                                     {{"start_depth", QStringLiteral("0.000")},
                                      {"end_depth", QStringLiteral("1.000")}, {"stepdown", 1.0},
                                      {"stepover", 2.0}}, name);
            j.insert("elements", QJsonArray{QJsonObject{{"uuid", e.id}}});
            return j;
        };
        addToolpath(doc, pocket(a, "pocketA"));
        addToolpath(doc, toolpath("3d_rough_toolpath", kFlat, {{"stepdown", 5.0}, {"stepover", 3.0}}, "rough"));
        addToolpath(doc, pocket(b, "pocketB"));
        const c2d::GcodeResult g = c2d::exportGcode(doc);
        check(g.done.size() == 3 && g.skipped.isEmpty(), "pooling: all three export");
        QStringList order;
        for (const Op &o : g.ops)
            if (o.kind == Op::Comment)
                order << o.text;
        check(order == QStringList({"pocketA", "rough", "pocketB"}), "pooling: document order kept around the 3D job");
        int tools = 0;
        for (const Op &o : g.ops)
            if (o.kind == Op::Tool)
                ++tools;
        check(tools == 1, "pooling: same tool, one tool change");
    }

    // --- empty / missing model is skipped with a reason -------------------------
    {
        c2d::HeightModel empty;
        empty.baseZ = 0;
        empty.resize(10, 10);   // all NoModel, floor at the stock top: nothing to cut
        current = &empty;
        c2d::Document doc;
        addToolpath(doc, toolpath("3d_rough_toolpath", kFlat, {{"stepdown", 2.0}}, "r"));
        addToolpath(doc, toolpath("3d_finish_toolpath", kBall, {{"stepover", 0.5}}, "f"));
        const c2d::GcodeResult g = c2d::exportGcode(doc);
        std::printf("empty model: done %d, skipped %d: %s\n", int(g.done.size()), int(g.skipped.size()),
                    g.skipped.join(QStringLiteral(" | ")).toUtf8().constData());
        check(g.done.isEmpty() && g.skipped.size() == 2
                  && g.skipped.first().contains(QLatin1String("nothing to cut")),
              "empty model: rough skipped with a reason");
        check(g.done.isEmpty() && g.skipped.size() == 2 && g.skipped.first().startsWith(QLatin1String("r ")),
              "empty model: skip names the toolpath");
        current = nullptr;
        const c2d::GcodeResult g2 = c2d::exportGcode(doc);
        check(g2.done.isEmpty() && g2.skipped.size() == 2
                  && g2.skipped.first().contains(QLatin1String("no 3D model")),
              "no model: skipped with a reason");
        current = &model;
    }

    // --- panel defaults parse and export ---------------------------------------
    {
        const QJsonObject r = c2d::defaultRoughToolpathJson();
        const QJsonObject f = c2d::defaultFinishToolpathJson();
        check(r.value("type").toString() == QLatin1String("3d_rough_toolpath")
                  && f.value("type").toString() == QLatin1String("3d_finish_toolpath")
                  && !r.value("uuid").toString().isEmpty() && r.value("uuid") != f.value("uuid"),
              "defaults: typed, unique uuids");
        const c2d::Cam3dParams pr = c2d::cam3dParams(r, 3.0);
        const c2d::Cam3dParams pf = c2d::cam3dParams(f, 3.0);
        check(pr.tool.kind == c2d::ToolGeom::Flat && approx(pr.stockToLeave, 0.5, 1e-9)
                  && pf.tool.kind == c2d::ToolGeom::Ball && pf.direction == c2d::Cam3dParams::Zigzag,
              "defaults: sensible tools and modes");
        c2d::Document doc;
        addToolpath(doc, r);
        addToolpath(doc, f);
        const c2d::GcodeResult g = c2d::exportGcode(doc);
        check(g.done.size() == 2 && g.skipped.isEmpty(), "defaults: both export over the hemisphere");
    }


    // --- rough: a level on every flat terrace -----------------------------------
    {
        const c2d::HeightModel steps = terraces();
        const QVector<double> flats = c2d::modelFlats(steps, 1.0);
        std::printf("terraces: %d flats:", int(flats.size()));
        for (double f : flats)
            std::printf(" %.3f", f);
        std::printf("\n");
        check(flats.size() == 3 && approx(flats.at(0), 0.0, 1e-6) && approx(flats.at(1), -3.3, 1e-5)
                  && approx(flats.at(2), -6.4, 1e-5),
              "flats: the three terraces, highest first, floor not listed");
        check(c2d::modelFlats(model, 1.0).isEmpty(), "flats: a dome has none");
        // An almost level ramp (0.0008 mm per cell) passes the per-cell
        // flatness test everywhere but is a slope, not a terrace.
        c2d::HeightModel ramp;
        ramp.cell = 0.5;
        ramp.baseZ = -2;
        ramp.resize(400, 40);
        for (int r = 0; r < ramp.rows; ++r)
            for (int c = 0; c < ramp.cols; ++c)
                ramp.ref(c, r) = float(-0.0008 * c);
        check(c2d::modelFlats(ramp, 1.0).isEmpty(), "flats: a gentle ramp has none");
        // Too small to matter: a terrace below the area threshold.
        check(c2d::modelFlats(steps, 2000.0).isEmpty(), "flats: area threshold respected");

        current = &steps;
        c2d::Document doc;
        addToolpath(doc, toolpath("3d_rough_toolpath", kFlat,
                                  {{"stepdown", 2.0}, {"stepover", 2.5}, {"stock_to_leave", 0.0}}));
        const c2d::GcodeResult g = c2d::exportGcode(doc);
        const QSet<qint64> lv = feedLevels(g.ops);
        QList<qint64> sorted = lv.values();
        std::sort(sorted.begin(), sorted.end());
        std::printf("terraces rough levels:");
        for (qint64 z : sorted)
            std::printf(" %.3f", z / 1e4);
        std::printf("\n");
        check(lv.size() == 7 && lv.contains(-20000) && lv.contains(-33000) && lv.contains(-40000)
                  && lv.contains(-60000) && lv.contains(-64000) && lv.contains(-80000)
                  && lv.contains(-90000),
              "rough: -2, -3.3 (terrace), -4, -6, -6.4 (terrace), -8, -9 (floor)");
        c2d::SimSettings ss = stock(60, 60, 12);
        ss.part = steps;
        const c2d::SimResult sim = c2d::simulate(g.ops, c2d::toolGeometry(doc), ss);
        std::printf("terraces rough: tier2 %.3f tier1 %.3f, %d gouge cells\n",
                    sim.map.sample(30 + 9.6, 30), sim.map.sample(30 + 16.1, 30), sim.gougeCells);
        if (qgetenv("TEST_DUMP") == "1")
            for (int iy = 0; iy < sim.map.height(); ++iy)
                for (int ix = 0; ix < sim.map.width(); ++ix)
                    if (!sim.gougeMask.isEmpty() && sim.gougeMask.at(iy * sim.map.width() + ix))
                        std::fprintf(stderr, "gouge (%.2f, %.2f) sim %.3f model %.3f\n", sim.map.cellCenterX(ix),
                                     sim.map.cellCenterY(iy), sim.map.at(ix, iy),
                                     steps.sample(sim.map.cellCenterX(ix), sim.map.cellCenterY(iy)));
        check(approx(sim.map.sample(30 + 9.6, 30), -3.3, 0.01), "rough: terrace at -3.3 cleared onto it");
        check(approx(sim.map.sample(30 + 16.1, 30), -6.4, 0.01), "rough: terrace at -6.4 cleared onto it");
        check(sim.gougeCells == 0, "rough: terraces are not gouged");

        // Stock to leave lifts the terrace levels with it; levels within
        // 0.05 mm of a step merge into the terrace.
        c2d::Document d2;
        addToolpath(d2, toolpath("3d_rough_toolpath", kFlat,
                                 {{"stepdown", 2.0}, {"stepover", 2.5}, {"stock_to_leave", 0.5}}));
        const QSet<qint64> lv2 = feedLevels(c2d::exportGcode(d2).ops);
        check(lv2.contains(-28000) && lv2.contains(-59000) && lv2.contains(-85000) && lv2.size() == 7,
              "rough + leave 0.5: -2, -2.8, -4, -5.9, -6, -8, -8.5");
        c2d::Document d3;
        addToolpath(d3, toolpath("3d_rough_toolpath", kFlat,
                                 {{"stepdown", 2.0}, {"stepover", 2.5}, {"stock_to_leave", 0.37}}));
        const QSet<qint64> lv3 = feedLevels(c2d::exportGcode(d3).ops);
        check(lv3.contains(-29300) && lv3.contains(-60300) && !lv3.contains(-60000) && lv3.size() == 6,
              "rough + leave 0.37: the terrace at -6.03 replaces the step at -6");
        current = &model;
    }

    // --- finish: waterline ---------------------------------------------------------
    {
        const double leave = 0.0;
        auto run = [&](const char *dir, const QJsonObject &extra = {}) {
            c2d::Document doc;
            QJsonObject e{{"stepdown", 2.0}, {"strategy", QStringLiteral("waterline")},
                          {"direction", QLatin1String(dir)}, {"stock_to_leave", leave}};
            for (const QString &k : extra.keys())
                e.insert(k, extra.value(k));
            addToolpath(doc, toolpath("3d_finish_toolpath", kBall, e));
            return c2d::exportGcode(doc);
        };
        check(c2d::cam3dParams(QJsonObject{{"strategy", "waterline"}}, 3).strategy
                  == c2d::Cam3dParams::Waterline
                  && c2d::cam3dParams(QJsonObject{}, 3).strategy == c2d::Cam3dParams::Raster,
              "waterline: parsed; absent = raster");
        const c2d::GcodeResult g = run("climb");
        check(g.done.size() == 1 && g.skipped.isEmpty(), "waterline: exported");
        const QSet<qint64> lv = feedLevels(g.ops);
        std::printf("waterline: %d ops, %d levels\n", int(g.ops.size()), int(lv.size()));
        check(lv.size() == 10 && lv.contains(-20000) && lv.contains(-200000),
              "waterline: -2 .. -18 every 2 mm and the floor at -20");

        c2d::ToolGeom ball; ball.kind = c2d::ToolGeom::Ball; ball.diameter = 3; ball.cornerRadius = 1.5;
        auto cz = [&](double x, double y) { return c2d::compensatedZ(model, ball, x, y) + leave; };
        // How far the tool at tip height z is into the dome at (x, y),
        // measured across the compensated surface rather than up it: by a
        // wall that surface is nearly vertical, and the vertex positions
        // are rounded to the micron by the boundary clipping.
        auto into = [&](double x, double y, double z) {
            const double h = 0.05;
            const double gx = (cz(x + h, y) - cz(x - h, y)) / (2 * h);
            const double gy = (cz(x, y + h) - cz(x, y - h)) / (2 * h);
            return (cz(x, y) - z) / std::sqrt(1 + gx * gx + gy * gy);
        };
        // Every move at a level: the tip sits at that level and the tool is
        // clear of the dome there (it may not be below the compensated
        // surface), at the vertices and between them.
        int onLevel = 0, touching = 0;
        double worstGouge = -1e9, worstMid = -1e9, area10 = 0;
        double px = 0, py = 0, pz = 1e9;
        for (const Op &o : g.ops) {
            if (o.kind == Op::Feed && std::fabs(o.z - pz) < 1e-9 && o.z < -1e-6) {
                ++onLevel;
                check(lv.contains(qRound64(o.z * 1e4)), "waterline: every point at a level");
                const double c = cz(o.x, o.y);
                worstGouge = std::fmax(worstGouge, into(o.x, o.y, o.z));
                if (c >= o.z - 0.05)
                    ++touching;
                {
                    const double mx = (o.x + px) / 2, my = (o.y + py) / 2;
                    const double dm = into(mx, my, o.z);
                    if (dm > 0.02 && qgetenv("TEST_DUMP") == "1")
                        std::fprintf(stderr, "mid (%.3f, %.3f) rho %.3f z %.2f over %.4f seg %.3f\n", mx, my,
                                     std::hypot(mx - 30, my - 30), o.z, dm, std::hypot(o.x - px, o.y - py));
                    worstMid = std::fmax(worstMid, dm);
                }
                if (qRound64(o.z * 1e4) == -100000)
                    area10 += (px - 30) * (o.y - 30) - (py - 30) * (o.x - 30);
            }
            if (o.kind == Op::Feed || o.kind == Op::Rapid) {
                px = o.x; py = o.y; pz = o.z;
            }
        }
        std::printf("waterline: %d moves on levels, %d touching; worst over the surface: "
                    "vertex %.4f, mid %.4f mm\n", onLevel, touching, worstGouge, worstMid);
        check(onLevel > 500, "waterline: contours traced");
        check(worstGouge <= 0.002, "waterline: no vertex into the dome");
        check(worstMid <= 0.02, "waterline: chords stay out of the dome (0.02 mm)");
        check(touching >= onLevel * 0.95, "waterline: the tool follows the wall, not the air");
        check(area10 < 0, "waterline climb: clockwise around the dome (material on the right)");

        // The level -10 loop is the circle where the ball touches the dome at
        // that height: tip at -10, centre on the offset sphere R + r.
        {
            const double zc = -10 + 1.5 + 20;   // ball centre above the sphere centre
            const double rhoExpect = std::sqrt(21.5 * 21.5 - zc * zc);
            double lo = 1e9, hi = 0;
            for (const Op &o : g.ops)
                if (o.kind == Op::Feed && qRound64(o.z * 1e4) == -100000) {
                    const double rho = std::hypot(o.x - 30, o.y - 30);
                    lo = std::fmin(lo, rho);
                    hi = std::fmax(hi, rho);
                }
            std::printf("waterline -10: rho %.3f..%.3f, expected %.3f\n", lo, hi, rhoExpect);
            check(approx(lo, rhoExpect, 0.1) && approx(hi, rhoExpect, 0.1), "waterline: -10 loop on the offset sphere");
        }

        const c2d::GcodeResult gc = run("conventional");
        double areaC = 0;
        px = py = 0;
        for (const Op &o : gc.ops) {
            if (o.kind == Op::Feed && qRound64(o.z * 1e4) == -100000 && std::fabs(o.z - pz) < 1e-9)
                areaC += (px - 30) * (o.y - 30) - (py - 30) * (o.x - 30);
            if (o.kind == Op::Feed || o.kind == Op::Rapid) {
                px = o.x; py = o.y; pz = o.z;
            }
        }
        check(areaC > 0, "waterline conventional: anticlockwise around the dome (material on the left)");

        // Simulated against the dome: never into it, and the gouge check
        // agrees.
        c2d::Document doc;
        addToolpath(doc, toolpath("3d_finish_toolpath", kBall,
                                  {{"stepdown", 2.0}, {"strategy", QStringLiteral("waterline")}}));
        c2d::SimSettings ss = stock(60, 60, 25);
        ss.part = model;
        const c2d::SimResult sim = c2d::simulate(c2d::exportGcode(doc).ops, c2d::toolGeometry(doc), ss);
        const double halfCell = 0.5 * M_SQRT2 * sim.map.cellSize();
        double worst = 1e9;
        for (int iy = 0; iy < sim.map.height(); ++iy)
            for (int ix = 0; ix < sim.map.width(); ++ix) {
                const double x = sim.map.cellCenterX(ix), y = sim.map.cellCenterY(iy);
                worst = std::fmin(worst, sim.map.at(ix, iy) - modelMin(model, x, y, halfCell));
            }
        std::printf("waterline: closest approach %.4f mm, %d gouge cells, %d rapid hits\n",
                    worst, sim.gougeCells, sim.rapidHitCount);
        check(worst >= -0.02, "waterline: never below the model (0.02 mm)");
        check(sim.gougeCells == 0 && sim.rapidHitCount == 0, "waterline: no gouge, no rapid into stock");

        // A boundary keeps the tool centre inside it.
        {
            c2d::Document d;
            const c2d::Element e = c2d::Element::makeRectangle({30, 30}, 20, 20, QJsonObject());
            d.addElement(e);
            QJsonObject j = toolpath("3d_finish_toolpath", kBall,
                                     {{"stepdown", 2.0}, {"strategy", QStringLiteral("waterline")}});
            j.insert("elements", QJsonArray{QJsonObject{{"uuid", e.id}}});
            addToolpath(d, j);
            const c2d::GcodeResult gb = c2d::exportGcode(d);
            const c2d::JobStats st = c2d::computeStats(gb.ops);
            check(gb.done.size() == 1 && st.minX >= 20 - 0.01 && st.maxX <= 40 + 0.01
                      && st.minY >= 20 - 0.01 && st.maxY <= 40 + 0.01,
                  "waterline: tool centre stays inside the boundary");
            // Loop radii: 9.1 mm at -2 (whole), 12.5 at -4 (corners clipped
            // off), 14.9 at -6 (beyond the 14.1 mm corner: gone).
            const QSet<qint64> lvb = feedLevels(gb.ops);
            check(lvb.size() == 2 && lvb.contains(-20000) && lvb.contains(-40000),
                  "waterline: clipped loops still cut");
        }

        // Terraces: a waterline on each terrace's own height.
        current = nullptr;
        const c2d::HeightModel steps = terraces();
        current = &steps;
        c2d::Document d4;
        addToolpath(d4, toolpath("3d_finish_toolpath", kBall,
                                 {{"stepdown", 2.0}, {"strategy", QStringLiteral("waterline")}}));
        const QSet<qint64> lv4 = feedLevels(c2d::exportGcode(d4).ops);
        check(lv4.contains(-33000) && lv4.contains(-64000), "waterline: loops on the terraces");
        current = &model;
    }

    // --- compensatedZ: flat tool on the dome peak, ball on the floor --------------
    {
        c2d::ToolGeom ball; ball.kind = c2d::ToolGeom::Ball; ball.diameter = 3; ball.cornerRadius = 1.5;
        c2d::ToolGeom flat; flat.kind = c2d::ToolGeom::Flat; flat.diameter = 6.35;
        check(approx(c2d::compensatedZ(model, ball, 5, 5), -20.0, 1e-6), "compensatedZ: ball on the floor");
        // A flat tool centred on the apex rides on the highest point under its
        // disc, which is the apex itself. Compare against the model's own value
        // there: the sampled apex sits a few microns below 0 because no cell
        // centre falls exactly on it (0.5 mm cells on a 20 mm sphere).
        const double flatPeak = c2d::compensatedZ(model, flat, 30, 30);
        check(approx(flatPeak, model.sample(30, 30), 1e-9), "compensatedZ: flat rides the peak");
        check(approx(flatPeak, 0.0, 0.01), "compensatedZ: the peak is the stock top");
        // Ball 10 mm off-centre: the tip sits on the offset sphere R + r, minus r.
        const double rho = 10, Rr = 21.5;
        const double expect = -20 + std::sqrt(Rr * Rr - rho * rho) - 1.5;
        check(approx(c2d::compensatedZ(model, ball, 40, 30), expect, 0.01), "compensatedZ: ball on the slope");
        // Flat tool centred at rho 10 touches the dome at rho 10 - r.
        const double e2 = -20 + std::sqrt(400 - (rho - 3.175) * (rho - 3.175));
        // The disc footprint is quantised onto the model grid, so the highest
        // point it finds can sit a fraction of a cell further out than the
        // analytic contact at rho - r. The tool may only end up HIGHER than
        // the analytic contact (leaving material), never lower (gouging).
        const double flatRim = c2d::compensatedZ(model, flat, 40, 30);
        check(flatRim >= e2 - 1e-6, "compensatedZ: flat rim never below the analytic contact");
        check(flatRim <= e2 + 0.08, "compensatedZ: flat rim within half a cell of slope");
    }

    // --- compensatedZ: a sharp peak on a node ----------------------------------
    // The ball was let down on a sub-grid tied to the tool's position, which
    // misses the model's nodes: over a cone's apex it read the flanks, sat
    // up to 0.225 mm too low, and the raster finish took 0.12 mm off the tip.
    {
        c2d::HeightModel m;
        m.cell = 0.5;
        m.baseZ = -8;
        m.resize(80, 80);
        const double ax = 40.5 * m.cell, ay = 40.5 * m.cell;   // the apex on node (40, 40)
        for (int r = 0; r < m.rows; ++r)
            for (int c = 0; c < m.cols; ++c) {
                const double d = std::hypot((c + 0.5) * m.cell - ax, (r + 0.5) * m.cell - ay);
                m.ref(c, r) = float(std::fmax(m.baseZ, -2.0 * d));
            }
        c2d::ToolGeom ball;
        ball.kind = c2d::ToolGeom::Ball;
        ball.diameter = 6.35;
        ball.cornerRadius = 3.175;
        const double R = ball.radius();
        double worst = 0;
        for (double dx = -0.5; dx <= 0.5; dx += 0.01)
            for (double dy = -0.5; dy <= 0.5; dy += 0.05) {
                const double onApex = -(R - std::sqrt(R * R - dx * dx - dy * dy));
                worst = std::fmax(worst, onApex - c2d::compensatedZ(m, ball, ax + dx, ay + dy));
            }
        std::fprintf(stderr, "cone apex: compensatedZ at most %.4f mm below the ball on the apex\n", worst);
        check(worst < 1e-6, "compensatedZ: the ball rests on a peak node");

        c2d::Document doc;
        addToolpath(doc, toolpath("3d_finish_toolpath",
                                  QJsonObject{{"number", 1}, {"diameter", 6.35}, {"type", 1}, {"corner_radius", 3.175}},
                                  {{"stepover", 0.3}, {"stepdown", 1.0}, {"stock_to_leave", 0.0},
                                   {"strategy", "raster"}, {"direction", "zigzag"}, {"raster_angle", 0.0},
                                   {"start_depth", "0.000"}, {"max_depth", "0.000"}, {"boundary_offset", 0.0}}));
        const c2d::GcodeResult g = c2d::exportGcode(doc, nullptr, &m);
        double cut = 0, cx = 0, cy = 0, cz = 10;
        for (const Op &o : g.ops) {
            if (o.kind == Op::Feed) {
                const int n = std::max(1, int(std::hypot(o.x - cx, o.y - cy) / 0.01));
                for (int i = 0; i <= n; ++i) {
                    const double t = double(i) / n;
                    const double rho = std::hypot(cx + (o.x - cx) * t - ax, cy + (o.y - cy) * t - ay);
                    if (rho < R)   // how far the ball's surface over the apex is below it
                        cut = std::fmax(cut, -(cz + (o.z - cz) * t + R - std::sqrt(R * R - rho * rho)));
                }
            }
            if (o.kind == Op::Rapid || o.kind == Op::Feed || o.kind == Op::Arc) {
                cx = o.x;
                cy = o.y;
                cz = o.z;
            }
        }
        std::fprintf(stderr, "cone apex: raster finish takes %.3f mm off the tip\n", cut);
        check(g.done.size() == 1 && cut < 0.03, "3D finish leaves a sharp peak standing");
    }

    std::printf("test_cam3d: %d checks OK\n", g_checks);
    return 0;
}
