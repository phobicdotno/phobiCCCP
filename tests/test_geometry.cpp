// Headless checks for the CAM engine (Clipper2 offsetting in gcodeexport),
// and - when a sample .c2d path is passed as
// argv[1] - a full-document export. Complements the app's --selftest, which
// covers the element factories and document round trip.
//
// Plain asserts, no test framework; exits 0 on success.

#include "../src/c2ddocument.h"
#include "../src/cam3d.h"
#include "../src/element.h"
#include "../src/gcodeexport.h"
#include "../src/heightmodel.h"
#include "../src/setupsheet.h"
#include "../src/sketch.h"
#include "../src/post_grbl.h"
#include "../src/toollibrary.h"
#include "../src/toolpathfactory.h"
#include "../src/zlibutil.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
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

// Closest the tool centre comes to `box` on any straight feed below Z0, and
// the number of retracts (rapids up from below Z0) - for the link checks.
static double closestFeedTo(const QVector<c2d::Op> &ops, const QRectF &box)
{
    double worst = 1e9;
    double cx = 0, cy = 0, cz = 10;
    for (const c2d::Op &o : ops) {
        if (o.kind == c2d::Op::Feed && std::max(cz, o.z) < -0.01) {
            for (int i = 0; i <= 400; ++i) {
                const double t = i / 400.0;
                const double x = cx + (o.x - cx) * t, y = cy + (o.y - cy) * t;
                const double dx = std::max({box.left() - x, 0.0, x - box.right()});
                const double dy = std::max({box.top() - y, 0.0, y - box.bottom()});
                worst = std::min(worst, std::hypot(dx, dy));
            }
        }
        if (o.kind == c2d::Op::Rapid || o.kind == c2d::Op::Feed || o.kind == c2d::Op::Arc) {
            cx = o.x;
            cy = o.y;
            cz = o.z;
        }
    }
    return worst;
}

static int retractsFromCut(const QVector<c2d::Op> &ops)
{
    int n = 0;
    double cz = 10;
    for (const c2d::Op &o : ops) {
        if (o.kind == c2d::Op::Rapid && o.z > cz + 1e-6 && cz < 0)
            ++n;
        if (o.kind == c2d::Op::Rapid || o.kind == c2d::Op::Feed || o.kind == c2d::Op::Arc)
            cz = o.z;
    }
    return n;
}

// Every point the tool centre (or a V-bit's cone, `coneTan` > 0: the
// radius it cuts at that depth) passes below Z -0.01 on feeds and arcs,
// handed to `at(x, y, z)`, sampled every 0.02 mm.
template <typename At>
static void forEachCutPoint(const QVector<c2d::Op> &ops, At at)
{
    double cx = 0, cy = 0, cz = 10;
    for (const c2d::Op &o : ops) {
        if ((o.kind == c2d::Op::Feed || o.kind == c2d::Op::Arc) && std::max(cz, o.z) < -0.01) {
            double sweep = 0, R = 0, a0 = 0, ox = 0, oy = 0;
            if (o.kind == c2d::Op::Arc) {
                ox = cx + o.ci;
                oy = cy + o.cj;
                R = std::hypot(o.ci, o.cj);
                a0 = std::atan2(cy - oy, cx - ox);
                sweep = std::atan2(o.y - oy, o.x - ox) - a0;
                if (o.cw) { while (sweep >= -1e-9) sweep -= 2 * M_PI; }
                else { while (sweep <= 1e-9) sweep += 2 * M_PI; }
            }
            const double len = o.kind == c2d::Op::Arc ? std::fabs(sweep) * R
                                                      : std::hypot(o.x - cx, o.y - cy);
            const int n = std::max(2, int(len / 0.02));
            for (int i = 0; i <= n; ++i) {
                const double t = double(i) / n;
                const double x = o.kind == c2d::Op::Arc ? ox + R * std::cos(a0 + sweep * t) : cx + (o.x - cx) * t;
                const double y = o.kind == c2d::Op::Arc ? oy + R * std::sin(a0 + sweep * t) : cy + (o.y - cy) * t;
                at(x, y, cz + (o.z - cz) * t);
            }
        }
        if (o.kind == c2d::Op::Rapid || o.kind == c2d::Op::Feed || o.kind == c2d::Op::Arc) {
            cx = o.x;
            cy = o.y;
            cz = o.z;
        }
    }
}

// Distance from (x, y) to the closed rings, negative outside the region
// they bound (even-odd).
static double signedRingDist(const QVector<QPolygonF> &rings, double x, double y)
{
    const QPointF q(x, y);
    double d = 1e30;
    int in = 0;
    for (const QPolygonF &ring : rings) {
        for (int i = 0; i < ring.size(); ++i) {
            const QPointF a = ring.at(i), b = ring.at((i + 1) % ring.size()), ab = b - a;
            const double l2 = ab.x() * ab.x() + ab.y() * ab.y();
            const double t = l2 > 0 ? std::clamp(((x - a.x()) * ab.x() + (y - a.y()) * ab.y()) / l2, 0.0, 1.0) : 0.0;
            d = std::min(d, QLineF(q, a + ab * t).length());
        }
        in += ring.containsPoint(q, Qt::OddEvenFill) ? 1 : 0;
    }
    return (in & 1) ? d : -d;
}

// Lengths of the stretches of the closed outline `part` that no cut below
// `tabTop` touched (a tool of radius r reaching within 0.01 mm of it): the
// part's edge each tab holds.
static QVector<double> tabHolds(const QVector<c2d::Op> &ops, const QPolygonF &part, double r, double tabTop)
{
    QHash<QPair<int, int>, QVector<QPointF>> grid;
    const double cs = r + 0.01;
    forEachCutPoint(ops, [&](double x, double y, double z) {
        if (z < tabTop - 0.02)
            grid[qMakePair(int(std::floor(x / cs)), int(std::floor(y / cs)))].append(QPointF(x, y));
    });
    auto touched = [&](const QPointF &q) {
        const int ix = int(std::floor(q.x() / cs)), iy = int(std::floor(q.y() / cs));
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (const QPointF &c : grid.value(qMakePair(ix + dx, iy + dy)))
                    if (QLineF(c, q).length() <= cs)
                        return true;
        return false;
    };
    QVector<bool> cut;
    const double step = 0.05;
    for (int i = 0; i + 1 < part.size(); ++i) {
        const QLineF e(part.at(i), part.at(i + 1));
        const int n = std::max(1, int(std::ceil(e.length() / step)));
        for (int k = 0; k < n; ++k)
            cut << touched(e.pointAt(double(k) / n));
    }
    const int n = cut.size();
    int s0 = 0;
    while (s0 < n && !cut.at(s0))
        ++s0;
    if (s0 == n)
        return {n * step};
    QVector<double> holds;
    int run = 0;
    for (int k = 1; k <= n; ++k) {
        if (!cut.at((s0 + k) % n)) {
            ++run;
        } else if (run) {
            holds << run * step;
            run = 0;
        }
    }
    return holds;
}

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

    // --- cutout tabs are measured from the cut depth, not the break-through.
    // The break-through goes into the spoilboard; counting it made every tab
    // thinner by that much (Z-2.500 here instead of Z-2.000).
    {
        Rig r;
        r.addShape(c2d::Element::makeRectangle({30, 20}, 40, 20, layer));
        r.addToolpath("cutout", {{"cut_depth", 5.0}, {"break_through", 0.5},
                                 {"tab_height", 3.0}, {"ignore_tabs", false}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        double minZ;
        cutBounds(g.gcode, &minZ);
        check(g.done.size() == 1 && approx(minZ, -5.5, 1e-3), "break-through cuts below cut_depth");
        check(g.gcode.contains(QLatin1String("Z-2.000")) && !g.gcode.contains(QLatin1String("Z-2.500")),
              "tabs keep their full height above the cut depth with a break-through");
    }

    // --- spindle: kept running between toolpaths that share a tool, stopped
    // only for a tool change and at the end, and every start from standstill
    // waits for spin-up before the first move.
    {
        Rig r;
        r.addShape(c2d::Element::makeRectangle({30, 20}, 40, 20, layer));
        r.addToolpath("contour", {{"ofset_dir", 1}});
        r.addToolpath("contour", {{"ofset_dir", -1}, {"uuid", QStringLiteral("{tp-2}")},
                                  {"name", QStringLiteral("u")}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        const QStringList l = g.gcode.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        const QString spin = QStringLiteral("G4P%1").arg(c2d::kSpindleSpinUpSec, 0, 'f', 2);
        check(g.done.size() == 2 && l.filter(QStringLiteral("M03")).size() == 1
                  && l.filter(QStringLiteral("M05")).size() == 1,
              "same tool: one spindle start and one stop for two toolpaths");
        const int on = int(l.indexOf(QStringLiteral("M03S10000")));
        check(on > 0 && l.value(on + 1) == spin, "the spindle start waits for spin-up");
        check(l.indexOf(QStringLiteral("M05")) == l.size() - 2, "the spindle stops at the end only");

        Rig t;
        t.addShape(c2d::Element::makeRectangle({30, 20}, 40, 20, layer));
        t.addToolpath("contour", {{"ofset_dir", 1}});
        t.addToolpath("contour", {{"ofset_dir", -1}, {"uuid", QStringLiteral("{tp-2}")},
                                  {"name", QStringLiteral("u")},
                                  {"tool", QJsonObject{{"diameter", 3.175}, {"number", 102}}}});
        const QStringList m = c2d::exportGcode(t.doc).gcode.split(QLatin1Char('\n'),
                                                                 Qt::SkipEmptyParts);
        const int change = int(m.indexOf(QStringLiteral("M0 ;T102")));
        check(change > 0 && m.value(change - 1) == QLatin1String("M05"),
              "tool change: the spindle stops first");
        int starts = 0;
        for (int i = 0; i < m.size(); ++i)
            if (m.at(i).startsWith(QLatin1String("M03")))
                starts += m.value(i + 1) == spin ? 1 : -100;
        check(starts == 2, "tool change: both starts wait for spin-up");
    }

    // --- spindle: speeding up a running spindle also waits -----------------
    {
        QVector<c2d::Op> ops;
        ops.append(c2d::Op::spindle(10000));
        ops.append(c2d::Op::spindle(20000));
        ops.append(c2d::Op::spindle(15000));
        const QStringList l = c2d::GrblPost(true).generate(ops).split(QLatin1Char('\n'));
        const int up = int(l.indexOf(QStringLiteral("S20000")));
        const int down = int(l.indexOf(QStringLiteral("S15000")));
        check(up > 0 && l.value(up + 1).startsWith(QLatin1String("G4P")),
              "a higher rpm on a running spindle waits for it");
        check(down > 0 && !l.value(down + 1).startsWith(QLatin1String("G4P")),
              "slowing down does not wait");
    }

    // --- ramping + tabs: the ramp must not cut through the tabs -------------
    {
        Rig r;
        r.addShape(c2d::Element::makeRectangle({30, 20}, 40, 20, layer));
        r.addToolpath("cutout", {{"cut_depth", 12.0}, {"depth_per_pass", 3.0},
                                 {"tab_height", 3.0}, {"ignore_tabs", false},
                                 {"enable_ramping", true}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        double atBottom = 0, px = 0, py = 0, pz = 0;
        for (const c2d::Op &op : g.ops) {
            if ((op.kind == c2d::Op::Feed || op.kind == c2d::Op::Arc)
                && op.z < -12 + 1e-6 && pz < -12 + 1e-6)
                atBottom += std::hypot(op.x - px, op.y - py);
            if (op.kind == c2d::Op::Feed || op.kind == c2d::Op::Rapid || op.kind == c2d::Op::Arc) {
                px = op.x; py = op.y; pz = op.z;
            }
        }
        const double loop = 2 * (40 + 20) + 2 * M_PI * 3.175;
        check(g.done.size() == 1 && atBottom > 0 && atBottom < loop - 1.0,
              "ramped cutout with tabs: the last pass leaves the tabs standing");
    }

    // --- feed rate 0 is refused, not posted as F0 --------------------------
    {
        Rig r;
        r.addShape(c2d::Element::makeRectangle({30, 20}, 40, 20, layer));
        r.addToolpath("contour", {{"speeds", QJsonObject{{"feedrate", 0}, {"plungerate", 300},
                                                         {"rpm", 10000}}}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        check(g.done.isEmpty() && g.skipped.size() == 1 && !g.gcode.contains(QLatin1String("F0")),
              "a toolpath with feed 0 is skipped");
    }

    // --- comments: parentheses in a name do not end the comment early -------
    {
        QVector<c2d::Op> ops;
        ops.append(c2d::Op::comment(QStringLiteral("Pocket (12)")));
        const QString out = c2d::GrblPost(true).generate(ops);
        check(out.contains(QLatin1String("(Pocket  12 )")) && !out.contains(QLatin1String("(12)")),
              "toolpath names with ( ) make a valid G-code comment");
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

    // --- pocket: a big pocket at a fine stepover is cleared to the middle.
    // The ring fill stopped at a fixed 500 rings without a word; this one
    // needs about 680.
    {
        Rig r;
        r.addShape(c2d::Element::makeRectangle({100, 100}, 140, 140, layer));
        r.addToolpath("pocket_toolpath", {{"stepover", 0.1},
                                          {"tool", QJsonObject{{"diameter", 3.175},
                                                               {"number", 201}}}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        double best = 1e9;
        for (const c2d::Op &op : g.ops)
            if (op.kind == c2d::Op::Feed)
                best = std::min(best, std::hypot(op.x - 100, op.y - 100));
        std::fprintf(stderr, "fine pocket: nearest cut to the centre %.3f mm\n", best);
        check(g.done.size() == 1 && g.notes.isEmpty(), "fine pocket exports without a note");
        check(best < 0.3, "fine pocket: the rings reach the centre");
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

    // --- rest depth: a pocket inside an earlier pocket starts at its floor ----
    {
        auto levels = [](const QString &gcode) {
            QList<double> zs;
            for (const CutPoint &p : cutPoints(gcode)) {
                bool seen = false;
                for (double z : zs)
                    if (approx(z, p.z, 1e-3))
                        seen = true;
                if (!seen)
                    zs << p.z;
            }
            std::sort(zs.begin(), zs.end());
            return zs;
        };
        auto has = [](const QList<double> &zs, double z) {
            for (double v : zs)
                if (std::fabs(v - z) < 1e-3)
                    return true;
            return false;
        };
        auto rig = [&](QPointF innerCentre, const QJsonObject &innerExtra) {
            Rig r;
            r.addShape(c2d::Element::makeRectangle({30, 20}, 40, 20, layer));
            r.addToolpath("pocket_toolpath", {{"stepover", 3.0}, {"end_depth", QStringLiteral("5.000")},
                                              {"stepdown", 2.5}, {"name", QStringLiteral("A")}});
            r.addShape(c2d::Element::makeRectangle(innerCentre, 20, 10, layer));
            QJsonObject extra{{"stepover", 3.0}, {"end_depth", QStringLiteral("10.000")},
                              {"stepdown", 2.5}, {"uuid", QStringLiteral("{tp-2}")},
                              {"name", QStringLiteral("B")}};
            for (const QString &k : innerExtra.keys())
                extra.insert(k, innerExtra.value(k));
            r.addToolpath("pocket_toolpath", extra);
            return c2d::exportGcode(r.doc);
        };
        // Without rest_auto: B cuts from the top, 2.5 at a time.
        c2d::GcodeResult g = rig({30, 20}, {});
        QList<double> zs = levels(g.gcode);
        check(g.done.size() == 2 && has(zs, -7.5) && !has(zs, -6.5), "no rest: inner pocket starts at the top");
        // With it: A reaches 5, B starts 1 mm above that floor: 4, then 6.5, 9, 10.
        g = rig({30, 20}, {{"rest_auto", true}, {"rest_overlap", 1.0}});
        zs = levels(g.gcode);
        std::fprintf(stderr, "rest depth levels:");
        for (double z : zs)
            std::fprintf(stderr, " %.2f", z);
        std::fprintf(stderr, "\n  notes: %s\n  skipped: %s\n", qPrintable(g.notes.join("|")), qPrintable(g.skipped.join("|")));
        check(g.done.size() == 2 && has(zs, -6.5) && has(zs, -9.0) && has(zs, -10.0) && !has(zs, -7.5),
              "rest depth: inner pocket starts at the outer pocket's floor minus the overlap");
        check(g.notes.size() == 1 && g.notes.first().startsWith(QStringLiteral("B: starts at 4.00 mm")),
              "rest depth is reported");
        // Only partly inside the first pocket: B keeps its own passes, but
        // down to A's floor they only cut the part A did not clear.
        const double fullB = c2d::computeStats(rig({45, 20}, {}).ops).cutLen;
        g = rig({45, 20}, {{"rest_auto", true}});
        zs = levels(g.gcode);
        const double restB = c2d::computeStats(g.ops).cutLen;
        std::fprintf(stderr, "partial rest: cut %.1f mm (without %.1f mm), notes: %s\n", restB, fullB,
                     qPrintable(g.notes.join("|")));
        check(g.done.size() == 2 && has(zs, -7.5) && has(zs, -10.0) && !has(zs, -6.5),
              "partly covered: B keeps its own depth passes");
        check(g.notes.size() == 1 && g.notes.first().contains(QStringLiteral("depth band")),
              "partial coverage reported");
        // A alone (B already cut to A's floor and left out) gives B's share.
        const double onlyA = c2d::computeStats(
            rig({30, 20}, {{"rest_auto", true}, {"rest_overlap", 0.0}, {"end_depth", QStringLiteral("5.000")}}).ops).cutLen;
        std::fprintf(stderr, "  B alone: %.1f mm with rest, %.1f mm without\n", restB - onlyA, fullB - onlyA);
        check(restB - onlyA < (fullB - onlyA) * 0.9, "partly covered: the area already cleared is not cut again");
        // As deep as A and mostly inside it: only the new strip is cut.
        const c2d::GcodeResult sd = rig({42, 20}, {{"rest_auto", true}, {"end_depth", QStringLiteral("5.000")}});
        const double sameDepth = c2d::computeStats(sd.ops).cutLen - onlyA;
        const double sameDepthFull = c2d::computeStats(rig({42, 20}, {{"end_depth", QStringLiteral("5.000")}}).ops).cutLen - onlyA;
        std::fprintf(stderr, "  mostly inside, same depth: B %.1f mm with rest, %.1f mm without; %s\n", sameDepth,
                     sameDepthFull, qPrintable(sd.notes.join("|")));
        check(sameDepth < sameDepthFull * 0.4, "mostly inside at the same depth: only the new strip is cut");
        bool uncovered = false;   // ...but B's own part beyond A is cut from the top
        for (const CutPoint &p : cutPoints(g.gcode))
            if (approx(p.z, -2.5, 1e-3) && p.x > 50.5)
                uncovered = true;
        check(uncovered, "partly covered: the new area is cut from the top");
        // The same for an adaptive B.
        auto adaptiveRig = [&](bool rest) {
            Rig r;
            r.addShape(c2d::Element::makeRectangle({30, 20}, 40, 20, layer));
            r.addToolpath("pocket_toolpath", {{"stepover", 3.0}, {"end_depth", QStringLiteral("5.000")},
                                              {"stepdown", 2.5}, {"name", QStringLiteral("A")}});
            r.addShape(c2d::Element::makeRectangle({42, 20}, 30, 16, layer));
            r.addToolpath("adaptive_toolpath", {{"stepover", 1.0}, {"end_depth", QStringLiteral("10.000")},
                                                {"stepdown", 5.0}, {"uuid", QStringLiteral("{tp-2}")},
                                                {"name", QStringLiteral("B")}, {"rest_auto", rest},
                                                {"rest_overlap", 1.0}});
            return c2d::exportGcode(r.doc);
        };
        const c2d::GcodeResult aFull = adaptiveRig(false), aRest = adaptiveRig(true);
        if (aFull.skipped.join("").contains(QStringLiteral("Clipper2"))) {
            std::fprintf(stderr, "adaptive rest: skipped (no Clipper2)\n");
        } else {
            const double af = c2d::computeStats(aFull.ops).cutLen, ar = c2d::computeStats(aRest.ops).cutLen;
            std::fprintf(stderr, "adaptive rest: cut %.1f mm (without %.1f mm), notes: %s\n", ar, af,
                         qPrintable(aRest.notes.join("|")));
            check(aRest.done.size() == 2 && aRest.notes.join("").contains(QStringLiteral("depth band")),
                  "adaptive partly covered: cut in rest bands");
            check(ar < af * 0.9, "adaptive partly covered: cuts less than without rest");
            double minZ;
            cutBounds(aRest.gcode, &minZ);
            check(approx(minZ, -10.0, 1e-3), "adaptive partly covered: still reaches end depth");
        }
        // Already as deep as the toolpaths above: with no overlap there is nothing left.
        g = rig({30, 20}, {{"rest_auto", true}, {"rest_overlap", 0.0}, {"end_depth", QStringLiteral("5.000")}});
        check(g.done.size() == 1 && g.skipped.size() == 1
                  && g.skipped.first().contains(QStringLiteral("already cut to 5.00 mm")),
              "fully cut above: left out");
        // The setup sheet takes each toolpath's time from that same program:
        // B is left out there, so its row says so instead of timing B cut
        // on its own from the top; and the rows add up to the job.
        {
            Rig r;
            r.addShape(c2d::Element::makeRectangle({30, 20}, 40, 20, layer));
            r.addToolpath("pocket_toolpath", {{"stepover", 3.0}, {"end_depth", QStringLiteral("5.000")},
                                              {"stepdown", 2.5}, {"name", QStringLiteral("A")}});
            r.addShape(c2d::Element::makeRectangle({30, 20}, 20, 10, layer));
            r.addToolpath("pocket_toolpath", {{"stepover", 3.0}, {"end_depth", QStringLiteral("5.000")},
                                              {"stepdown", 2.5}, {"uuid", QStringLiteral("{tp-2}")},
                                              {"name", QStringLiteral("B")}, {"rest_auto", true},
                                              {"rest_overlap", 0.0}});
            r.addShape(c2d::Element::makeRectangle({30, 20}, 50, 30, layer));
            r.addToolpath("contour", {{"name", QStringLiteral("C")}, {"ofset_dir", 1},
                                      {"uuid", QStringLiteral("{tp-3}")}});
            const QString html = c2d::setupSheetHtml(r.doc, QStringLiteral("Job"));
            const int at = html.indexOf(QStringLiteral("<td>B</td>"));
            const QString rowB = html.mid(at, html.indexOf(QStringLiteral("</tr>"), at) - at);
            check(at >= 0 && rowB.contains(QStringLiteral("already cut")),
                  "setup sheet: a toolpath rest depth leaves out has no time of its own");
            auto secs = [](const QString &m) {
                const QStringList p = m.split(QLatin1Char(':'));
                return p.size() == 2 ? p.at(0).toInt() * 60 + p.at(1).toInt() : -1;
            };
            const QRegularExpression rowTime(QStringLiteral("<td class=\"n\">(\\d+:\\d\\d)</td></tr>"));
            int sum = 0, rows = 0;
            for (auto it = rowTime.globalMatch(html); it.hasNext();) {
                sum += secs(it.next().captured(1));
                ++rows;
            }
            const QRegularExpressionMatch tot =
                QRegularExpression(QStringLiteral("Estimated time</th><td>(\\d+:\\d\\d) min")).match(html);
            check(rows == 2 && tot.hasMatch() && std::abs(sum - secs(tot.captured(1))) <= rows,
                  "setup sheet: the toolpaths' times add up to the job's");
        }
        // Same with 1 mm overlap: one finishing pass over the floor.
        g = rig({30, 20}, {{"rest_auto", true}, {"rest_overlap", 1.0}, {"end_depth", QStringLiteral("5.000")}});
        zs = levels(g.gcode);
        check(g.done.size() == 2 && g.skipped.isEmpty() && !has(zs, -4.0) && has(zs, -5.0),
              "fully cut above with overlap: one pass at the floor");
    }

    // --- rest depth: a straight pass between two cleared corners still sees
    // the island standing between them -------------------------------------
    {
        Rig r;
        const c2d::Element box = c2d::Element::makeRectangle({50, 20}, 80, 40, layer);
        const c2d::Element island = c2d::Element::makeCircle({50, 20}, 2, layer);
        r.addShape(box);
        r.addShape(island);
        r.addToolpath("pocket_toolpath", {{"stepover", 3.0}, {"end_depth", QStringLiteral("10.000")},
                                          {"stepdown", 5.0}, {"name", QStringLiteral("A")},
                                          {"elements", QJsonArray{QJsonObject{{"uuid", box.id}},
                                                                  QJsonObject{{"uuid", island.id}}}}});
        r.addShape(c2d::Element::makeRectangle({50, 20}, 60, 8, layer));
        r.addToolpath("pocket_toolpath", {{"stepover", 3.0}, {"end_depth", QStringLiteral("12.000")},
                                          {"stepdown", 2.0}, {"uuid", QStringLiteral("{tp-2}")},
                                          {"name", QStringLiteral("B")}, {"rest_auto", true}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        check(!g.notes.join("|").contains(QStringLiteral("B: starts at")),
              "rest depth: a pass across a standing island is not started deep");
    }

    // --- rest depth: a V-bit groove is no flat floor --------------------------
    {
        Rig r;
        r.addShape(c2d::Element::makeRectangle({30, 20}, 40, 20, layer));
        QJsonObject vbit{{"diameter", 12.7}, {"number", 301}, {"angle", 90.0}, {"type", 2}};
        r.addToolpath("pocket_toolpath", {{"stepover", 3.0}, {"end_depth", QStringLiteral("5.000")},
                                          {"stepdown", 5.0}, {"name", QStringLiteral("A")}, {"tool", vbit}});
        r.addShape(c2d::Element::makeRectangle({30, 20}, 20, 10, layer));
        r.addToolpath("pocket_toolpath", {{"stepover", 3.0}, {"end_depth", QStringLiteral("8.000")},
                                          {"stepdown", 2.0}, {"uuid", QStringLiteral("{tp-2}")},
                                          {"name", QStringLiteral("B")}, {"rest_auto", true}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        check(g.notes.join("|").indexOf(QStringLiteral("B: ")) < 0,
              "rest depth: what a V-bit cut is not taken as cleared");
    }

    // --- same area, 10 mm then 3 mm: the 3 mm only cleans the corners -------
    {
        auto rig = [&](bool rest, const QString &depth2) {
            Rig r;
            r.addShape(c2d::Element::makeRectangle({30, 20}, 40, 20, layer));   // 10..50 x 10..30
            r.addToolpath("pocket_toolpath", {{"stepover", 4.0}, {"end_depth", QStringLiteral("3.000")},
                                              {"stepdown", 3.0}, {"name", QStringLiteral("10 mm")},
                                              {"tool", QJsonObject{{"diameter", 10.0}, {"number", 1}}}});
            r.addToolpath("pocket_toolpath", {{"stepover", 1.2}, {"end_depth", depth2},
                                              {"stepdown", 3.0}, {"uuid", QStringLiteral("{tp-2}")},
                                              {"name", QStringLiteral("3 mm")}, {"rest_auto", rest},
                                              {"tool", QJsonObject{{"diameter", 3.0}, {"number", 2}}}});
            return c2d::exportGcode(r.doc);
        };
        // The 3 mm program is everything after its tool change.
        auto smallCuts = [](const c2d::GcodeResult &g) {
            const int at = g.gcode.indexOf(QStringLiteral("T2"));
            return cutPoints(at < 0 ? QString() : g.gcode.mid(at));
        };
        const c2d::GcodeResult g = rig(true, QStringLiteral("3.000"));
        bool middle = false, corner = false;
        for (const CutPoint &p : smallCuts(g)) {
            if (p.x > 17 && p.x < 43 && p.y > 15 && p.y < 25)
                middle = true;
            if (p.x < 14 && p.y < 14)
                corner = true;
        }
        const double withRest = c2d::computeStats(g.ops).cutLen;
        const double without = c2d::computeStats(rig(false, QStringLiteral("3.000")).ops).cutLen;
        // The 10 mm alone: the 3 mm already cut to its floor with no overlap, left out.
        const double big = c2d::computeStats(rig(true, QStringLiteral("0.000")).ops).cutLen;
        std::fprintf(stderr, "10 then 3 mm: 10 mm %.1f mm; 3 mm %.1f mm with rest, %.1f mm without; %s\n", big,
                     withRest - big, without - big, qPrintable(g.notes.join("|")));
        check(corner && !middle, "10 then 3 mm: the 3 mm cuts the corners, not the cleared middle");
        check(withRest - big < (without - big) * 0.4, "10 then 3 mm: far less cutting than a full second pocket");
        // Deeper than the 10 mm: below its floor the 3 mm cuts the whole area.
        bool deepMiddle = false;
        for (const CutPoint &p : smallCuts(rig(true, QStringLiteral("6.000"))))
            if (p.x > 17 && p.x < 43 && p.y > 15 && p.y < 25 && p.z < -3.5)
                deepMiddle = true;
        check(deepMiddle, "10 then 3 mm deeper: the whole area below the 10 mm floor");
    }

    // --- one tool number, two bits: still a tool change -----------------------
    {
        Rig r;
        r.addShape(c2d::Element::makeRectangle({30, 20}, 40, 20, layer));
        r.addToolpath("pocket_toolpath", {{"stepover", 3.0}, {"name", QStringLiteral("big")},
                                          {"tool", QJsonObject{{"diameter", 6.35}, {"number", 1}}}});
        r.addToolpath("pocket_toolpath", {{"stepover", 1.0}, {"name", QStringLiteral("small")},
                                          {"uuid", QStringLiteral("{tp-2}")},
                                          {"tool", QJsonObject{{"diameter", 3.175}, {"number", 1}}}});
        r.addToolpath("pocket_toolpath", {{"stepover", 1.0}, {"name", QStringLiteral("small again")},
                                          {"uuid", QStringLiteral("{tp-3}")},
                                          {"tool", QJsonObject{{"diameter", 3.175}, {"number", 1}}}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        check(g.gcode.count(QStringLiteral("M0 ;T1")) == 2,
              "same number, other bit: a tool change; same bit again: none");
        int toolOps = 0;
        for (const c2d::Op &o : g.ops)
            if (o.kind == c2d::Op::Tool) {
                ++toolOps;
                check(o.x > 0, "the tool change carries the bit's diameter");
            }
        check(toolOps == 2, "two tool changes in the ops");
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
        check(lib.tools().size() >= 12 && lib.materials().size() == 7, "library content");
        check(lib.materialIdForCC(QStringLiteral("PVC")) == QLatin1String("pvc"), "PVC is a material");
        check(lib.byNumber(1040) && lib.byNumber(1040)->kind == QLatin1String("roundover"),
              "the R4 corner-rounding bit is in");
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

    // A notch in a bar: the medial axis runs between the notch's two reflex
    // corners, where the clearance dips well below both end vertices' (2.2
    // and 5.2 mm). Fed straight from vertex to vertex, the cone cut 0.19 mm
    // into the corners; each piece of such an edge now has its own depth.
    {
        c2d::Document doc;
        const QVector<QPointF> pts{{0, 0}, {30, 0}, {30, 10}, {17, 10}, {15, 13.923780136749714}, {13, 10}, {0, 10}};
        const c2d::Element e = c2d::Element::makePath(pts, true, layer);
        doc.addElement(e);
        c2d::Toolpath tp = c2d::makeToolpath(doc, QStringLiteral("advanced_vcarve_toolpath"), {e.id});
        tp.json["start_depth"] = "0.000";
        tp.json["end_depth"] = "9.384";
        const double angle = 57.721149050169402;
        QJsonObject tool = tp.json["tool"].toObject();
        tool["angle"] = angle;
        tool["type"] = 2;
        tool["diameter"] = 12.7;
        tp.json["tool"] = tool;
        doc.addToolpath(tp);
        const c2d::GcodeResult g = c2d::exportGcode(doc);
        const double ht = std::tan(angle / 2 * M_PI / 180);
        const QVector<QPolygonF> rings{QPolygonF(pts)};
        double over = 0;
        forEachCutPoint(g.ops, [&](double x, double y, double z) {
            over = std::max(over, -z * ht - signedRingDist(rings, x, y));
        });
        check(g.done.size() == 1 && over < 0.03, "v-carve between two corners stays inside the walls");
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
        check(marker >= 0 && lines.value(marker + 1) == QLatin1String("G90G21"),
              "absolute mode restored after a tool change (a failed probe can leave G91)");
        const QString after = marker >= 0 ? lines.value(marker + 2) : QString();
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

    // --- face: zig-zag over the stock, past both ends ------------------------
    {
        Rig r;
        r.doc.setParam("width", "100");
        r.doc.setParam("height", "50");
        r.addToolpath("face_toolpath",
                      {{"end_depth", "1.000"}, {"stepdown", 0.5}, {"stepover", 4.0},
                       {"elements", QJsonArray()}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        check(g.done.size() == 1 && g.skipped.isEmpty(), "face exports with no vectors");
        double minZ;
        const QRectF bb = cutBounds(g.gcode, &minZ);
        const double R = 6.35 / 2;
        check(approx(bb.left(), -R - 1, 1e-3) && approx(bb.right(), 100 + R + 1, 1e-3)
              && approx(bb.top(), 0, 1e-3) && approx(bb.bottom(), 50, 1e-3),
              "face runs past both ends and covers edge to edge");
        check(approx(minZ, -1, 1e-3), "face depth");
        // Rows no further apart than the stepover, at both depths.
        QVector<double> ys;
        for (const CutPoint &p : cutPoints(g.gcode))
            if (approx(p.z, -0.5, 1e-6) && !ys.contains(p.y))
                ys.append(p.y);
        std::sort(ys.begin(), ys.end());
        double gap = 0;
        for (int i = 1; i < ys.size(); ++i)
            gap = std::max(gap, ys[i] - ys[i - 1]);
        check(ys.size() >= 14 && gap <= 4 + 1e-6, "face rows never wider than the stepover");
        // Every plunge lands beside the stock, not on it.
        bool plungesOff = true;
        double px = 0, py = 0, pz = 10;
        for (const c2d::Op &op : g.ops) {
            if (op.kind == c2d::Op::Feed && op.z < pz - 1e-9 && approx(op.x, px) && approx(op.y, py)
                && op.x > -R + 1e-6 && op.x < 100 + R - 1e-6)
                plungesOff = false;
            if (op.kind == c2d::Op::Feed || op.kind == c2d::Op::Rapid || op.kind == c2d::Op::Arc) {
                px = op.x; py = op.y; pz = op.z;
            }
        }
        check(plungesOff, "face plunges beside the stock");
    }
    // Face over selected vectors: the box around them, the tool inside it.
    {
        Rig r;
        r.addShape(c2d::Element::makeRectangle({150, 100}, 60, 40, layer));
        r.addToolpath("face_toolpath", {{"end_depth", "1.000"}, {"stepdown", 1.0}, {"stepover", 4.0}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        double minZ;
        const QRectF bb = cutBounds(g.gcode, &minZ);
        const double R = 6.35 / 2;
        check(g.done.size() == 1 && bb.left() >= 120 + R - 1e-3 && bb.right() <= 180 - R + 1e-3
                  && bb.top() >= 80 + R - 1e-3 && bb.bottom() <= 120 - R + 1e-3,
              "face over vectors keeps the tool inside their box");
        check(bb.width() > 60 - 2 * R - 0.1 && bb.height() > 40 - 2 * R - 0.1, "face over vectors covers the box");
    }

    // --- bore: helix down the circle's wall ------------------------------------
    {
        Rig r;
        r.addShape(c2d::Element::makeCircle({20, 20}, 10, layer));
        r.addToolpath("bore_toolpath", {{"end_depth", "5.000"}, {"stepdown", 2.0}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        check(g.done.size() == 1 && g.skipped.isEmpty(), "bore exports");
        const double hr = 10 - 6.35 / 2;
        int arcs = 0;
        bool onWall = true, sinking = true, ccw = true;
        double lastZ = 0, minZ = 0;
        double px = 0, py = 0;
        for (const c2d::Op &op : g.ops) {
            if (op.kind == c2d::Op::Arc) {
                ++arcs;
                onWall = onWall && approx(std::hypot(op.x - 20, op.y - 20), hr, 1e-6)
                         && approx(std::hypot(px + op.ci - 20, py + op.cj - 20), 0, 1e-6);
                sinking = sinking && op.z <= lastZ + 1e-9 && lastZ - op.z <= 1.0 + 1e-9;
                ccw = ccw && !op.cw;
                lastZ = op.z;
                minZ = std::min(minZ, op.z);
            }
            if (op.kind != c2d::Op::Comment && op.kind != c2d::Op::Spindle && op.kind != c2d::Op::Tool) {
                px = op.x; py = op.y;
                if (op.kind != c2d::Op::Arc) lastZ = op.kind == c2d::Op::Rapid ? lastZ : op.z;
            }
        }
        check(arcs == 5 * 2 / 2 + 2, "bore: half turns to 5 mm at 1 mm each, then a flat lap");
        check(onWall, "bore arcs ride the wall, centered on the hole");
        check(sinking && approx(minZ, -5, 1e-9), "bore sinks half the pitch per half turn");
        check(ccw, "bore climbs (counter-clockwise) by default");

        Rig small;
        small.addShape(c2d::Element::makeCircle({0, 0}, 3, layer));
        small.addToolpath("bore_toolpath", {});
        const c2d::GcodeResult gs = c2d::exportGcode(small.doc);
        check(gs.done.isEmpty() && gs.skipped.size() == 1 && gs.skipped.first().contains("no wider"),
              "a hole the tool cannot bore is reported once");
    }

    // --- 2D chamfer: V-bit, offset tip, depth from the angle ------------------
    {
        QJsonObject vbit;
        vbit.insert("diameter", 12.7);
        vbit.insert("number", 301);
        vbit.insert("angle", 90);
        vbit.insert("type", 2);
        for (const bool inside : {false, true}) {
            Rig r;
            r.addShape(c2d::Element::makeRectangle({30, 20}, 40, 20, layer));
            r.addToolpath("chamfer_toolpath",
                          {{"chamfer_width", 1.0}, {"tip_offset", 0.5},
                           {"side", inside ? "inside" : "outside"}, {"stepdown", 5.0},
                           {"tool", vbit}});
            const c2d::GcodeResult g = c2d::exportGcode(r.doc);
            check(g.done.size() == 1 && g.skipped.isEmpty(), "chamfer exports");
            double minZ;
            const QRectF bb = cutBounds(g.gcode, &minZ);
            const double o = inside ? -0.5 : 0.5;
            check(approx(bb.left(), 10 - o, 1e-2) && approx(bb.right(), 50 + o, 1e-2)
                  && approx(bb.top(), 10 - o, 1e-2) && approx(bb.bottom(), 30 + o, 1e-2),
                  inside ? "inside chamfer: tip inset by the offset"
                         : "outside chamfer: tip outset by the offset");
            check(approx(minZ, -1.5, 1e-3), "90-degree bit: depth = width + tip offset");
        }
        Rig flat;
        flat.addShape(c2d::Element::makeRectangle({30, 20}, 40, 20, layer));
        flat.addToolpath("chamfer_toolpath", {});
        const c2d::GcodeResult gf = c2d::exportGcode(flat.doc);
        check(gf.done.isEmpty() && gf.skipped.size() == 1 && gf.skipped.first().contains("V-bit"),
              "chamfer with an end mill is refused");
        // A ball bit with an angle on file is still no V-bit; a bevel deeper
        // than the cone would be cut by the shank.
        QJsonObject ball = vbit;
        ball.insert("type", 1);
        Rig rb;
        rb.addShape(c2d::Element::makeRectangle({30, 20}, 40, 20, layer));
        rb.addToolpath("chamfer_toolpath", {{"tool", ball}});
        check(c2d::exportGcode(rb.doc).done.isEmpty(), "chamfer with a ball bit is refused");
        Rig rw;
        rw.addShape(c2d::Element::makeRectangle({30, 20}, 40, 20, layer));
        rw.addToolpath("chamfer_toolpath", {{"chamfer_width", 6.0}, {"tip_offset", 0.5}, {"tool", vbit}});
        const c2d::GcodeResult gw = c2d::exportGcode(rw.doc);
        check(gw.done.isEmpty() && gw.skipped.size() == 1 && gw.skipped.first().contains("radius"),
              "chamfer wider than the V-bit is refused");
        // V-carve with a flat end mill: refused, not carved past the walls.
        QJsonObject em;
        em.insert("diameter", 6.35);
        em.insert("number", 202);
        em.insert("type", 0);
        Rig rv;
        rv.addShape(c2d::Element::makeRectangle({30, 20}, 40, 20, layer));
        rv.addToolpath("advanced_vcarve_toolpath", {{"tool", em}});
        const c2d::GcodeResult gv = c2d::exportGcode(rv.doc);
        check(gv.done.isEmpty() && gv.skipped.size() == 1 && gv.skipped.first().contains("V-bit"),
              "v-carve with an end mill is refused");
        // V-carve deeper than the cone (90 degrees, 12.7 mm: 6.35 mm) on a
        // shape wider than the bit leaves a lip: refused; within the cone it carves.
        for (const char *end : {"10.000", "6.000"}) {
            Rig rd;
            rd.addShape(c2d::Element::makeRectangle({30, 30}, 40, 40, layer));
            rd.addToolpath("advanced_vcarve_toolpath", {{"start_depth", "0.000"}, {"end_depth", end}, {"tool", vbit}});
            const c2d::GcodeResult gd = c2d::exportGcode(rd.doc);
            const bool deep = QByteArray(end) == "10.000";
            check(deep ? gd.done.isEmpty() && gd.skipped.size() == 1 && gd.skipped.first().contains("cone")
                       : gd.done.size() == 1 && gd.skipped.isEmpty(),
                  deep ? "v-carve deeper than the cone is refused" : "v-carve within the cone carves");
        }
        // An inlay male starts below the top; its flat clearing laps still
        // step down from the stock top, not a full lap at the start depth.
        {
            Rig ri;
            const c2d::Element outer = c2d::Element::makeRectangle({60, 60}, 60, 60, layer);
            const c2d::Element inner = c2d::Element::makeRectangle({60, 60}, 20, 20, layer);
            ri.addShape(outer);
            ri.addShape(inner);
            QJsonObject v60 = vbit;
            v60.insert("angle", 60);
            ri.addToolpath("advanced_vcarve_toolpath", {{"start_depth", "4.500"}, {"end_depth", "5.000"},
                                                        {"stepdown", 1.0}, {"tool", v60},
                                                        {"elements", QJsonArray{QJsonObject{{"uuid", outer.id}},
                                                                                QJsonObject{{"uuid", inner.id}}}}});
            const c2d::GcodeResult gi = c2d::exportGcode(ri.doc);
            bool firstLap = false;
            for (int i = 1; i < gi.ops.size(); ++i) {
                const c2d::Op &o = gi.ops.at(i), &prev = gi.ops.at(i - 1);
                if (o.kind == c2d::Op::Feed && prev.kind == c2d::Op::Rapid && std::fabs(prev.x - o.x) < 1e-9
                    && std::fabs(prev.y - o.y) < 1e-9 && std::fabs(o.z + 1.0) < 1e-6)
                    firstLap = true;
            }
            check(gi.done.size() == 1 && firstLap, "v-carve flat clearing steps down from the stock top");
        }
    }

    // --- adaptive: helix entry, passes repeated per level, inside the pocket --
    {
        Rig r;
        r.addShape(c2d::Element::makeRectangle({40, 30}, 60, 40, layer));
        r.addToolpath("adaptive_toolpath",
                      {{"end_depth", "6.000"}, {"stepdown", 3.0}, {"stepover", 0.6},
                       {"ramp_angle", 3}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        check(g.done.size() == 1 && g.skipped.isEmpty(), "adaptive exports");
        double minZ;
        const QRectF bb = cutBounds(g.gcode, &minZ);
        const double R = 6.35 / 2;
        check(bb.left() >= 10 + R - 1e-3 && bb.right() <= 70 - R + 1e-3
              && bb.top() >= 10 + R - 1e-3 && bb.bottom() <= 50 - R + 1e-3,
              "adaptive keeps the tool inside the pocket");
        check(bb.width() > 60 - 2 * R - 0.1 && bb.height() > 40 - 2 * R - 0.1,
              "adaptive reaches the walls");
        check(approx(minZ, -6, 1e-6), "adaptive depth");
        bool origin = false;
        for (const c2d::Op &op : g.ops) {
            if (op.kind == c2d::Op::Feed || op.kind == c2d::Op::Arc)
                break;
            origin = origin || (op.kind == c2d::Op::Rapid && std::fabs(op.x) < 1e-9 && std::fabs(op.y) < 1e-9);
        }
        check(!origin, "adaptive does not rapid to X0 Y0 before its first cut");
        // Two levels, each entered on a helix that sinks gently.
        int helixArcs = 0;
        bool gentle = true;
        double pz = 0;
        QVector<double> levels;
        for (const c2d::Op &op : g.ops) {
            if (op.kind == c2d::Op::Arc && op.z < pz - 1e-9) {
                ++helixArcs;
                gentle = gentle && pz - op.z < 1.0;
            }
            if (op.kind == c2d::Op::Feed || op.kind == c2d::Op::Arc) {
                if (op.z < -1e-9 && !levels.contains(op.z) && (op.z == -3 || op.z == -6))
                    levels.append(op.z);
            }
            if (op.kind == c2d::Op::Feed || op.kind == c2d::Op::Rapid || op.kind == c2d::Op::Arc)
                pz = op.z;
        }
        check(helixArcs >= 2 * 3 && gentle, "adaptive enters each level on a gentle helix");
        check(levels.size() == 2, "adaptive cuts at both levels");
        // No straight plunge deeper than the hop clearance above the last floor.
        bool softPlunges = true;
        double px = 0, py = 0;
        pz = 10;
        for (const c2d::Op &op : g.ops) {
            if (op.kind == c2d::Op::Feed && approx(op.x, px) && approx(op.y, py) && op.z < pz - 1e-9)
                softPlunges = softPlunges && pz - op.z <= 3.0 + 1.0 + 1e-6;
            if (op.kind == c2d::Op::Feed || op.kind == c2d::Op::Rapid || op.kind == c2d::Op::Arc) {
                px = op.x; py = op.y; pz = op.z;
            }
        }
        check(softPlunges, "adaptive never plunges more than a level into material");

        Rig narrow;
        narrow.addShape(c2d::Element::makeRectangle({40, 30}, 60, 5, layer));
        narrow.addToolpath("adaptive_toolpath", {});
        const c2d::GcodeResult gn = c2d::exportGcode(narrow.doc);
        check(gn.done.isEmpty() && gn.skipped.size() == 1, "adaptive in a slot narrower than the tool is reported once");
    }

    // --- contour lead-in / lead-out: arcs on the air side ------------------------
    {
        const double R = 6.35 / 2;
        for (const int dir : {1, -1}) {   // outside, inside
            Rig r;
            r.addShape(c2d::Element::makeRectangle({40, 30}, 40, 20, layer));
            r.addToolpath("contour", {{"ofset_dir", dir}, {"lead_radius", 3.0},
                                      {"end_depth", "4.000"}, {"stepdown", 2.0}});
            const c2d::GcodeResult g = c2d::exportGcode(r.doc);
            check(g.done.size() == 1 && g.skipped.isEmpty(), "lead contour exports");
            // Every cut point keeps the tool off the finished edge, and each
            // pass drops in at the lead's start, clear of the cut.
            auto edgeDist = [&](double x, double y) {
                if (dir > 0)
                    return std::hypot(std::max({20 - x, x - 60, 0.0}), std::max({20 - y, y - 40, 0.0}));
                return std::min({x - 20, 60 - x, y - 20, 40 - y});
            };
            bool clear = true;
            for (const CutPoint &p : cutPoints(g.gcode))
                if (p.z < -1e-6)
                    clear = clear && edgeDist(p.x, p.y) >= R - 1e-3;
            check(clear, dir > 0 ? "outside leads stay off the part" : "inside leads stay in the hole");
            int plunges = 0, offRing = 0;
            double px = 0, py = 0, pz = 10;
            for (const c2d::Op &op : g.ops) {
                if (op.kind == c2d::Op::Feed && approx(op.x, px) && approx(op.y, py) && op.z < pz - 1e-9) {
                    ++plunges;
                    offRing += edgeDist(op.x, op.y) > R + 0.5 ? 1 : 0;
                }
                if (op.kind == c2d::Op::Feed || op.kind == c2d::Op::Rapid || op.kind == c2d::Op::Arc) {
                    px = op.x; py = op.y; pz = op.z;
                }
            }
            check(plunges == 2 && offRing == 2, "each pass drops in at the lead's start, off the cut");
        }
        // Ramping wins: no lead arcs, same as before.
        Rig a, b;
        for (Rig *r : {&a, &b})
            r->addShape(c2d::Element::makeRectangle({40, 30}, 40, 20, layer));
        a.addToolpath("contour", {{"ofset_dir", 1}, {"enable_ramping", true}});
        b.addToolpath("contour", {{"ofset_dir", 1}, {"enable_ramping", true}, {"lead_radius", 3.0}});
        check(c2d::exportGcode(a.doc).gcode == c2d::exportGcode(b.doc).gcode,
              "ramping replaces the leads");
    }

    // --- thread milling: helix of one pitch per turn ------------------------------
    {
        const double R = 6.35 / 2;
        for (const bool external : {false, true}) {
            Rig r;
            r.addShape(c2d::Element::makeCircle({30, 30}, 8, layer));
            r.addToolpath("thread_toolpath",
                          {{"end_depth", "6.000"}, {"pitch", 1.5},
                           {"side", external ? "external" : "internal"}});
            const c2d::GcodeResult g = c2d::exportGcode(r.doc);
            check(g.done.size() == 1 && g.skipped.isEmpty(), "thread exports");
            const double td = 0.6134 * 1.5;
            const double wallR = external ? 8 + R - td : 8 - R;
            int arcs = 0;
            bool onWall = true, steady = true, rising = true, ccw = true;
            double lastZ = 0, minZ = 0, maxZ = -100;
            for (const c2d::Op &op : g.ops) {
                if (op.kind == c2d::Op::Arc) {
                    if (arcs > 0) {
                        steady = steady && approx(std::fabs(op.z - lastZ), 0.75, 1e-9);
                        rising = rising && op.z > lastZ;
                    }
                    ++arcs;
                    onWall = onWall && approx(std::hypot(op.x - 30, op.y - 30), wallR, 1e-6);
                    ccw = ccw && !op.cw;
                    minZ = std::min(minZ, op.z);
                    maxZ = std::max(maxZ, op.z);
                    lastZ = op.z;
                }
            }
            check(arcs == 8 && onWall, external ? "external thread: 4 turns on the minor diameter"
                                                : "internal thread: 4 turns on the major diameter");
            check(steady, "thread: half the pitch per half turn");
            // Right-hand climb: internal goes up counter-clockwise, external
            // down clockwise; neither goes below the end depth.
            check(external ? (!rising && !ccw) : (rising && ccw), "right-hand climb direction");
            check(minZ >= -6 - 1e-9 && approx(maxZ - minZ, 5.25, 1e-9) && (external ? approx(minZ, -6) : approx(maxZ, 0)), "thread covers the length, never deeper");
        }
        // Started below the top (the lower part of a boss, a hole under a
        // counterbore): whole turns carried the helix up to a pitch above
        // the start, into stock. It ends at the start on a part turn and
        // leaves the wall radially.
        for (const bool external : {false, true}) {
            Rig r;
            r.addShape(c2d::Element::makeCircle({30, 30}, 8, layer));
            r.addToolpath("thread_toolpath",
                          {{"start_depth", "2.000"}, {"end_depth", "6.000"}, {"pitch", 1.5},
                           {"side", external ? "external" : "internal"}});
            const c2d::GcodeResult g = c2d::exportGcode(r.doc);
            check(g.done.size() == 1, "thread below the top exports");
            double minZ = 0, maxZ = -100, pz = 50;
            QPointF prev;
            bool arcBefore = false, radialOut = true;
            int exits = 0;
            for (const c2d::Op &op : g.ops) {
                if (op.kind == c2d::Op::Arc) {
                    // An arc runs from where the tool was to its end.
                    minZ = std::min({minZ, op.z, pz});
                    maxZ = std::max({maxZ, op.z, pz});
                } else if (op.kind == c2d::Op::Feed && arcBefore) {
                    // The move off the wall keeps the angle it ended at.
                    const QPointF a = prev - QPointF(30, 30), b = QPointF(op.x, op.y) - QPointF(30, 30);
                    const double cross = a.x() * b.y() - a.y() * b.x();
                    radialOut = radialOut && std::fabs(cross) < 1e-6 * (1 + std::hypot(b.x(), b.y()))
                                && approx(op.z, pz, 1e-9);
                    ++exits;
                }
                arcBefore = op.kind == c2d::Op::Arc;
                if (op.kind == c2d::Op::Feed || op.kind == c2d::Op::Rapid || op.kind == c2d::Op::Arc) {
                    prev = QPointF(op.x, op.y);
                    pz = op.z;
                }
            }
            check(approx(minZ, -6, 1e-9) && approx(maxZ, -2, 1e-9),
                  "thread below the top: the helix runs from the end depth to the start, no higher");
            check(exits >= 1 && radialOut, "thread below the top: it leaves the wall radially");
        }
        Rig small;
        small.addShape(c2d::Element::makeCircle({0, 0}, 3, layer));
        small.addToolpath("thread_toolpath", {{"end_depth", "5.000"}, {"pitch", 1.0}});
        const c2d::GcodeResult gs = c2d::exportGcode(small.doc);
        check(gs.done.isEmpty() && gs.skipped.size() == 1 && gs.skipped.first().contains("too small"),
              "a hole too small to thread is reported once");
    }

    // --- construction geometry and sketch points ---------------------------------
    {
        Rig r;
        const c2d::Element box = c2d::Element::makeRectangle({40, 30}, 40, 20, layer);
        const c2d::Element guide = c2d::Element::withConstruction(
            c2d::Element::makeRectangle({40, 30}, 80, 60, layer), true);
        const c2d::Element pt = c2d::Element::makePoint({40, 30}, layer);
        r.doc.addElement(box);
        r.doc.addElement(guide);
        r.doc.addElement(pt);
        check(c2d::Element::isConstruction(guide) && !c2d::Element::isConstruction(box),
              "construction flag");
        check(c2d::Element::isPoint(pt) && !c2d::Element::isPoint(box), "point flag");
        check(c2d::Element::isConstruction(c2d::Element::regen(guide, {{"width", 70.0}})),
              "construction survives a resize");
        QJsonArray all;
        for (const c2d::Element *e : {&box, &guide, &pt})
            all.append(QJsonObject{{"uuid", e->id}});
        r.addToolpath("pocket_toolpath", {{"elements", all}, {"stepover", 2.0}});
        c2d::GcodeResult g = c2d::exportGcode(r.doc);
        double minZ;
        QRectF bb = cutBounds(g.gcode, &minZ);
        check(g.done.size() == 1 && bb.width() < 40 && bb.height() < 20,
              "pocket machines the box, not the construction frame or the point");
        Rig d;
        d.doc.addElement(guide);
        d.doc.addElement(pt);
        d.addToolpath("drilling_toolpath", {{"elements", all}});
        g = c2d::exportGcode(d.doc);
        int plunges = 0;
        bool atPoint = true;
        for (const CutPoint &p : cutPoints(g.gcode))
            if (p.z < -1e-6) {
                ++plunges;
                atPoint = atPoint && approx(p.x, 40, 1e-6) && approx(p.y, 30, 1e-6);
            }
        check(g.done.size() == 1 && plunges >= 1 && atPoint, "drilling drills the point only");
        Rig only;
        only.addShape(guide);
        only.addToolpath("contour", {});
        g = c2d::exportGcode(only.doc);
        check(g.done.isEmpty() && g.skipped.size() == 1 && g.skipped.first().contains("no vectors"),
              "a toolpath on construction geometry alone is reported, not cut");
    }

    // --- contour finishing passes and spring pass ----------------------------------
    {
        auto contourWidthAt = [&](const QJsonObject &extra, double z, int *nAtBottom) {
            Rig r;
            r.addShape(c2d::Element::makeRectangle({50, 40}, 40, 20, layer));
            QJsonObject x{{"end_depth", "2.000"}, {"stepdown", 1.0}, {"ofset_dir", 1},
                          {"lead_radius", 0}};
            for (const QString &k : extra.keys())
                x.insert(k, extra.value(k));
            r.addToolpath("contour", x);
            const c2d::GcodeResult g = c2d::exportGcode(r.doc);
            double lo = 1e30, hi = -1e30;
            *nAtBottom = 0;
            for (const CutPoint &p : cutPoints(g.gcode)) {
                if (approx(p.z, z, 1e-6)) {
                    lo = std::min(lo, p.x);
                    hi = std::max(hi, p.x);
                }
                if (approx(p.z, -2, 1e-6))
                    ++*nAtBottom;
            }
            return hi - lo;
        };
        int nPlain = 0, nFin = 0, nSpring = 0, dummy = 0;
        const double plain = contourWidthAt({}, -2, &nPlain);
        const double rough = contourWidthAt({{"finish_passes", 2}, {"finish_stepover", 0.5}}, -1, &nFin);
        const double last = contourWidthAt({{"finish_passes", 2}, {"finish_stepover", 0.5}}, -2, &dummy);
        contourWidthAt({{"finish_passes", 2}, {"finish_stepover", 0.5}, {"spring_pass", true}}, -2, &nSpring);
        check(approx(std::fabs(rough - plain), 2.0, 0.05),
              "finishing: the roughing ring stays passes x stepover off the wall");
        check(approx(last, std::max(plain, rough), 0.05) && nFin > nPlain,
              "finishing: extra rings at the final depth");
        check(nSpring > nFin, "spring pass repeats the last ring");
    }

    // Two parts 8 mm apart: their roughing offsets merge into one ring round
    // both, their final rings do not. Matched by one point, both loops took
    // the merged ring as their own roughing, and one stepped across from it
    // to its final ring straight through the other part at full depth. Such
    // a loop is cut as before, without finishing rings.
    for (const double lead : {3.175, 0.0}) {
        c2d::Document doc;
        const c2d::Element a = c2d::Element::makeRectangle({20, 20}, 30, 10, layer);   // x 5..35
        const c2d::Element b = c2d::Element::makeRectangle({48, 20}, 10, 40, layer);   // x 43..53
        doc.addElement(a);
        doc.addElement(b);
        c2d::Toolpath tp = c2d::makeToolpath(doc, QStringLiteral("contour"), {a.id, b.id});
        tp.json["ofset_dir"] = 1;
        tp.json["start_depth"] = "0.000";
        tp.json["end_depth"] = "3.000";
        tp.json["stepdown"] = 3.0;
        QJsonObject tool = tp.json["tool"].toObject();
        tool["diameter"] = 6.35;
        tp.json["tool"] = tool;
        tp.json["finish_passes"] = 2;
        tp.json["finish_stepover"] = 1.0;
        tp.json["ignore_tabs"] = true;
        tp.json["lead_radius"] = lead;
        doc.addToolpath(tp);
        const c2d::GcodeResult g = c2d::exportGcode(doc);
        const QVector<QPolygonF> parts{QPolygonF(QRectF(5, 15, 30, 10)), QPolygonF(QRectF(43, 0, 10, 40))};
        double worst = 1e30;
        forEachCutPoint(g.ops, [&](double x, double y, double) {
            for (const QPolygonF &part : parts)
                worst = std::min(worst, -signedRingDist({part}, x, y));
        });
        check(g.done.size() == 1 && worst >= 6.35 / 2 - 0.05,
              lead > 0 ? "finishing never steps across a neighbouring part (leads)"
                       : "finishing never steps across a neighbouring part");
    }

    // Lead-in larger than the tool in a hole with islands: the feed back
    // from the lead's end to its start between passes runs straight across
    // the lead circle, and that chord was never checked - it took the tool
    // centre 0.175 mm into an island, a 1.25 mm gouge.
    {
        c2d::Document doc;
        const QVector<QPolygonF> rings{
            QPolygonF({QPointF(75.240, 20.558), QPointF(74.057, 52.491), QPointF(75.203, 79.873),
                       QPointF(42.686, 87.084), QPointF(35.232, 58.598), QPointF(27.651, 39.653),
                       QPointF(40.143, 12.950)}),
            QPolygonF({QPointF(47.484, 46.301), QPointF(47.497, 44.117), QPointF(48.709, 42.564),
                       QPointF(50.389, 43.525), QPointF(51.490, 44.605), QPointF(51.174, 46.991),
                       QPointF(49.025, 47.554)}),
            QPolygonF({QPointF(63.144, 50.589), QPointF(59.666, 52.945), QPointF(56.755, 49.979),
                       QPointF(60.023, 46.199), QPointF(62.540, 47.504)})};
        QJsonArray els;
        for (const QPolygonF &ring : rings) {
            const c2d::Element e = c2d::Element::makePath(QVector<QPointF>(ring.begin(), ring.end()), true, layer);
            doc.addElement(e);
            els.append(QJsonObject{{"uuid", e.id}});
        }
        c2d::Toolpath tp = c2d::makeToolpath(doc, QStringLiteral("contour"), {});
        tp.json["elements"] = els;
        tp.json["ofset_dir"] = -1;
        tp.json["start_depth"] = "0.000";
        tp.json["end_depth"] = "5.711";
        tp.json["stepdown"] = 3.606213057502436;
        tp.json["stock_to_leave"] = 0.0;
        tp.json["climb"] = false;
        QJsonObject tool = tp.json["tool"].toObject();
        tool["diameter"] = 2.1538238049284382;
        tool["type"] = 0;
        tool["angle"] = 0;
        tool.remove("corner_radius");
        tp.json["tool"] = tool;
        tp.json["lead_radius"] = 5.093207597666928;
        doc.addToolpath(tp);
        const c2d::GcodeResult g = c2d::exportGcode(doc);
        double worst = 1e30;
        forEachCutPoint(g.ops, [&](double x, double y, double) {
            worst = std::min(worst, signedRingDist(rings, x, y));
        });
        check(g.done.size() == 1 && worst >= 2.1538238049284382 / 2 - 0.06,
              "a lead's step back between passes stays off the islands");
    }

    // --- helical pocket entry ------------------------------------------------------
    {
        auto pocketOps = [&](bool helix) {
            Rig r;
            r.addShape(c2d::Element::makeRectangle({50, 40}, 40, 30, layer));
            r.addToolpath("pocket_toolpath", {{"end_depth", "2.000"}, {"stepdown", 1.0},
                                              {"stepover", 2.5}, {"helix_entry", helix},
                                              {"helix_diameter", 6.35}, {"helix_angle", 2}});
            return c2d::exportGcode(r.doc);
        };
        auto deepestPlunge = [](const c2d::GcodeResult &g) {
            double px = 0, py = 0, pz = 0, deepest = 0;
            for (const c2d::Op &op : g.ops) {
                if (op.kind != c2d::Op::Rapid && op.kind != c2d::Op::Feed && op.kind != c2d::Op::Arc)
                    continue;
                if (op.kind == c2d::Op::Feed && approx(op.x, px, 1e-9) && approx(op.y, py, 1e-9)
                    && op.z < pz - 1e-9)
                    deepest = std::min(deepest, op.z);
                px = op.x; py = op.y; pz = op.z;
            }
            return deepest;
        };
        const c2d::GcodeResult h = pocketOps(true), s = pocketOps(false);
        check(h.done.size() == 1 && h.skipped.isEmpty(), "helix pocket exports");
        check(deepestPlunge(s) < -1 - 1e-6, "a plain pocket plunges straight to depth");
        check(deepestPlunge(h) >= -1 - 1e-6,
              "helix entry: straight drops only to levels already cut");
        int down = 0;
        bool inside = true;
        double pz = 0;
        for (const c2d::Op &op : h.ops) {
            if (op.kind == c2d::Op::Arc) {
                if (op.z < pz - 1e-9)
                    ++down;
                inside = inside && op.x > 30 + 3.17 && op.x < 70 - 3.17 && op.y > 25 + 3.17 && op.y < 55 - 3.17;
            }
            if (op.kind == c2d::Op::Rapid || op.kind == c2d::Op::Feed || op.kind == c2d::Op::Arc)
                pz = op.z;
        }
        check(down >= 4 && inside, "helix entry: descending arcs inside the pocket");
    }

    // --- drilling dwell and chip breaking -----------------------------------------
    {
        Rig r;
        r.addShape(c2d::Element::makeCircle({20, 20}, 2, layer));
        r.addToolpath("drilling_toolpath", {{"end_depth", "6.000"}, {"peck_distance", 2.0},
                                            {"dwell", 0.5}, {"chip_break", true},
                                            {"chip_retract", 0.3}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        check(g.done.size() == 1 && g.gcode.contains("G4P0.50"), "drilling: dwell at the bottom");
        int dwells = 0, backoffs = 0;
        double maxBetween = -100;
        bool started = false;
        for (const c2d::Op &op : g.ops) {
            if (op.kind == c2d::Op::Dwell)
                ++dwells;
            if (op.kind == c2d::Op::Feed && op.z < -1e-6)
                started = true;
            if (started && op.kind == c2d::Op::Rapid && op.z < -1e-6) {
                ++backoffs;
                maxBetween = std::max(maxBetween, op.z);
            }
        }
        check(dwells == 1, "drilling: one dwell per hole");
        check(backoffs == 2 && maxBetween < -1.5,
              "chip breaking: back off a little between pecks, staying in the hole");
    }

    // --- 2D slot ------------------------------------------------------------------
    {
        const double R = 6.35 / 2;
        Rig r;
        r.addShape(c2d::Element::makeBezierPath(c2d::sketch::slot({20, 30}, {60, 30}, 6.35), layer));
        r.addToolpath("slot_toolpath", {{"end_depth", "3.000"}, {"stepdown", 1.0}});
        c2d::GcodeResult g = c2d::exportGcode(r.doc);
        check(g.done.size() == 1 && g.skipped.isEmpty(), "slot exports");
        double minZ;
        QRectF bb = cutBounds(g.gcode, &minZ);
        check(approx(bb.left(), 20, 0.01) && approx(bb.right(), 60, 0.01) && approx(bb.height(), 0, 0.01)
              && approx(minZ, -3), "slot: a tool-width slot runs its centreline to depth");
        bool ramps = true;
        double px = 0, pz = 0;
        for (const CutPoint &p : cutPoints(g.gcode)) {
            if (p.z < pz - 1e-6 && approx(p.x, px, 1e-6))
                ramps = false;   // a straight plunge below the top
            px = p.x;
            pz = p.z;
        }
        check(ramps, "slot: every depth is ramped along the slot");

        Rig w;
        w.addShape(c2d::Element::makeBezierPath(c2d::sketch::slot({20, 30}, {60, 30}, 10), layer));
        w.addToolpath("slot_toolpath", {{"end_depth", "1.000"}, {"stepdown", 1.0}});
        g = c2d::exportGcode(w.doc);
        bb = cutBounds(g.gcode, &minZ);
        check(g.done.size() == 1 && approx(bb.height(), 10 - 2 * R, 0.02)
              && approx(bb.left(), 15 + R, 0.1), "slot: a wider slot also cleans its walls");
        // (left edge: arc moves only report their endpoints, which sit
        // near, not on, the end's leftmost point)

        Rig bad;
        bad.addShape(c2d::Element::makeRectangle({40, 30}, 40, 20, layer));
        bad.addShape(c2d::Element::makeBezierPath(c2d::sketch::slot({20, 60}, {60, 60}, 4), layer));
        QJsonArray both;
        for (const c2d::Element &e : bad.doc.elements())
            both.append(QJsonObject{{"uuid", e.id}});
        bad.addToolpath("slot_toolpath", {{"elements", both}});
        g = c2d::exportGcode(bad.doc);
        check(g.done.isEmpty() && g.skipped.size() == 2
                  && g.skipped.join(' ').contains("not slots")
                  && g.skipped.join(' ').contains("narrower than the tool"),
              "slot: rectangles and too-narrow slots are reported");
    }

    // --- circular pocket and boss -------------------------------------------------
    {
        const double R = 6.35 / 2;
        for (const bool outside : {false, true}) {
            Rig r;
            r.addShape(c2d::Element::makeCircle({40, 40}, 12, layer));
            r.addToolpath("circular_toolpath", {{"end_depth", "2.000"}, {"stepdown", 1.0},
                                                {"side", outside ? "outside" : "inside"},
                                                {"stepover", 2.5}, {"boss_clearance", 5.0}});
            const c2d::GcodeResult g = c2d::exportGcode(r.doc);
            check(g.done.size() == 1 && g.skipped.isEmpty(), "circular exports");
            double px = 0, py = 0, rMin = 1e9, rMax = 0;
            bool ccwAll = true, cwAll = true, centred = true;
            for (const c2d::Op &op : g.ops) {
                if (op.kind == c2d::Op::Arc) {
                    centred = centred && approx(px + op.ci, 40, 1e-6) && approx(py + op.cj, 40, 1e-6);
                    const double rr = std::hypot(op.x - 40, op.y - 40);
                    rMin = std::min(rMin, rr);
                    rMax = std::max(rMax, rr);
                    ccwAll = ccwAll && !op.cw;
                    cwAll = cwAll && op.cw;
                }
                if (op.kind == c2d::Op::Rapid || op.kind == c2d::Op::Feed || op.kind == c2d::Op::Arc) {
                    px = op.x;
                    py = op.y;
                }
            }
            check(centred, "circular: every lap is round the circle's centre");
            if (outside)
                check(approx(rMin, 12 + R, 1e-6) && approx(rMax, 12 + R + 5, 1e-6) && cwAll,
                      "circular boss: clockwise laps from the clearance in to the wall");
            else
                check(approx(rMax, 12 - R, 1e-6) && rMin <= 2.5 + 1e-6 && ccwAll,
                      "circular pocket: counter-clockwise laps out to the wall");
        }
        // A boss is entered on a helix round its outermost lap: a plunge
        // beside it went straight down into stock nothing had cut.
        {
            Rig r;
            r.addShape(c2d::Element::makeCircle({40, 40}, 12, layer));
            r.addToolpath("circular_toolpath", {{"end_depth", "3.000"}, {"stepdown", 1.0},
                                                {"side", "outside"}, {"stepover", 2.5},
                                                {"boss_clearance", 5.0}});
            const c2d::GcodeResult g = c2d::exportGcode(r.doc);
            double px = 0, py = 0, pz = 50, rOut = 0;
            int plunges = 0;
            for (const c2d::Op &op : g.ops) {
                if (op.kind == c2d::Op::Arc)
                    rOut = std::max(rOut, std::hypot(op.x - 40, op.y - 40));
                if (op.kind == c2d::Op::Feed && op.z < pz - 1e-9 && op.z < -1e-9
                    && approx(op.x, px, 1e-9) && approx(op.y, py, 1e-9))
                    ++plunges;
                if (op.kind == c2d::Op::Rapid || op.kind == c2d::Op::Feed || op.kind == c2d::Op::Arc) {
                    px = op.x;
                    py = op.y;
                    pz = op.z;
                }
            }
            check(g.done.size() == 1 && plunges == 0, "circular boss: no plunge into the stock");
            check(approx(rOut, 12 + R + 5, 1e-6), "circular boss: the helix stays on the outermost lap");
        }
        // Bosses side by side: the laps round one stop short of the other,
        // and one the tool cannot pass beside is skipped with a note.
        {
            const double gap = 2 * R + 3;
            Rig r;
            const c2d::Element a = c2d::Element::makeCircle({40, 40}, 12, layer),
                               b = c2d::Element::makeCircle({40 + 24 + gap, 40}, 12, layer);
            r.addShape(a);
            r.addShape(b);
            r.addToolpath("circular_toolpath", {{"end_depth", "3.000"}, {"stepdown", 1.5},
                                                {"side", "outside"}, {"stepover", 2.5},
                                                {"elements", QJsonArray{QJsonObject{{"uuid", a.id}},
                                                                        QJsonObject{{"uuid", b.id}}}}});
            const c2d::GcodeResult g = c2d::exportGcode(r.doc);
            double nearest = 1e9;
            for (const c2d::Op &op : g.ops)
                if ((op.kind == c2d::Op::Arc || op.kind == c2d::Op::Feed) && op.z < -1e-9)
                    nearest = std::min({nearest, std::hypot(op.x - 40, op.y - 40),
                                        std::hypot(op.x - 40 - 24 - gap, op.y - 40)});
            check(g.done.size() == 1 && nearest >= 12 + R - 1e-6, "circular bosses: no lap cuts the other boss");
            Rig t;
            const c2d::Element c = c2d::Element::makeCircle({40, 40}, 12, layer),
                               e = c2d::Element::makeCircle({40 + 24 + R, 40}, 12, layer);
            t.addShape(c);
            t.addShape(e);
            t.addToolpath("circular_toolpath", {{"end_depth", "3.000"}, {"side", "outside"},
                                                {"elements", QJsonArray{QJsonObject{{"uuid", c.id}},
                                                                        QJsonObject{{"uuid", e.id}}}}});
            const c2d::GcodeResult gt = c2d::exportGcode(t.doc);
            check(!gt.skipped.isEmpty() && gt.skipped.first().contains("too close"),
                  "circular bosses: closer than the tool is reported");
        }
        Rig small;
        small.addShape(c2d::Element::makeCircle({0, 0}, 3, layer));
        small.addToolpath("circular_toolpath", {});
        const c2d::GcodeResult gs = c2d::exportGcode(small.doc);
        check(gs.done.isEmpty() && gs.skipped.size() == 1 && gs.skipped.first().contains("no wider"),
              "circular: a hole no wider than the tool is reported");
    }

    // --- setup sheet ---------------------------------------------------------------
    {
        Rig r;
        r.doc.setParam("width", "100");
        r.doc.setParam("height", "60");
        r.doc.setParam("thickness", "12");
        r.addShape(c2d::Element::makeRectangle({40, 30}, 40, 20, layer));
        r.addToolpath("contour", {{"name", "Outline <cut>"}, {"ofset_dir", 1}});
        const QString html = c2d::setupSheetHtml(r.doc, QStringLiteral("Job"));
        check(html.contains("100.00 × 60.00 × 12.00 mm"), "setup sheet: stock size");
        check(html.contains("Outline &lt;cut&gt;") && !html.contains("<cut>"),
              "setup sheet: toolpath names, escaped");
        check(html.contains("T201") && html.contains("6.350 mm"), "setup sheet: the tool");
        check(html.contains("Estimated time"), "setup sheet: job totals");

        // A "%3" in a name is text: the chained .arg() filled it with the
        // next column's value.
        Rig p;
        p.addShape(c2d::Element::makeRectangle({40, 30}, 40, 20, layer));
        p.addToolpath("contour", {{"name", "Cut 50%3 deep"}, {"ofset_dir", 1}});
        check(c2d::setupSheetHtml(p.doc, QStringLiteral("Job")).contains("Cut 50%3 deep"),
              "setup sheet: a % placeholder in a name is kept as written");
    }

    // --- the factory makes all four with working defaults --------------------
    {
        c2d::Document doc;
        doc.setParam("width", "200");
        doc.setParam("height", "100");
        doc.setParam("thickness", "12");
        const c2d::Element hole = c2d::Element::makeCircle({50, 50}, 8, layer);
        const c2d::Element box = c2d::Element::makeRectangle({120, 50}, 40, 30, layer);
        doc.addElement(hole);
        doc.addElement(box);
        doc.addToolpath(c2d::makeToolpath(doc, QStringLiteral("face_toolpath"), {}));
        doc.addToolpath(c2d::makeToolpath(doc, QStringLiteral("bore_toolpath"), {hole.id}));
        doc.addToolpath(c2d::makeToolpath(doc, QStringLiteral("chamfer_toolpath"), {box.id}));
        doc.addToolpath(c2d::makeToolpath(doc, QStringLiteral("adaptive_toolpath"), {box.id}));
        doc.addToolpath(c2d::makeToolpath(doc, QStringLiteral("thread_toolpath"), {hole.id}));
        doc.addToolpath(c2d::makeToolpath(doc, QStringLiteral("contour"), {box.id}));
        const c2d::Element slot = c2d::Element::makeBezierPath(
            c2d::sketch::slot({30, 20}, {80, 20}, 8), layer);
        doc.addElement(slot);
        doc.addToolpath(c2d::makeToolpath(doc, QStringLiteral("slot_toolpath"), {slot.id}));
        doc.addToolpath(c2d::makeToolpath(doc, QStringLiteral("circular_toolpath"), {hole.id}));
        doc.addToolpath(c2d::makeToolpath(doc, QStringLiteral("pocket_toolpath"), {box.id}));
        doc.addToolpath(c2d::makeToolpath(doc, QStringLiteral("drilling_toolpath"), {hole.id}));
        check(c2d::toolpathLabel("chamfer_toolpath") == QLatin1String("2D Chamfer"),
              "2D Chamfer is in the New menu");
        check(c2d::toolpathLabel("slot_toolpath") == QLatin1String("2D Slot")
                  && c2d::toolpathLabel("circular_toolpath") == QLatin1String("Circular"),
              "2D Slot and Circular are in the New menu");
        const c2d::GcodeResult g = c2d::exportGcode(doc);
        check(g.done.size() == 10 && g.skipped.isEmpty(),
              "face, bore, chamfer, adaptive, thread, lead contour, slot, circular, "
              "helix pocket and drilling defaults all export");
        const c2d::Toolpath ch = doc.toolpaths().at(2);
        check(ch.json.value("tool").toObject().value("angle").toDouble() > 0,
              "chamfer gets a V-bit from the library");
    }

    // --- stay-down links never cross an island ------------------------------
    // A 2 mm island splits the 12.7 mm bit's pocket into two lobes 25 mm
    // apart, under linkDist (25.4): the link fed straight across the island
    // at every depth. It must retract instead, and an ordinary pocket must
    // keep every link it had.
    {
        Rig r;
        const c2d::Element outer = c2d::Element::makeRectangle({25, 20}, 50, 40, layer);
        const c2d::Element island = c2d::Element::makeRectangle({30, 20}, 2, 30, layer);
        r.doc.addElement(outer);
        r.doc.addElement(island);
        for (const bool helix : {false, true}) {
            Rig h;
            h.doc = r.doc;
            h.addToolpath("pocket_toolpath",
                          {{"stepover", 4.0}, {"end_depth", "6"}, {"stepdown", 3.0},
                           {"helix_entry", helix},
                           {"tool", QJsonObject{{"diameter", 12.7}, {"number", 1}}},
                           {"elements", QJsonArray{QJsonObject{{"uuid", outer.id}},
                                                   QJsonObject{{"uuid", island.id}}}}});
            const c2d::GcodeResult g = c2d::exportGcode(h.doc);
            check(g.done.size() == 1, "pocket round an island exports");
            check(closestFeedTo(g.ops, QRectF(29, 5, 2, 30)) > 6.35 - 0.05,
                  helix ? "helix pocket: no link feeds across the island"
                        : "pocket: no link feeds across the island");
        }
        for (const int mode : {0, 1, 2}) {
            Rig p;
            p.addShape(c2d::Element::makeRectangle({30, 20}, 60, 40, layer));
            QJsonObject extra{{"stepover", 2.5}, {"end_depth", "6"}, {"stepdown", 2.0}};
            if (mode == 1)
                extra.insert("enable_ramping", true);
            if (mode == 2)
                extra.insert("helix_entry", false);
            p.addToolpath("pocket_toolpath", extra);
            const c2d::GcodeResult g = c2d::exportGcode(p.doc);
            check(g.done.size() == 1 && retractsFromCut(g.ops) == 1,
                  "an ordinary pocket keeps its stay-down links (one retract, at the end)");
        }
    }

    // --- 3D rough: links stay out of a hole in the boundary -----------------
    {
        c2d::HeightModel m;
        m.cell = 0.5;
        m.baseZ = -6;
        m.resize(100, 80);   // 50 x 40 mm, no model: the floor at -6 everywhere
        Rig r;
        const c2d::Element outer = c2d::Element::makeRectangle({25, 20}, 50, 40, layer);
        const c2d::Element hole = c2d::Element::makeRectangle({30, 20}, 2, 30, layer);
        r.doc.addElement(outer);
        r.doc.addElement(hole);
        QJsonObject extra;
        const QJsonObject def = c2d::defaultRoughToolpathJson();
        for (const QString &k : def.keys())
            if (k != "elements" && k != "uuid" && k != "name")
                extra.insert(k, def.value(k));
        extra.insert("tool", QJsonObject{{"number", 1}, {"diameter", 12.7}, {"type", 0}});
        extra.insert("stepover", 4.0);
        extra.insert("stock_to_leave", 0.0);
        extra.insert("stepdown", 3.0);
        extra.insert("elements", QJsonArray{QJsonObject{{"uuid", outer.id}},
                                            QJsonObject{{"uuid", hole.id}}});
        r.addToolpath("3d_rough_toolpath", extra);
        const c2d::GcodeResult g = c2d::exportGcode(r.doc, nullptr, &m);
        check(g.done.size() == 1, "3D rough with a hole in its boundary exports");
        check(closestFeedTo(g.ops, QRectF(29, 5, 2, 30)) > 6.35 - 0.05,
              "3D rough: no link feeds across a hole in the boundary");

        // An unreadable max depth is refused, not read as "to the floor".
        QJsonObject e2 = extra;
        e2.insert("max_depth", QStringLiteral("5,0"));
        e2.insert("uuid", QStringLiteral("{tp-2}"));
        c2d::Document d2;
        for (const c2d::Element &e : {outer, hole})
            d2.addElement(e);
        Rig b2;
        b2.doc = d2;
        b2.addToolpath("3d_rough_toolpath", e2);
        const c2d::GcodeResult g2 = c2d::exportGcode(b2.doc, nullptr, &m);
        check(g2.done.isEmpty() && g2.skipped.size() == 1
                  && g2.skipped.first().contains(QLatin1String("max depth")),
              "3D rough: an unreadable max depth is skipped");
        bool ok = true;
        c2d::cam3dParams(QJsonObject{{"max_depth", QStringLiteral("5mm")}}, 3, &ok);
        check(!ok, "cam3dParams: \"5mm\" is not a max depth");
        const c2d::Cam3dParams p3 =
            c2d::cam3dParams(QJsonObject{{"max_depth", QStringLiteral("2.000")}}, 3, &ok);
        check(ok && approx(p3.maxDepth, -2.0), "cam3dParams: a readable max depth is read");
        c2d::cam3dParams(QJsonObject{}, 3, &ok);
        check(ok, "cam3dParams: no max depth is fine");
    }

    // --- rest depth samples the contour's lead arcs -------------------------
    // The second contour's ring lies in what the first cleared, but its lead
    // arcs reach out into stock never cut: it must not start lower down.
    {
        Rig r;
        r.addShape(c2d::Element::makeCircle({50, 50}, 20, layer));
        r.addToolpath("contour", {{"ofset_dir", 1}, {"end_depth", "10"}, {"stepdown", 2.0}});
        QJsonObject second{{"ofset_dir", 1}, {"end_depth", "12"}, {"stepdown", 2.0},
                           {"tool", QJsonObject{{"diameter", 3.175}, {"number", 2}}},
                           {"rest_auto", true}, {"rest_overlap", 1.0}, {"lead_radius", 5.0},
                           {"name", QStringLiteral("t1")}, {"uuid", QStringLiteral("{tp-2}")}};
        r.addToolpath("contour", second);
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        check(g.done.size() == 2, "rest contour with leads exports");
        bool in = false, found = false;
        double firstZ = 0;
        for (const c2d::Op &o : g.ops) {
            if (o.kind == c2d::Op::Comment)
                in = o.text == QLatin1String("t1");
            if (in && o.kind == c2d::Op::Feed && o.z < 0) {
                firstZ = o.z;
                found = true;
                break;
            }
        }
        check(found && firstZ > -2.0 - 1e-6,
              "rest contour: lead arcs in uncut stock keep the first pass at the top");
    }

    // --- site order: a straight dado listed before its part's cutout ---------
    // A horizontal line's box has no height; it still overlaps the part, so
    // the earlier dado is cut before the cutout frees the part.
    {
        Rig r;
        r.doc.setParam("site_order", "1");
        const c2d::Element part = c2d::Element::makeRectangle({100, 60}, 200, 100, layer);
        const c2d::Element dado = c2d::Element::makePath({QPointF(205, 60), QPointF(-5, 60)}, false, layer);
        r.addShape(part);
        r.doc.addElement(dado);
        r.shapeId = dado.id;
        r.addToolpath("contour", {{"ofset_dir", 0}, {"end_depth", "6.000"}, {"name", "dado"}, {"uuid", "{tp-d}"}});
        r.shapeId = part.id;
        r.addToolpath("cutout", {{"cut_depth", "18.000"}, {"depth_per_pass", 2.0}, {"break_through", "0.200"},
                                 {"name", "cutout"}, {"uuid", "{tp-c}"}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        QStringList seen;
        for (const c2d::Op &o : g.ops)
            if (o.kind == c2d::Op::Comment && (o.text == QLatin1String("dado") || o.text == QLatin1String("cutout"))
                && !seen.contains(o.text))
                seen << o.text;
        check(seen == QStringList({"dado", "cutout"}), "site order: a straight dado runs before the cutout");
    }

    // --- spindle speed 0, a bad tool diameter, start below end: skipped -----
    {
        auto skippedWith = [&](const char *type, const QJsonObject &extra, const char *msg) {
            Rig r;
            if (QLatin1String(type) == QLatin1String("slot_toolpath"))
                r.addShape(c2d::Element::makePath({{10, 10}, {40, 10}}, false, layer));
            else
                r.addShape(c2d::Element::makeCircle({20, 20}, 10, layer));
            r.addToolpath(type, extra);
            const c2d::GcodeResult g = c2d::exportGcode(r.doc);
            return g.done.isEmpty() && g.skipped.size() == 1
                   && g.skipped.first().contains(QLatin1String(msg))
                   && !g.gcode.contains(QLatin1String("G1"));
        };
        check(skippedWith("pocket_toolpath",
                          {{"speeds", QJsonObject{{"feedrate", 1000}, {"plungerate", 300},
                                                  {"rpm", 0}}}},
                          "spindle speed must be above 0"),
              "a toolpath at 0 rpm is skipped");
        for (const QJsonValue &dia : {QJsonValue(0.0), QJsonValue(-6.35), QJsonValue()}) {
            QJsonObject tool{{"number", 3}};
            if (!dia.isUndefined())
                tool.insert("diameter", dia);
            check(skippedWith("pocket_toolpath", {{"tool", tool}}, "tool diameter must be above 0"),
                  "a zero, negative or missing tool diameter is skipped");
        }
        for (const char *type : {"bore_toolpath", "circular_toolpath", "slot_toolpath",
                                 "pocket_toolpath", "drilling_toolpath", "contour"})
            check(skippedWith(type, {{"start_depth", "5"}, {"end_depth", "2"}, {"peck_distance", 1.0}},
                              "start depth is below end depth"),
                  "a start depth below the end depth is skipped, whatever the type");
    }

    // --- a ramp that runs out of laps plunges the rest ---------------------
    // The innermost ring of a R5 pocket at a 1.5 stepover is ~0.3 mm round:
    // 20 laps at 3 degrees do not reach the depth, and the remainder used to
    // go down on the next move at the cutting feed.
    {
        Rig r;
        r.addShape(c2d::Element::makeCircle({20, 20}, 5, layer));
        r.addToolpath("pocket_toolpath", {{"stepover", 1.5}, {"enable_ramping", true},
                                          {"ramp_angle", 3.0}, {"end_depth", "6"}, {"stepdown", 3.0}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        check(g.done.size() == 1, "short-ring ramp pocket exports");
        int steep = 0;
        double cx = 0, cy = 0, cz = 10;
        for (const c2d::Op &o : g.ops) {
            if (o.kind == c2d::Op::Feed || o.kind == c2d::Op::Arc) {
                const double dz = cz - o.z;
                const double xy = std::hypot(o.x - cx, o.y - cy);
                if (dz > 0.05 && o.z < -0.01 && o.feed > 300 + 1e-6 && dz > xy)
                    ++steep;
            }
            if (o.kind == c2d::Op::Rapid || o.kind == c2d::Op::Feed || o.kind == c2d::Op::Arc) {
                cx = o.x;
                cy = o.y;
                cz = o.z;
            }
        }
        check(steep == 0, "ramp: no steep descent at the cutting feed");
    }

    // Tabs were spaced by length along the tool-centre ring. The round
    // corner arc of an outside ring all sits against one corner point of
    // the part, and with a lead-in (the ring starting mid-edge) every tab of
    // this square landed on one: nothing held the part. Tabs are measured
    // on the part's own outline now.
    for (const double lead : {0.0, 2.0}) {
        Rig r;
        r.addShape(c2d::Element::makeRectangle({50, 50}, 40, 40, layer));   // 30..70
        r.addToolpath("contour", {{"ofset_dir", 1}, {"end_depth", "9.000"}, {"stepdown", 3.0},
                                  {"ignore_tabs", false}, {"tab_height", 3.0}, {"tab_width", 4.0},
                                  {"lead_radius", lead}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        const QVector<double> holds = tabHolds(g.ops, QPolygonF(QRectF(30, 30, 40, 40)), 6.35 / 2, -6.0);
        double total = 0;
        for (const double h : holds)
            total += h;
        std::fprintf(stderr, "square tabs, lead %.0f: %d piece(s), %.2f mm of edge held\n",
                     lead, int(holds.size()), total);
        // 4 tabs on a 160 mm outline; the reach of 0.01 mm takes 0.25 mm
        // off either end of each.
        check(g.done.size() == 1 && total > 4 * (4.0 - 0.8) && total < 4 * 4.0 + 0.5,
              lead > 0 ? "tabs hold their width of the part's edge (lead-in)"
                       : "tabs hold their width of the part's edge");
    }

    // Finishing rings lifted for the tab width along their own length: in a
    // hole they are longer than the roughing ring and cut the sides off the
    // tabs it had left (5.6 mm of wall per tab down to 3.1). Each tab is
    // kept as its two ends now, projected onto every ring.
    {
        QVector<double> totals;
        QPolygonF hole;
        for (int k = 0; k <= 96; ++k)
            hole << QPointF(50 + 10 * std::cos(k * M_PI / 48), 50 + 10 * std::sin(k * M_PI / 48));
        for (const int nFin : {0, 3}) {
            Rig r;
            r.addShape(c2d::Element::makeCircle({50, 50}, 10, layer));
            const QJsonObject tool{{"diameter", 3.175}, {"number", 201}};
            r.addToolpath("contour", {{"ofset_dir", -1}, {"end_depth", "9.000"}, {"stepdown", 3.0},
                                      {"tool", tool}, {"ignore_tabs", false}, {"tab_height", 3.0},
                                      {"tab_width", 6.0}, {"finish_passes", nFin},
                                      {"finish_stepover", 1.0}});
            const c2d::GcodeResult g = c2d::exportGcode(r.doc);
            double total = 0;
            for (const double h : tabHolds(g.ops, hole, 3.175 / 2, -6.0))
                total += h;
            totals << total;
        }
        std::fprintf(stderr, "hole tabs: %.2f mm of wall held, %.2f with 3 finishing passes\n",
                     totals.at(0), totals.at(1));
        check(totals.at(0) > 3 * 6.0 - 1.5, "hole tabs hold their width of the wall");
        check(std::fabs(totals.at(1) - totals.at(0)) < 0.5, "finishing passes keep the tabs' width");
    }

    // Pocket, contour, cutout and V-carve took an open path as a polygon
    // closed by its ends and cut a shape that was never drawn. They leave it
    // out now and say so; engraving and a contour on the line follow it.
    {
        const c2d::Element open = c2d::Element::makePath({{10, 10}, {60, 10}, {60, 60}}, false, layer);
        const c2d::Element closed = c2d::Element::makeRectangle({100, 100}, 20, 20, layer);
        const QList<QPair<const char *, QJsonObject>> refused{
            {"pocket_toolpath", {}}, {"contour", {{"ofset_dir", 1}}}, {"contour", {{"ofset_dir", -1}}},
            {"cutout", {{"cut_depth", 3.0}}}, {"advanced_vcarve_toolpath", {}}};
        for (const auto &t : refused) {
            Rig r;
            r.addShape(open);
            r.addToolpath(t.first, t.second);
            const c2d::GcodeResult g = c2d::exportGcode(r.doc);
            int cuts = 0;
            forEachCutPoint(g.ops, [&](double, double, double) { ++cuts; });
            check(g.done.isEmpty() && g.skipped.join(" ").contains("open vectors ignored") && cuts == 0,
                  "an open path alone is not pocketed, contoured or carved");
            // With a closed vector beside it: that one is cut, the open one
            // left out, with a note.
            Rig both;
            both.addShape(open);
            both.addShape(closed);
            QJsonObject extra = t.second;
            extra.insert("elements", QJsonArray{QJsonObject{{"uuid", open.id}}, QJsonObject{{"uuid", closed.id}}});
            both.addToolpath(t.first, extra);
            const c2d::GcodeResult g2 = c2d::exportGcode(both.doc);
            bool nearOpen = false;
            forEachCutPoint(g2.ops, [&](double x, double y, double) { nearOpen = nearOpen || (x < 70 && y < 70); });
            check(g2.done.size() == 1 && g2.notes.join(" ").contains("1 open vector(s) ignored") && !nearOpen,
                  "an open path beside a closed one is left out, with a note");
        }
        for (const auto &t : QList<QPair<const char *, QJsonObject>>{
                 {"engrave_toolpath", {}}, {"contour", {{"ofset_dir", 0}}}}) {
            Rig r;
            r.addShape(open);
            r.addToolpath(t.first, t.second);
            const c2d::GcodeResult g = c2d::exportGcode(r.doc);
            check(g.done.size() == 1, "engraving and a contour on the line still follow an open path");
        }
    }

    // A finishing pass asked for and not cut said nothing. And the check
    // that a finishing ring stays parallel all along only allowed for a
    // sharp corner made at one vertex: Clipper rounds the cusp of this "R"
    // (DejaVu Sans, 22.2 mm) over a few micro-segments, so its outline lost
    // the pass. The offsets of its counter vanish: skipped, with a note.
    {
        const QVector<QVector<QPointF>> rings{
            {             {20.615, 18.175}, {20.992, 18.010}, {21.364, 17.772}, {21.731, 17.461}, {22.093, 17.077},
             {22.452, 16.617}, {22.812, 16.078}, {23.173, 15.461}, {23.535, 14.765}, {25.929, 10.000},
             {23.395, 10.000}, {21.164, 14.473}, {20.735, 15.275}, {20.313, 15.930}, {19.898, 16.437},
             {19.692, 16.635}, {19.488, 16.797}, {19.046, 17.047}, {18.529, 17.226}, {17.939, 17.333},
             {17.275, 17.369}, {14.706, 17.369}, {14.706, 10.000}, {12.347, 10.000}, {12.347, 27.435},
             {17.672, 27.435}, {18.396, 27.416}, {19.072, 27.357}, {19.701, 27.260}, {20.283, 27.123},
             {20.816, 26.947}, {21.303, 26.732}, {21.742, 26.479}, {22.134, 26.186}, {22.478, 25.854},
             {22.777, 25.481}, {23.030, 25.070}, {23.237, 24.618}, {23.398, 24.127}, {23.513, 23.595},
             {23.582, 23.025}, {23.605, 22.414}, {23.593, 22.011}, {23.557, 21.626}, {23.497, 21.258},
             {23.414, 20.907}, {23.306, 20.574}, {23.175, 20.259}, {23.019, 19.961}, {22.840, 19.681},
             {22.638, 19.420}, {22.414, 19.180}, {22.169, 18.960}, {21.901, 18.761}, {21.612, 18.584},
             {21.302, 18.426}, {20.969, 18.290}, {20.615, 18.175}},
            {             {14.706, 25.497}, {14.706, 19.307}, {17.672, 19.307}, {18.086, 19.320}, {18.473, 19.357},
             {18.834, 19.418}, {19.169, 19.504}, {19.478, 19.615}, {19.760, 19.751}, {20.017, 19.911},
             {20.248, 20.096}, {20.451, 20.304}, {20.628, 20.536}, {20.778, 20.791}, {20.900, 21.069},
             {20.995, 21.371}, {21.063, 21.695}, {21.104, 22.043}, {21.118, 22.414}, {21.104, 22.784},
             {21.063, 23.132}, {20.995, 23.455}, {20.900, 23.755}, {20.778, 24.032}, {20.628, 24.285},
             {20.451, 24.514}, {20.248, 24.720}, {20.017, 24.902}, {19.760, 25.060}, {19.478, 25.193},
             {19.169, 25.303}, {18.834, 25.388}, {18.473, 25.448}, {18.086, 25.485}, {17.672, 25.497},
             {14.706, 25.497}}};
        QString notes[2];
        int ops[2];
        for (const int nFin : {0, 1}) {
            c2d::Document doc;
            QJsonArray els;
            for (const QVector<QPointF> &ring : rings) {
                const c2d::Element e = c2d::Element::makePath(ring, true, layer);
                doc.addElement(e);
                els.append(QJsonObject{{"uuid", e.id}});
            }
            c2d::Toolpath tp = c2d::makeToolpath(doc, QStringLiteral("contour"), {});
            tp.json["elements"] = els;
            tp.json["ofset_dir"] = 1;
            tp.json["start_depth"] = "0.000";
            tp.json["end_depth"] = "3.000";
            tp.json["stepdown"] = 3.0;
            QJsonObject tool = tp.json["tool"].toObject();
            tool["diameter"] = 6.03;
            tp.json["tool"] = tool;
            tp.json["finish_passes"] = nFin;
            tp.json["finish_stepover"] = 0.5;
            tp.json["ignore_tabs"] = true;
            tp.json["lead_radius"] = 0.0;
            doc.addToolpath(tp);
            const c2d::GcodeResult g = c2d::exportGcode(doc);
            notes[nFin] = g.notes.join("|");
            ops[nFin] = g.ops.size();
        }
        std::fprintf(stderr, "R finishing: %d ops without, %d with; notes [%s]\n", ops[0], ops[1],
                     qPrintable(notes[1]));
        check(ops[1] > ops[0], "the R's outline gets its finishing pass");
        check(notes[1].contains("finishing passes skipped on 1 loop(s)"),
              "finishing passes skipped are reported");
    }

    // A hole with a narrow arm, finished inside: the corner reading walked
    // round the arm's tip and back down its other side, read the two sides
    // as a corner of almost no angle and let a rough ring that had lost the
    // arm stand 20 mm off it. The arm was then cut by the finishing rings
    // alone, at full depth in one pass. It must be cut at every stepdown.
    {
        for (const double w : {7.5, 8.5}) {
            c2d::Document doc;
            const double x0 = 20 - w / 2, x1 = 20 + w / 2;
            const c2d::Element e = c2d::Element::makePath(
                {{0, 0}, {40, 0}, {40, 40}, {x1, 40}, {x1, 60}, {x0, 60}, {x0, 40}, {0, 40}}, true, layer);
            doc.addElement(e);
            c2d::Toolpath tp = c2d::makeToolpath(doc, QStringLiteral("contour"), {e.id});
            tp.json["ofset_dir"] = -1;
            tp.json["start_depth"] = "0.000";
            tp.json["end_depth"] = "10.000";
            tp.json["stepdown"] = 2.0;
            QJsonObject tool = tp.json["tool"].toObject();
            tool["diameter"] = 6.0;
            tool["type"] = 0;
            tp.json["tool"] = tool;
            tp.json["finish_passes"] = 2;
            tp.json["finish_stepover"] = 0.5;
            tp.json["spring_pass"] = false;
            tp.json["ignore_tabs"] = true;
            tp.json["lead_radius"] = 0.0;
            doc.addToolpath(tp);
            const c2d::GcodeResult g = c2d::exportGcode(doc);
            QSet<int> armDepths;
            for (const c2d::Op &op : g.ops)
                if (op.kind == c2d::Op::Feed && op.z < -1e-6 && op.y > 45)
                    armDepths.insert(int(std::lround(-op.z)));
            check(g.done.size() == 1 && armDepths == QSet<int>({2, 4, 6, 8, 10}),
                  "finishing a hole with a narrow arm: the arm is cut at every stepdown");
        }
    }

    // Texture fills an area: an open vector closed by its end points
    // textured a triangle that was never drawn.
    {
        c2d::Document doc;
        const c2d::Element v = c2d::Element::makePath({{10, 10}, {60, 60}, {110, 10}}, false, layer);
        doc.addElement(v);
        c2d::Toolpath tp = c2d::makeToolpath(doc, QStringLiteral("texture_toolpath"), {v.id});
        doc.addToolpath(tp);
        const c2d::GcodeResult g = c2d::exportGcode(doc);
        check(g.done.isEmpty() && g.skipped.size() == 1 && g.skipped.first().contains("needs closed vectors"),
              "texture: an open vector is not taken as an area");
        const c2d::Element box = c2d::Element::makeRectangle({60, 80}, 20, 20, layer);
        doc.addElement(box);
        tp.json["elements"] = QJsonArray{QJsonObject{{"uuid", v.id}}, QJsonObject{{"uuid", box.id}}};
        doc.removeToolpath(tp.uuid);
        doc.addToolpath(tp);
        const c2d::GcodeResult g2 = c2d::exportGcode(doc);
        bool inBox = true;
        for (const c2d::Op &op : g2.ops)
            if (op.kind == c2d::Op::Feed && op.z < -1e-6)
                inBox = inBox && op.x > 49.9 && op.x < 70.1 && op.y > 69.9 && op.y < 90.1;
        check(g2.done.size() == 1 && g2.notes.join(' ').contains("1 open vector(s) ignored") && inBox,
              "texture: the open vector is left out with a note, the closed one textured");
    }

    std::printf("OK: %d checks passed\n", g_checks);
    return 0;
}
