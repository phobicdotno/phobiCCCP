// Job zero (phobic_zero_xy / phobic_zero_z): the posted program moves to the
// chosen point of the stock, the preview operations stay in design
// coordinates, and with the default (lower left, top) nothing changes.
// Plain asserts, exits 0 on success.

#include "../src/c2ddocument.h"
#include "../src/element.h"
#include "../src/gcodeexport.h"
#include "../src/post_grbl.h"
#include "../src/tiling.h"

#include <QCoreApplication>
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

// A 10 x 10 mm pocket at the stock's lower-left corner, 3 mm deep.
static c2d::Document pocketDoc()
{
    c2d::Document doc;
    doc.setParam(QStringLiteral("width"), QStringLiteral("100"));
    doc.setParam(QStringLiteral("height"), QStringLiteral("100"));
    doc.setParam(QStringLiteral("thickness"), QStringLiteral("19"));
    const c2d::Element e = c2d::Element::makeRectangle({5, 5}, 10, 10, QJsonObject{});
    doc.addElement(e);
    c2d::Toolpath tp;
    tp.uuid = QStringLiteral("{tp-pocket}");
    tp.type = QStringLiteral("pocket_toolpath");
    tp.json = QJsonObject{
        {"type", "pocket_toolpath"}, {"name", "pocket"}, {"enabled", true}, {"uuid", tp.uuid},
        {"start_depth", QStringLiteral("0.000")}, {"end_depth", QStringLiteral("3.000")},
        {"stepdown", 1.0}, {"stepover", 40},
        {"elements", QJsonArray{QJsonObject{{"uuid", e.id}}}},
        {"speeds", QJsonObject{{"feedrate", 1000}, {"plungerate", 300}, {"rpm", 10000}}},
        {"tool", QJsonObject{{"diameter", 3.175}, {"number", 201}}}};
    doc.addToolpath(tp);
    return doc;
}

struct Words { QChar axis; double v; };

// Every X/Y/Z word of the program's motion lines, in order (comments skipped).
static QList<QList<Words>> motion(const QString &gcode)
{
    static const QRegularExpression re(QStringLiteral("([XYZ])(-?[0-9.]+)"));
    QList<QList<Words>> out;
    for (QString line : gcode.split(QLatin1Char('\n'))) {
        const int c = line.indexOf(QLatin1Char('('));
        if (c >= 0)
            line.truncate(c);
        const int s = line.indexOf(QLatin1Char(';'));
        if (s >= 0)
            line.truncate(s);
        QList<Words> w;
        for (auto it = re.globalMatch(line); it.hasNext();) {
            const auto m = it.next();
            w.append({m.captured(1).at(0), m.captured(2).toDouble()});
        }
        if (!w.isEmpty())
            out.append(w);
    }
    return out;
}

static bool shiftedBy(const QString &base, const QString &moved, double dx, double dy, double dz)
{
    const auto a = motion(base), b = motion(moved);
    if (a.size() != b.size() || a.isEmpty())
        return false;
    for (int i = 0; i < a.size(); ++i) {
        if (a.at(i).size() != b.at(i).size())
            return false;
        for (int k = 0; k < a.at(i).size(); ++k) {
            const Words &p = a.at(i).at(k), &q = b.at(i).at(k);
            const double d = p.axis == QLatin1Char('X') ? dx : p.axis == QLatin1Char('Y') ? dy : dz;
            if (p.axis != q.axis || std::abs(p.v + d - q.v) > 0.002)
                return false;
        }
    }
    return true;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    c2d::Document base = pocketDoc();
    const c2d::GcodeResult r0 = c2d::exportGcode(base);
    check(r0.done.size() == 1, "the pocket exports");
    check(c2d::jobZeroIsDefault(base), "no zero params: lower left, top");
    check(c2d::jobZeroText(base) == QStringLiteral("lower left corner, top"), "default zero text");

    // Default spelled out: byte for byte the same program.
    c2d::Document spelled = pocketDoc();
    spelled.setParam(QStringLiteral("phobic_zero_xy"), QStringLiteral("lower_left"));
    spelled.setParam(QStringLiteral("phobic_zero_z"), QStringLiteral("top"));
    check(c2d::exportGcode(spelled).gcode == r0.gcode, "default zero is byte-identical");
    // An unknown value falls back to the default, also unchanged.
    spelled.setParam(QStringLiteral("phobic_zero_xy"), QStringLiteral("nonsense"));
    check(c2d::exportGcode(spelled).gcode == r0.gcode, "unknown zero value reads as the default");

    // Centre of a 100 x 100 stock: X and Y shifted by -50, Z unchanged.
    c2d::Document centre = pocketDoc();
    centre.setParam(QStringLiteral("phobic_zero_xy"), QStringLiteral("centre"));
    check(c2d::jobZeroPoint(centre) == QPointF(50, 50), "centre zero point");
    const c2d::GcodeResult rc = c2d::exportGcode(centre);
    check(shiftedBy(r0.gcode, rc.gcode, -50, -50, 0), "centre zero shifts X/Y by -50");
    check(rc.ops.size() == r0.ops.size(), "preview ops are the same count");
    bool sameOps = true;
    for (int i = 0; i < rc.ops.size(); ++i)
        sameOps = sameOps && rc.ops.at(i).x == r0.ops.at(i).x && rc.ops.at(i).y == r0.ops.at(i).y
                  && rc.ops.at(i).z == r0.ops.at(i).z;
    check(sameOps, "preview ops stay in design coordinates");
    check(rc.gcode.contains(QStringLiteral("Zero: centre of stock, top")), "program names its zero");

    // Bottom zero: Z shifted up by the thickness (cut at Z = 19 - depth).
    c2d::Document bottom = pocketDoc();
    bottom.setParam(QStringLiteral("phobic_zero_z"), QStringLiteral("bottom"));
    const c2d::GcodeResult rb = c2d::exportGcode(bottom);
    check(shiftedBy(r0.gcode, rb.gcode, 0, 0, 19), "bottom zero shifts Z by +thickness");
    check(c2d::jobZeroText(bottom) == QStringLiteral("lower left corner, bottom (spoilboard)"),
          "bottom zero text");
    {
        QVector<c2d::Op> prog = rb.ops;
        c2d::shiftOps(prog, c2d::jobZeroShift(bottom));
        const c2d::JobStats st = c2d::computeStats(prog);
        check(std::abs(st.minZ - 16.0) < 0.01, "deepest cut is at Z = thickness - depth");
    }

    // The corners.
    c2d::Document ur = pocketDoc();
    ur.setParam(QStringLiteral("phobic_zero_xy"), QStringLiteral("upper_right"));
    check(shiftedBy(r0.gcode, c2d::exportGcode(ur).gcode, -100, -100, 0), "upper right shifts -100/-100");
    c2d::Document ul = pocketDoc();
    ul.setParam(QStringLiteral("phobic_zero_xy"), QStringLiteral("upper_left"));
    check(shiftedBy(r0.gcode, c2d::exportGcode(ul).gcode, 0, -100, 0), "upper left shifts Y -100");
    c2d::Document lr = pocketDoc();
    lr.setParam(QStringLiteral("phobic_zero_xy"), QStringLiteral("lower_right"));
    check(shiftedBy(r0.gcode, c2d::exportGcode(lr).gcode, -100, 0, 0), "lower right shifts X -100");

    // Tiled export: X and Z follow the zero, Y stays at the tile's lower edge.
    {
        QTemporaryDir dir;
        check(dir.isValid(), "temp dir");
        c2d::Document plain = pocketDoc();
        const c2d::TiledExport t0 = c2d::exportTiled(plain, dir.filePath(QStringLiteral("a")), 200);
        c2d::Document moved = pocketDoc();
        moved.setParam(QStringLiteral("phobic_zero_xy"), QStringLiteral("centre"));
        moved.setParam(QStringLiteral("phobic_zero_z"), QStringLiteral("bottom"));
        const c2d::TiledExport t1 = c2d::exportTiled(moved, dir.filePath(QStringLiteral("b")), 200);
        check(t0.error.isEmpty() && t1.error.isEmpty() && t0.gcode.size() == 1 && t1.gcode.size() == 1,
              "tiled exports written");
        check(shiftedBy(t0.gcode.at(0), t1.gcode.at(0), -50, 0, 19), "tile shifted in X and Z only");
        check(t1.gcode.at(0).startsWith(QStringLiteral("(Zero:")), "tile names its zero");
    }

    std::printf("jobzero: %d checks passed\n", g_checks);
    return 0;
}
