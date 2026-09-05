// Headless checks for toolpath tiling (src/tiling.cpp): a synthetic
// three-tile job is split, every tile's motion must stay inside
// [0, tileH], nothing may be lost or cut twice, and each program must be
// complete. Plain asserts, exits 0 on success.

#include "../src/c2ddocument.h"
#include "../src/element.h"
#include "../src/gcodeexport.h"
#include "../src/post_grbl.h"
#include "../src/tiling.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>

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

// XY cutting length of an op list (feeds + arcs, Z-only moves excluded).
static double xyCutLength(const QVector<c2d::Op> &ops)
{
    double len = 0, px = 0, py = 0;
    for (const c2d::Op &op : ops) {
        if (op.kind == c2d::Op::Feed) {
            len += std::hypot(op.x - px, op.y - py);
        } else if (op.kind == c2d::Op::Arc) {
            const double cx = px + op.ci, cy = py + op.cj, r = std::hypot(op.ci, op.cj);
            const double a0 = std::atan2(py - cy, px - cx), a1 = std::atan2(op.y - cy, op.x - cx);
            double sweep = op.cw ? a0 - a1 : a1 - a0;
            while (sweep <= 1e-9) sweep += 2 * M_PI;
            while (sweep > 2 * M_PI) sweep -= 2 * M_PI;
            len += r * sweep;
        }
        if (op.kind == c2d::Op::Feed || op.kind == c2d::Op::Arc || op.kind == c2d::Op::Rapid) {
            px = op.x;
            py = op.y;
        }
    }
    return len;
}

static void yRange(const QVector<c2d::Op> &ops, double *lo, double *hi)
{
    *lo = 1e30;
    *hi = -1e30;
    double px = 0, py = 0;
    for (const c2d::Op &op : ops) {
        if (op.kind == c2d::Op::Arc) {
            const double cx = px + op.ci, cy = py + op.cj, r = std::hypot(op.ci, op.cj);
            for (int i = 0; i <= 64; ++i) {
                const double a0 = std::atan2(py - cy, px - cx), a1 = std::atan2(op.y - cy, op.x - cx);
                double sweep = op.cw ? a0 - a1 : a1 - a0;
                while (sweep <= 1e-9) sweep += 2 * M_PI;
                while (sweep > 2 * M_PI) sweep -= 2 * M_PI;
                const double a = a0 + (op.cw ? -sweep : sweep) * i / 64.0;
                *lo = std::fmin(*lo, cy + r * std::sin(a));
                *hi = std::fmax(*hi, cy + r * std::sin(a));
            }
        }
        if (op.kind == c2d::Op::Feed || op.kind == c2d::Op::Arc || op.kind == c2d::Op::Rapid) {
            *lo = std::fmin(*lo, op.y);
            *hi = std::fmax(*hi, op.y);
            px = op.x;
            py = op.y;
        }
    }
}

// Y range of the motion in a g-code text (modal post: bare X/Y/Z lines).
static void gcodeYRange(const QString &gcode, double *lo, double *hi)
{
    static const QRegularExpression yWord(QStringLiteral("Y(-?[0-9]+\\.?[0-9]*)"));
    *lo = 1e30;
    *hi = -1e30;
    for (const QString &line : gcode.split(QChar('\n'))) {
        if (line.startsWith(QChar('(')) || line.startsWith(QChar(';')))
            continue;
        const QRegularExpressionMatch m = yWord.match(line);
        if (m.hasMatch()) {
            const double y = m.captured(1).toDouble();
            *lo = std::fmin(*lo, y);
            *hi = std::fmax(*hi, y);
        }
    }
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const double tileH = 100.0;
    const double safeZ = 3.0;

    // --- synthetic op list spanning three tiles ----------------------------
    // A tall rectangle outline, a full circle straddling the 100 boundary, a
    // line crossing both boundaries, and a peck-drill at Y = 250.
    QVector<c2d::Op> ops;
    ops << c2d::Op::tool(201) << c2d::Op::comment(QStringLiteral("frame"))
        << c2d::Op::spindle(18000);
    ops << c2d::Op::rapid(10, 5, safeZ) << c2d::Op::feedTo(10, 5, -1, 300)
        << c2d::Op::feedTo(60, 5, -1, 1000) << c2d::Op::feedTo(60, 295, -1, 1000)
        << c2d::Op::feedTo(10, 295, -1, 1000) << c2d::Op::feedTo(10, 5, -1, 1000)
        << c2d::Op::rapid(10, 5, safeZ);
    // circle centre (35,100) r 10, start at (45,100): two G2 half arcs
    ops << c2d::Op::rapid(45, 100, safeZ) << c2d::Op::feedTo(45, 100, -2, 300)
        << c2d::Op::arcTo(25, 100, -2, -10, 0, true, 800)
        << c2d::Op::arcTo(45, 100, -2, 10, 0, true, 800)
        << c2d::Op::rapid(45, 100, safeZ);
    ops << c2d::Op::rapid(20, 50, safeZ) << c2d::Op::feedTo(20, 50, -1.5, 300)
        << c2d::Op::feedTo(50, 250, -1.5, 900) << c2d::Op::rapid(50, 250, safeZ);
    ops << c2d::Op::rapid(30, 250, safeZ) << c2d::Op::feedTo(30, 250, -2, 200)
        << c2d::Op::rapid(30, 250, safeZ) << c2d::Op::feedTo(30, 250, -4, 200)
        << c2d::Op::rapid(30, 250, safeZ);
    ops << c2d::Op::spindle(0);

    check(c2d::tileCount(ops, tileH) == 3, "three tiles");
    const QVector<QVector<c2d::Op>> tiles = c2d::tileOps(ops, tileH, safeZ);
    check(tiles.size() == 3, "tileOps returns three tiles");

    // A toolpath whose cuts all land in one tile must not leave its tool
    // change and spindle start behind in the tiles it never reaches.
    {
        QVector<c2d::Op> j;
        j.append(c2d::Op::tool(201));
        j.append(c2d::Op::comment(QStringLiteral("low")));
        j.append(c2d::Op::spindle(10000));
        j.append(c2d::Op::rapid(10, 5, 5));
        j.append(c2d::Op::feedTo(10, 5, -1, 400));
        j.append(c2d::Op::feedTo(60, 5, -1, 400));      // wholly inside tile 0
        j.append(c2d::Op::spindle(0));
        j.append(c2d::Op::tool(102));
        j.append(c2d::Op::comment(QStringLiteral("high")));
        j.append(c2d::Op::spindle(12000));
        j.append(c2d::Op::rapid(10, 250, 5));
        j.append(c2d::Op::feedTo(10, 250, -1, 400));
        j.append(c2d::Op::feedTo(60, 250, -1, 400));    // wholly inside tile 2
        j.append(c2d::Op::spindle(0));
        const QVector<QVector<c2d::Op>> t = c2d::tileOps(j, 100.0, 5.0);
        check(t.size() == 3, "two-toolpath job tiles into three");
        auto toolsIn = [](const QVector<c2d::Op> &ops) {
            QVector<int> n;
            for (const c2d::Op &o : ops)
                if (o.kind == c2d::Op::Tool)
                    n.append(o.ival);
            return n;
        };
        auto cuts = [](const QVector<c2d::Op> &ops) {
            int n = 0;
            for (const c2d::Op &o : ops)
                if (o.kind == c2d::Op::Feed || o.kind == c2d::Op::Arc)
                    ++n;
            return n;
        };
        check(cuts(t.at(0)) > 0 && cuts(t.at(2)) > 0, "both tiles cut");
        check(toolsIn(t.at(0)) == QVector<int>{201}, "tile 0 changes to its own tool only");
        check(toolsIn(t.at(2)) == QVector<int>{102}, "tile 2 changes to its own tool only");
        check(cuts(t.at(1)) == 0 && toolsIn(t.at(1)).isEmpty(),
              "the middle tile cuts nothing and changes no tool");
    }

    double total = 0;
    for (int k = 0; k < tiles.size(); ++k) {
        double lo, hi;
        yRange(tiles.at(k), &lo, &hi);
        std::fprintf(stderr, "tile %d: %d ops, Y %.3f..%.3f, cut %.2f mm\n", k + 1,
                     int(tiles.at(k).size()), lo, hi, xyCutLength(tiles.at(k)));
        check(lo >= -1e-6 && hi <= tileH + 1e-6, "tile Y range within [0, tileH]");
        check(xyCutLength(tiles.at(k)) > 1, "tile has cutting motion");
        bool tool = false, spindle = false, endsUp = false;
        for (const c2d::Op &op : tiles.at(k)) {
            tool |= op.kind == c2d::Op::Tool;
            spindle |= op.kind == c2d::Op::Spindle && op.ival > 0;
        }
        for (int i = tiles.at(k).size() - 1; i >= 0; --i) {
            const c2d::Op &op = tiles.at(k).at(i);
            if (op.kind == c2d::Op::Rapid || op.kind == c2d::Op::Feed || op.kind == c2d::Op::Arc) {
                endsUp = op.kind == c2d::Op::Rapid && std::fabs(op.z - safeZ) < 1e-9;
                break;
            }
        }
        check(tool && spindle, "tile carries tool change and spindle");
        check(endsUp, "tile ends retracted at safe Z");
        // No feed may start from a position the tile never rapid'ed to: the
        // first motion in each tile is a rapid at safe Z.
        for (const c2d::Op &op : tiles.at(k))
            if (op.kind == c2d::Op::Rapid || op.kind == c2d::Op::Feed || op.kind == c2d::Op::Arc) {
                check(op.kind == c2d::Op::Rapid && std::fabs(op.z - safeZ) < 1e-9,
                      "tile starts with a rapid at safe Z");
                break;
            }
        total += xyCutLength(tiles.at(k));
        const QString gcode = c2d::GrblPost(true).generate(tiles.at(k));
        check(gcode.startsWith(QLatin1String("G90")) && gcode.contains(QLatin1String("G21"))
                  && gcode.contains(QLatin1String("M03S18000"))
                  && gcode.trimmed().endsWith(QLatin1String("M02")),
              "tile is a complete program");
    }
    const double orig = xyCutLength(ops);
    std::fprintf(stderr, "cut length: original %.3f, tiles %.3f\n", orig, total);
    check(std::fabs(total - orig) < 0.05, "tiles cut everything exactly once");

    // The circle crosses the boundary: tessellated pieces must reach Y = 0
    // in tile 2 and Y = 100 in tile 1 (the split happens on the boundary).
    {
        double lo1, hi1, lo2, hi2;
        yRange(tiles.at(0), &lo1, &hi1);
        yRange(tiles.at(1), &lo2, &hi2);
        check(std::fabs(hi1 - tileH) < 1e-6 && std::fabs(lo2) < 1e-6,
              "boundary-crossing motion is split exactly at the boundary");
    }
    // Peck-drill at Y = 250 lands in tile 3 at Y = 50 with both pecks.
    {
        int pecks = 0;
        for (const c2d::Op &op : tiles.at(2))
            if (op.kind == c2d::Op::Feed && std::fabs(op.x - 30) < 1e-9
                && std::fabs(op.y - 50) < 1e-9 && op.z < -1)
                ++pecks;
        check(pecks == 2, "pecks translated into tile 3");
    }

    // --- document round: a real export through exportTiled -----------------
    {
        c2d::Document doc;
        const QJsonObject layer;
        const c2d::Element rect = c2d::Element::makeRectangle({40, 150}, 40, 280, layer);
        doc.addElement(rect);
        QJsonObject j;
        j.insert("type", QStringLiteral("contour"));
        j.insert("name", QStringLiteral("tall"));
        j.insert("enabled", true);
        j.insert("uuid", QStringLiteral("{tp-tile}"));
        j.insert("start_depth", QStringLiteral("0.000"));
        j.insert("end_depth", QStringLiteral("1.000"));
        j.insert("stepdown", 1.0);
        j.insert("ofset_dir", 0);
        QJsonObject speeds;
        speeds.insert("feedrate", 1000);
        speeds.insert("plungerate", 300);
        speeds.insert("rpm", 12000);
        j.insert("speeds", speeds);
        QJsonObject tool;
        tool.insert("diameter", 6.35);
        tool.insert("number", 201);
        j.insert("tool", tool);
        j.insert("elements", QJsonArray{QJsonObject{{"uuid", rect.id}}});
        c2d::Toolpath tp;
        tp.uuid = j.value("uuid").toString();
        tp.type = QStringLiteral("contour");
        tp.json = j;
        doc.addToolpath(tp);

        QTemporaryDir dir;
        check(dir.isValid(), "temp dir");
        const c2d::TiledExport r = c2d::exportTiled(doc, dir.path() + QStringLiteral("/job"), tileH);
        check(r.error.isEmpty(), "exportTiled succeeds");
        check(r.files.size() == 3, "exportTiled writes three files");
        for (int k = 0; k < r.files.size(); ++k) {
            QFile f(r.files.at(k));
            check(f.open(QIODevice::ReadOnly), "tile file readable");
            const QString g = QString::fromUtf8(f.readAll());
            check(g == r.gcode.at(k), "file content matches returned program");
            check(g.endsWith(QStringLiteral("_tile%1.nc").arg(k + 1)) == false
                      && r.files.at(k).endsWith(QStringLiteral("_tile%1.nc").arg(k + 1)),
                  "file named _tile<k>.nc");
            double lo, hi;
            gcodeYRange(g, &lo, &hi);
            std::fprintf(stderr, "%s: Y %.3f..%.3f\n", qPrintable(r.files.at(k)), lo, hi);
            check(lo >= -1e-6 && hi <= tileH + 1e-6, "written tile Y range within [0, tileH]");
            check(g.contains(QLatin1String("(tile")) && g.trimmed().endsWith(QLatin1String("M02")),
                  "written tile framed");
        }
    }

    std::printf("OK: %d checks passed\n", g_checks);
    return 0;
}
