// Headless checks for the CAM engine (Clipper2 offsetting in gcodeexport),
// and - when a sample .c2d path is passed as
// argv[1] - a full-document export. Complements the app's --selftest, which
// covers the element factories and document round trip.
//
// Plain asserts, no test framework; exits 0 on success.

#include "../src/c2ddocument.h"
#include "../src/element.h"
#include "../src/gcodeexport.h"
#include "../src/post_grbl.h"
#include "../src/toollibrary.h"
#include "../src/toolpathfactory.h"
#include "../src/zlibutil.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

static int g_checks = 0;

static void check(bool cond, const char *what)
{
    ++g_checks;
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

static bool approx(double a, double b, double eps = 1e-6)
{
    return std::fabs(a - b) < eps;
}

// Parse "X12.7Y3.4Z-1.0" style coordinates out of a g-code line.
static bool axisValue(const QString &line, QChar axis, double *out)
{
    const int i = line.indexOf(axis);
    if (i < 0)
        return false;
    int j = i + 1;
    while (j < line.size() &&
           (line.at(j).isDigit() || line.at(j) == QChar('-') || line.at(j) == QChar('.')))
        ++j;
    *out = line.mid(i + 1, j - i - 1).toDouble();
    return true;
}

// Every cutting-move endpoint (x, y, z). The post is fully modal (CC
// dialect): both the G word and unchanged axes are omitted, so a feed line
// after the first can be a bare "X..Y..". Track the modal G and the position
// across lines; endpoints while G1 is active count as cuts.
struct CutPoint { double x, y, z; };
static QVector<CutPoint> cutPoints(const QString &gcode)
{
    QVector<CutPoint> out;
    double curX = 0, curY = 0, curZ = 0;
    int modalG = -1;
    const QStringList lines = gcode.split(QChar('\n'));
    for (const QString &line : lines) {
        if (line.isEmpty() || line.startsWith(QChar('(')))
            continue;   // comments
        if (line.startsWith(QLatin1String("G0")))
            modalG = 0;
        else if (line.startsWith(QLatin1String("G1")) || line.startsWith(QLatin1String("G2"))
                 || line.startsWith(QLatin1String("G3")))
            modalG = 1;    // arcs: the endpoint is on the cut (extremes of a
                           // rounded offset lie on the straight runs anyway)
        else if (line.startsWith(QChar('G')) || line.startsWith(QChar('M')) ||
                 line.startsWith(QChar('S')) || line.startsWith(QChar(';')))
            continue;   // other G/M/S words: no motion
        // else: bare axis words continue the current modal motion.
        bool moved = false;
        double v;
        if (axisValue(line, QChar('X'), &v)) { curX = v; moved = true; }
        if (axisValue(line, QChar('Y'), &v)) { curY = v; moved = true; }
        if (axisValue(line, QChar('Z'), &v)) { curZ = v; moved = true; }
        if (moved && modalG == 1)
            out.append({curX, curY, curZ});
    }
    return out;
}

static QRectF cutBounds(const QString &gcode, double *minZ)
{
    double minX = 1e30, maxX = -1e30, minY = 1e30, maxY = -1e30;
    *minZ = 1e30;
    const QVector<CutPoint> pts = cutPoints(gcode);
    for (const CutPoint &p : pts) {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
        *minZ = std::min(*minZ, p.z);
    }
    return pts.isEmpty() ? QRectF()
                         : QRectF(QPointF(minX, minY), QPointF(maxX, maxY));
}

// A document with one shape and one toolpath referencing it, built through
// the public factories so the CAM sees exactly what the app produces.
struct Rig {
    c2d::Document doc;   // starts empty: no load() - CAM needs only
                         // elements/toolpaths/params
    QString shapeId;

    void addShape(const c2d::Element &e) { shapeId = e.id; doc.addElement(e); }

    void addToolpath(const char *type, const QJsonObject &extra)
    {
        QJsonObject j;
        j.insert("type", QLatin1String(type));
        j.insert("name", QStringLiteral("t"));
        j.insert("enabled", true);
        j.insert("uuid", QStringLiteral("{tp-1}"));
        j.insert("start_depth", QStringLiteral("0.000"));
        j.insert("end_depth", QStringLiteral("2.000"));
        j.insert("stepdown", 2.0);
        j.insert("stock_to_leave", 0.0);
        QJsonObject speeds;
        speeds.insert("feedrate", 1000);
        speeds.insert("plungerate", 300);
        speeds.insert("rpm", 10000);
        j.insert("speeds", speeds);
        QJsonObject tool;
        tool.insert("diameter", 6.35);
        tool.insert("number", 201);
        j.insert("tool", tool);
        j.insert("elements", QJsonArray{QJsonObject{{"uuid", shapeId}}});
        for (const QString &k : extra.keys())
            j.insert(k, extra.value(k));
        c2d::Toolpath tp;
        tp.uuid = j.value("uuid").toString();
        tp.type = QLatin1String(type);
        tp.json = j;
        doc.addToolpath(tp);
    }
};

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QJsonObject layer;   // CAM never reads the layer

    // --- contour outside: cut path offset outward by the tool radius -------
    {
        Rig r;
        r.addShape(c2d::Element::makeRectangle({30, 20}, 40, 20, layer));
        r.addToolpath("contour", {{"ofset_dir", 1}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        check(g.done.size() == 1 && g.skipped.isEmpty(), "contour exports");
        double minZ;
        const QRectF bb = cutBounds(g.gcode, &minZ);
        std::fprintf(stderr, "contour outside: bbox %.3f..%.3f x %.3f..%.3f\n",
                     bb.left(), bb.right(), bb.top(), bb.bottom());
        if (qgetenv("TEST_DUMP") == "1")
            std::fprintf(stderr, "%s\n", g.gcode.toUtf8().constData());
        check(approx(bb.left(), 10 - 3.175, 0.05) &&
              approx(bb.right(), 50 + 3.175, 0.05) &&
              approx(bb.top(), 10 - 3.175, 0.05) &&
              approx(bb.bottom(), 30 + 3.175, 0.05),
              "contour outside bbox");
        check(approx(minZ, -2.0, 1e-3), "contour reaches end depth");
    }

    // --- contour inside ----------------------------------------------------
    {
        Rig r;
        r.addShape(c2d::Element::makeRectangle({30, 20}, 40, 20, layer));
        r.addToolpath("contour", {{"ofset_dir", -1}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        double minZ;
        const QRectF bb = cutBounds(g.gcode, &minZ);
        check(approx(bb.left(), 10 + 3.175, 0.05) &&
              approx(bb.right(), 50 - 3.175, 0.05),
              "contour inside bbox");
    }

    // --- contour no-offset follows the vector ------------------------------
    {
        Rig r;
        r.addShape(c2d::Element::makeRectangle({30, 20}, 40, 20, layer));
        r.addToolpath("contour", {{"ofset_dir", 0}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        double minZ;
        const QRectF bb = cutBounds(g.gcode, &minZ);
        check(approx(bb.left(), 10, 0.05) && approx(bb.right(), 50, 0.05),
              "contour follow bbox");
    }

    // --- contour stock_to_leave adds to the offset -------------------------
    {
        Rig r;
        r.addShape(c2d::Element::makeRectangle({30, 20}, 40, 20, layer));
        r.addToolpath("contour", {{"ofset_dir", 1}, {"stock_to_leave", 0.5}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        double minZ;
        const QRectF bb = cutBounds(g.gcode, &minZ);
        check(approx(bb.right(), 50 + 3.175 + 0.5, 0.05),
              "stock_to_leave honoured");
    }

    // --- cutout: number depth fields, outside unless flipped ---------------
    {
        Rig r;
        r.addShape(c2d::Element::makeRectangle({30, 20}, 40, 20, layer));
        r.addToolpath("cutout", {{"cut_depth", 5.0}, {"depth_per_pass", 2.5}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        check(g.done.size() == 1, "cutout exports");
        double minZ;
        const QRectF bb = cutBounds(g.gcode, &minZ);
        check(approx(bb.right(), 50 + 3.175, 0.05), "cutout cuts outside");
        check(approx(minZ, -5.0, 1e-3), "cutout uses cut_depth");
    }

    // --- cutout with tabs: the last pass lifts to tab height ---------------
    {
        Rig r;
        r.addShape(c2d::Element::makeRectangle({30, 20}, 40, 20, layer));
        r.addToolpath("cutout", {{"cut_depth", 5.0}, {"tab_height", 3.0},
                                 {"ignore_tabs", false}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        double minZ;
        cutBounds(g.gcode, &minZ);
        check(g.done.size() == 1 && approx(minZ, -5.0, 1e-3), "cutout with tabs exports");
        check(g.gcode.contains(QLatin1String("Z-2.000")), "tabs lift to cut_depth - tab_height");
    }

    // --- pocket: stays inside, has inner rings -----------------------------
    {
        Rig r;
        r.addShape(c2d::Element::makeRectangle({30, 20}, 40, 20, layer));
        r.addToolpath("pocket_toolpath", {{"stepover", 3.0}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        check(g.done.size() == 1, "pocket exports");
        double minZ;
        const QRectF bb = cutBounds(g.gcode, &minZ);
        check(bb.left() >= 10 + 3.175 - 0.05 && bb.right() <= 50 - 3.175 + 0.05 &&
              bb.top() >= 10 + 3.175 - 0.05 && bb.bottom() <= 30 - 3.175 + 0.05,
              "pocket stays inside");
        // Multiple shells: some cut strictly inside the boundary pass.
        bool inner = false;
        const QVector<CutPoint> pts = cutPoints(g.gcode);
        for (const CutPoint &p : pts)
            if (p.x > 10 + 3.175 + 1.0 && p.x < 50 - 3.175 - 1.0 &&
                p.y > 10 + 3.175 + 1.0 && p.y < 30 - 3.175 - 1.0)
                inner = true;
        check(inner, "pocket has inner rings");
    }

    // --- pocket rest machining: only what the 12.7 tool left in the corners --
    {
        Rig full;
        full.addShape(c2d::Element::makeRectangle({30, 20}, 40, 20, layer));
        full.addToolpath("pocket_toolpath", {{"stepover", 3.0}});
        const double fullLen = c2d::computeStats(c2d::exportGcode(full.doc).ops).cutLen;

        Rig r;
        r.addShape(c2d::Element::makeRectangle({30, 20}, 40, 20, layer));
        r.addToolpath("pocket_toolpath", {{"stepover", 3.0}, {"enable_rest", true},
                                          {"rest_diameter", 12.7}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        check(g.done.size() == 1 && g.skipped.isEmpty(), "rest pocket exports");
        const double restLen = c2d::computeStats(g.ops).cutLen;
        double minZ;
        const QRectF bb = cutBounds(g.gcode, &minZ);
        std::fprintf(stderr, "rest pocket: bbox %.3f..%.3f x %.3f..%.3f, cut %.1f mm "
                             "(full pocket %.1f mm)\n",
                     bb.left(), bb.right(), bb.top(), bb.bottom(), restLen, fullLen);
        // The rest rings sit in the four corners: the bbox reaches the corners
        // of the tool-radius inset, while the whole middle is untouched.
        check(bb.left() < 10 + 3.175 + 0.3 && bb.right() > 50 - 3.175 - 0.3 &&
              bb.top() < 10 + 3.175 + 0.3 && bb.bottom() > 30 - 3.175 - 0.3,
              "rest reaches all four corners");
        check(bb.left() >= 10 + 3.175 - 0.05 && bb.right() <= 50 - 3.175 + 0.05,
              "rest stays inside the pocket");
        bool middle = false;
        for (const CutPoint &p : cutPoints(g.gcode))
            if (p.x > 20 && p.x < 40 && p.y > 15 && p.y < 25)
                middle = true;
        check(!middle, "rest leaves the cleared middle alone");
        check(approx(minZ, -2.0, 1e-3), "rest reaches end depth");
        check(restLen < fullLen * 0.35, "rest cuts far less than a full pocket");

        // rest_diameter not larger than the tool: a normal full pocket.
        Rig same;
        same.addShape(c2d::Element::makeRectangle({30, 20}, 40, 20, layer));
        same.addToolpath("pocket_toolpath", {{"stepover", 3.0}, {"enable_rest", true},
                                             {"rest_diameter", 6.35}});
        const double sameLen = c2d::computeStats(c2d::exportGcode(same.doc).ops).cutLen;
        check(approx(sameLen, fullLen, 1e-6), "rest with same-size tool = full pocket");
    }

    // --- drilling: pecks at the circle center ------------------------------
    {
        Rig r;
        r.addShape(c2d::Element::makeCircle({15, 15}, 2, layer));
        r.addToolpath("drilling_toolpath", {{"end_depth", QStringLiteral("3.000")},
                                            {"peck_distance", 1.0}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        check(g.done.size() == 1, "drilling exports");
        double minZ;
        const QRectF bb = cutBounds(g.gcode, &minZ);
        check(approx(bb.left(), 15, 1e-3) && approx(bb.right(), 15, 1e-3) &&
              approx(bb.top(), 15, 1e-3), "drill at center");
        check(approx(minZ, -3.0, 1e-3), "drill reaches depth");
        check(g.gcode.count(QLatin1String("G1")) == 3, "drill pecks");
    }

    // --- tool library: embedded catalogue applies CC's key names -----------
    {
        c2d::ToolLibrary lib;
        QString err;
        check(lib.loadDefault(&err), "tool library loads from the embedded resource");
        check(lib.tools().size() >= 12 && lib.materials().size() == 6, "library content");
        const c2d::LibraryTool *t102 = lib.byNumber(102);
        check(t102 && approx(t102->diameter, 3.175) && t102->ccType() == 0, "#102 present");
        check(lib.byNumber(301) && lib.byNumber(301)->ccType() == 2
                  && approx(lib.byNumber(301)->angle, 90),
              "#301 is a 90 degree V-bit");
        check(lib.byNumber(202) && lib.byNumber(202)->ccType() == 1, "#202 is a ball");
        check(lib.materialIdForCC(QStringLiteral("Softwood")) == QLatin1String("softwood")
                  && lib.materialIdForCC(QStringLiteral("Aluminum")) == QLatin1String("aluminium"),
              "CC material names map");
        // Every tool has feeds for every material.
        for (const c2d::LibraryTool &t : lib.tools())
            for (const c2d::LibraryMaterial &m : lib.materials())
                check(t.feedsFor(m.id) && t.feedsFor(m.id)->valid()
                          && t.feedsFor(m.id)->stepdown > 0 && t.feedsFor(m.id)->stepoverPct > 0,
                      "feeds complete");

        Rig r;
        r.addShape(c2d::Element::makeRectangle({30, 20}, 40, 20, layer));
        r.addToolpath("contour", {{"ofset_dir", 1}, {"stepover", 3.0}});
        c2d::Toolpath tp = r.doc.toolpaths().first();
        QJsonObject j = tp.json;
        const c2d::ToolFeeds f = *t102->feedsFor(QStringLiteral("hardwood"));
        c2d::ToolLibrary::applyToToolpath(j, *t102, f);
        const QJsonObject tool = j.value("tool").toObject();
        const QJsonObject speeds = j.value("speeds").toObject();
        check(tool.value("number").toInt() == 102 && approx(tool.value("diameter").toDouble(), 3.175)
                  && tool.value("flutes").toInt() == 2 && tool.value("type").toInt() == 0
                  && tool.value("name").toString().startsWith(QStringLiteral("#102"))
                  && tool.value("read_only").isBool() && tool.contains("plungerate")
                  && tool.contains("slot_feedrate") && tool.contains("vendor"),
              "tool object uses CC keys");
        check(approx(speeds.value("feedrate").toDouble(), f.feed)
                  && approx(speeds.value("plungerate").toDouble(), f.plunge)
                  && approx(speeds.value("rpm").toDouble(), f.rpm),
              "speeds object written");
        check(approx(j.value("stepdown").toDouble(), f.stepdown)
                  && approx(j.value("stepover").toDouble(), 3.175 * f.stepoverPct / 100.0)
                  && j.value("end_depth").isString(),
              "stepdown/stepover written, depth strings untouched");
        tp.json = j;
        r.doc.replaceToolpath(tp);
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        double minZ;
        const QRectF bb = cutBounds(g.gcode, &minZ);
        check(approx(bb.right(), 50 + 1.5875, 0.05), "exporter picks up the new tool diameter");
        check(g.gcode.contains(QStringLiteral("M03S%1").arg(int(f.rpm)))
                  && g.gcode.contains(QStringLiteral("F%1").arg(f.feed, 0, 'f', 1)),
              "exporter picks up the new speeds");
    }

    // --- unsupported type is skipped ----------------------------------------
    {
        Rig r;
        r.addShape(c2d::Element::makeRectangle({30, 20}, 40, 20, layer));
        r.addToolpath("bogus_toolpath", {});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        check(g.done.isEmpty() && g.skipped.size() == 1, "unsupported skipped");
    }

    // --- build 843 negative-down depths give the same machine Z -------------
    {
        Rig r;
        r.addShape(c2d::Element::makeRectangle({30, 20}, 40, 20, layer));
        r.addToolpath("contour", {{"ofset_dir", 0},
                                  {"end_depth", QStringLiteral("-2.000")}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        double minZ;
        cutBounds(g.gcode, &minZ);
        check(approx(minZ, -2.0, 1e-3), "negative-down depth normalized");
    }

    // --- v-carve of a rounded slot keeps its medial axis ----------------------
    // Spur pruning must stop at the axis: a 60x2 slot with semicircular ends
    // once collapsed to a single plunge (every axis endpoint is a "smooth"
    // boundary vertex).
    {
        Rig r;
        QVector<QPointF> pts;
        const double L = 60, w = 2, R = w / 2;
        for (int i = 0; i <= 16; ++i) {          // right cap, bottom to top
            const double a = -M_PI / 2 + M_PI * i / 16;
            pts.append(QPointF(10 + L + R * std::cos(a), 20 + R * std::sin(a)));
        }
        for (int i = 0; i <= 16; ++i) {          // left cap, top to bottom
            const double a = M_PI / 2 + M_PI * i / 16;
            pts.append(QPointF(10 + R * std::cos(a), 20 + R * std::sin(a)));
        }
        r.addShape(c2d::Element::makePath(pts, true, layer));
        r.addToolpath("advanced_vcarve_toolpath", {{"end_depth", QStringLiteral("-3.000")},
                                                   {"start_depth", QStringLiteral("0.000")}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        double minZ;
        const QRectF bb = cutBounds(g.gcode, &minZ);
        check(g.done.size() == 1, "slot v-carve exports");
        check(bb.width() > L * 0.8, "slot v-carve rides the whole axis, not one plunge");
    }

    // --- post: a tool change forgets every modal word ------------------------
    {
        QVector<c2d::Op> ops;
        ops.append(c2d::Op::tool(1));
        ops.append(c2d::Op::rapid(0, 0, 5));
        ops.append(c2d::Op::feedTo(0, 0, -1, 500));
        ops.append(c2d::Op::feedTo(10, 0, -1, 500));
        ops.append(c2d::Op::tool(2));
        ops.append(c2d::Op::feedTo(20, 0, -1, 500));   // same G/Z/F as before the change
        const QStringList lines = c2d::GrblPost(true).generate(ops).split(QChar('\n'));
        const int marker = int(lines.indexOf(QStringLiteral("M0 ;T2")));
        check(marker >= 0, "tool marker emitted");
        const QString after = marker >= 0 ? lines.value(marker + 1) : QString();
        check(after.startsWith(QLatin1String("G1")) && after.contains(QLatin1String("Z-1.000"))
                  && after.contains(QLatin1String("F500")),
              "first move after a tool change carries G, Z and F again");
    }

    // --- engrave outline: trace the vector exactly, no offset --------------
    {
        Rig r;
        r.addShape(c2d::Element::makeRectangle({30, 20}, 40, 20, layer));
        r.addToolpath("engrave_toolpath",
                      {{"mode", "outline"}, {"end_depth", "0.500"}, {"stepdown", 0.5}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        check(g.done.size() == 1 && g.skipped.isEmpty(), "engrave outline exports");
        double minZ;
        const QRectF bb = cutBounds(g.gcode, &minZ);
        check(approx(bb.left(), 10, 1e-3) && approx(bb.right(), 50, 1e-3)
              && approx(bb.top(), 10, 1e-3) && approx(bb.bottom(), 30, 1e-3),
              "engrave outline follows the rectangle");
        check(approx(minZ, -0.5, 1e-3), "engrave outline depth");
        // Every cut point lies on the rectangle's edges.
        bool onEdge = true;
        for (const CutPoint &p : cutPoints(g.gcode))
            if (!(approx(p.x, 10, 1e-3) || approx(p.x, 50, 1e-3)
                  || approx(p.y, 10, 1e-3) || approx(p.y, 30, 1e-3)))
                onEdge = false;
        check(onEdge, "engrave outline stays on the vector");
    }

    // --- engrave fill: 1 mm hatch on a 10x10 square, Ø0.5 tool -------------
    {
        Rig r;
        r.addShape(c2d::Element::makeRectangle({5, 5}, 10, 10, layer));
        QJsonObject tool;
        tool.insert("diameter", 0.5);
        tool.insert("number", 501);
        tool.insert("angle", 0);
        r.addToolpath("engrave_toolpath",
                      {{"mode", "fill"}, {"end_depth", "0.500"}, {"stepdown", 0.5},
                       {"line_spacing", 1.0}, {"angle", 0}, {"crosshatch", false},
                       {"tool", tool}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        check(g.done.size() == 1 && g.skipped.isEmpty(), "engrave fill exports");
        // Hatch runs: horizontal feed moves spanning the inset width (9.5 mm)
        // strictly inside the inset boundary (y = 0.25 / 9.75 are the outline).
        int hatch = 0, inside = 0, cuts = 0;
        double px = 0, py = 0, pz = 5;
        for (const c2d::Op &op : g.ops) {
            if (op.kind == c2d::Op::Feed || op.kind == c2d::Op::Arc) {
                if (op.z < -1e-6 && pz < -1e-6) {
                    ++cuts;
                    if (op.x >= 0.25 - 1e-3 && op.x <= 9.75 + 1e-3
                        && op.y >= 0.25 - 1e-3 && op.y <= 9.75 + 1e-3)
                        ++inside;
                    if (approx(op.y, py, 1e-6) && std::fabs(op.x - px) > 9.0
                        && op.y > 0.3 && op.y < 9.7)
                        ++hatch;
                }
            }
            if (op.kind == c2d::Op::Feed || op.kind == c2d::Op::Arc
                || op.kind == c2d::Op::Rapid) {
                px = op.x; py = op.y; pz = op.z;
            }
        }
        std::fprintf(stderr, "engrave fill: %d hatch runs, %d/%d cut moves inside\n",
                     hatch, inside, cuts);
        check(hatch == 9, "engrave fill yields 9 hatch lines at 1 mm");
        check(cuts > 0 && inside == cuts, "engrave fill stays inside the inset region");
        // The zigzag keeps the engraver down: far fewer retracts than runs.
        int rapids = 0;
        for (const c2d::Op &op : g.ops)
            if (op.kind == c2d::Op::Rapid)
                ++rapids;
        check(rapids < 9, "engrave fill chains hatch runs without retracting");
        double minZ;
        cutBounds(g.gcode, &minZ);
        check(approx(minZ, -0.5, 1e-3), "engrave fill depth");
    }

    // --- toolpath lifecycle round trip through a .c2d container ------------
    // Build a minimal CC-schema container, create toolpaths with the factory,
    // save / reload, delete + reorder, save / reload: count, order, the
    // engrave type and params.num_toolpaths must all come back.
    {
        const QString dir = QDir::tempPath();
        const QString src = dir + QStringLiteral("/phobicccp_test_src.c2d");
        const QString out = dir + QStringLiteral("/phobicccp_test_out.c2d");
        QFile::remove(src);
        QFile::remove(out);
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                        QStringLiteral("mk"));
            db.setDatabaseName(src);
            check(db.open(), "create container");
            QSqlQuery q(db);
            q.exec(QStringLiteral("CREATE TABLE metadata(key TEXT PRIMARY KEY, value TEXT)"));
            q.exec(QStringLiteral("CREATE TABLE params(key TEXT PRIMARY KEY, value TEXT)"));
            q.exec(QStringLiteral("CREATE TABLE sqlar(name TEXT PRIMARY KEY, mode INT, "
                                  "mtime INT, sz INT, data BLOB)"));
            q.exec(QStringLiteral("CREATE TABLE items(id INTEGER PRIMARY KEY AUTOINCREMENT, "
                                  "uuid TEXT UNIQUE, name TEXT, type TEXT, version TEXT, "
                                  "sz INT, data BLOB)"));
            for (const char *kv : {"width|100", "height|100", "thickness|10",
                                   "num_toolpaths|0", "material|Softwood",
                                   "retract|2.54", "display_mm|1"}) {
                const QStringList p = QString::fromLatin1(kv).split(QChar('|'));
                QSqlQuery ins(db);
                ins.prepare(QStringLiteral("INSERT INTO params(key,value) VALUES(?,?)"));
                ins.addBindValue(p.at(0));
                ins.addBindValue(p.at(1));
                check(ins.exec(), "param row");
            }
            const QByteArray group = "{\"enabled\":true,\"expanded\":true,"
                                     "\"name\":\"Group 1\",\"uuid\":\"{g-1}\"}";
            QSqlQuery ins(db);
            ins.prepare(QStringLiteral("INSERT INTO items(uuid,name,type,version,sz,data) "
                                       "VALUES('{g-1}','','toolpath_group','J1',?,?)"));
            ins.addBindValue(group.size());
            ins.addBindValue(c2d::zlibDeflate(group));
            check(ins.exec(), "group row");
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("mk"));

        c2d::Document doc;
        QString err;
        check(doc.load(src, &err), "load minimal container");
        check(doc.toolpathGroups().size() == 1
              && doc.defaultToolpathGroup() == QLatin1String("{g-1}"),
              "group row read");
        const c2d::Element sq = c2d::Element::makeRectangle({50, 50}, 20, 20, doc.defaultLayer());
        doc.addElement(sq);
        const c2d::Toolpath a = c2d::makeToolpath(doc, QStringLiteral("contour"), {sq.id});
        doc.addToolpath(a);
        const c2d::Toolpath b = c2d::makeToolpath(doc, QStringLiteral("pocket_toolpath"), {sq.id});
        doc.addToolpath(b);
        const c2d::Toolpath e = c2d::makeToolpath(doc, QStringLiteral("engrave_toolpath"), {sq.id});
        doc.addToolpath(e);
        check(a.json.value("name").toString() == QLatin1String("Contour Toolpath 1")
              && b.json.value("name").toString() == QLatin1String("Pocket Toolpath 1")
              && e.json.value("name").toString() == QLatin1String("Engrave 1"),
              "factory names");
        check(a.json.value("toolpath_group").toString() == QLatin1String("{g-1}")
              && a.json.value("end_depth").isString()
              && a.json.value("ofset_dir").toInt() == -1
              && a.json.value("tool").toObject().value("number").toInt() == 201
              && b.json.value("stepover").isDouble()
              && e.json.value("mode").toString() == QLatin1String("outline")
              && e.json.value("tool").toObject().value("number").toInt() == 501,
              "factory defaults");
        check(c2d::exportGcode(doc).done.size() == 3, "factory toolpaths all export");

        check(doc.save(out, &err), "save with new toolpaths");
        c2d::Document back;
        check(back.load(out, &err), "reload");
        check(back.toolpaths().size() == 3
              && back.toolpaths().at(0).uuid == a.uuid
              && back.toolpaths().at(1).uuid == b.uuid
              && back.toolpaths().at(2).uuid == e.uuid
              && back.toolpaths().at(2).type == QLatin1String("engrave_toolpath")
              && back.params().value("num_toolpaths") == QLatin1String("3"),
              "reload keeps the new toolpaths, their order and the engrave type");

        // delete the pocket, move the engrave to the top
        check(back.removeToolpath(b.uuid), "remove");
        check(back.moveToolpath(e.uuid, 0), "move");
        check(back.save(out, &err), "save after delete/move");
        c2d::Document again;
        check(again.load(out, &err), "reload 2");
        check(again.toolpaths().size() == 2
              && again.toolpaths().at(0).uuid == e.uuid
              && again.toolpaths().at(1).uuid == a.uuid
              && !again.toolpathByUuid(b.uuid)
              && again.toolpathByUuid(e.uuid)->json.value("mode").toString()
                     == QLatin1String("outline")
              && again.params().value("num_toolpaths") == QLatin1String("2"),
              "delete + move persist through save/reload");
        const c2d::Toolpath d = c2d::duplicateToolpath(*again.toolpathByUuid(a.uuid));
        check(d.uuid != a.uuid && d.json.value("uuid").toString() == d.uuid
              && d.json.value("name").toString().endsWith(QLatin1String(" copy")),
              "duplicate gets a new uuid");
        QFile::remove(src);
        QFile::remove(out);
    }

    // --- full-document export over the sample (argv[1]) ---------------------
    if (argc > 1) {
        c2d::Document doc;
        QString err;
        check(doc.load(QString::fromLocal8Bit(argv[1]), &err), "load sample");
        const c2d::GcodeResult g = c2d::exportGcode(doc);
        check(!g.done.isEmpty(), "sample exports toolpaths");
        check(g.skipped.isEmpty(), "sample exports every toolpath type");
        check(g.gcode.startsWith(QLatin1String("G90")) &&
              g.gcode.trimmed().endsWith(QLatin1String("M02")),
              "sample program framing");
    } else {
        std::fprintf(stderr, "note: no .c2d passed, sample export skipped\n");
    }

    std::printf("OK: %d checks passed\n", g_checks);
    return 0;
}
