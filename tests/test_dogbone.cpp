// Corner relief (dogbone / T-bone): the bites, the relieved region, and a
// pocket and inside contour export that reach into the corners with them.
// Plain asserts, no test framework; exits 0 on success.

#include "../src/c2ddocument.h"
#include "../src/dogbone.h"
#include "../src/element.h"
#include "../src/gcodeexport.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineF>
#include <QRegularExpression>

#include <algorithm>
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

static QPainterPath rect(double x0, double y0, double x1, double y1)
{
    QPainterPath p;
    p.setFillRule(Qt::OddEvenFill);
    p.addRect(QRectF(QPointF(x0, y0), QPointF(x1, y1)));
    return p;
}

static double dist(QPointF a, QPointF b) { return QLineF(a, b).length(); }

static bool hasBite(const QList<CornerBite> &bites, QPointF corner, QPointF centre)
{
    for (const CornerBite &b : bites)
        if (dist(b.corner, corner) < 1e-6 && dist(b.centre, centre) < 1e-6)
            return true;
    return false;
}

// Tool-centre points of every feed below the top in `gcode`.
static QVector<QPointF> cutXY(const QString &gcode)
{
    QVector<QPointF> out;
    double x = 0, y = 0, z = 0;
    static const QRegularExpression word(QStringLiteral("([XYZ])(-?[0-9.]+)"));
    for (const QString &line : gcode.split(QLatin1Char('\n'))) {
        if (line.startsWith(QLatin1Char('(')))
            continue;
        auto it = word.globalMatch(line);
        while (it.hasNext()) {
            const auto m = it.next();
            const double v = m.captured(2).toDouble();
            (m.captured(1) == QLatin1String("X") ? x : m.captured(1) == QLatin1String("Y") ? y : z) = v;
        }
        if (z < -0.01)
            out.append(QPointF(x, y));
    }
    return out;
}

static double closest(const QVector<QPointF> &pts, QPointF to)
{
    double best = 1e30;
    for (const QPointF &p : pts)
        best = std::min(best, dist(p, to));
    return best;
}

static QString exportOne(const char *type, const QJsonObject &extra)
{
    Document doc;
    const Element e = Element::makeRectangle({10, 5}, 20, 10, doc.defaultLayer());
    doc.addElement(e);
    QJsonObject j{{"type", QLatin1String(type)}, {"name", QStringLiteral("t")}, {"enabled", true},
                  {"uuid", QStringLiteral("{tp-1}")}, {"start_depth", QStringLiteral("0.000")},
                  {"end_depth", QStringLiteral("2.000")}, {"stepdown", 2.0}, {"stepover", 2.0},
                  {"stock_to_leave", 0.0},
                  {"speeds", QJsonObject{{"feedrate", 1000}, {"plungerate", 300}, {"rpm", 10000}}},
                  {"tool", QJsonObject{{"diameter", 6.0}, {"number", 201}}},
                  {"elements", QJsonArray{QJsonObject{{"uuid", e.id}}}}};
    for (auto it = extra.begin(); it != extra.end(); ++it)
        j.insert(it.key(), it.value());
    Toolpath tp;
    tp.uuid = j.value("uuid").toString();
    tp.type = QLatin1String(type);
    tp.json = j;
    doc.addToolpath(tp);
    const GcodeResult g = exportGcode(doc);
    check(g.done.size() == 1, "the toolpath exports");
    return g.gcode;
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    const double d = 6.0, r = d / 2, k = r / std::sqrt(2.0);

    check(cornerStyleOf(QJsonValue()) == CornerStyle::Sharp, "absent key: sharp");
    check(cornerStyleOf(QStringLiteral("off")) == CornerStyle::Sharp, "off: sharp");
    check(cornerStyleOf(QStringLiteral("dogbone")) == CornerStyle::Dogbone, "dogbone");
    check(cornerStyleOf(QStringLiteral("tbone")) == CornerStyle::TBone, "tbone");

    // A 20 x 10 pocket with a 6 mm bit: four bites, circles through the corners.
    const QPainterPath hole = rect(0, 0, 20, 10);
    const QList<CornerBite> dog = cornerBites(hole, false, d, CornerStyle::Dogbone);
    check(dog.size() == 4, "a rectangle pocket gets 4 dogbones");
    for (const CornerBite &b : dog)
        check(std::abs(dist(b.corner, b.centre) - r) < 1e-9, "the bite circle passes through the corner");
    check(hasBite(dog, {0, 0}, {k, k}) && hasBite(dog, {20, 0}, {20 - k, k})
              && hasBite(dog, {20, 10}, {20 - k, 10 - k}) && hasBite(dog, {0, 10}, {k, 10 - k}),
          "dogbone centres on the bisectors, into the pocket");

    // T-bone: centres on the longer (20 mm) walls, r from the corner.
    const QList<CornerBite> tb = cornerBites(hole, false, d, CornerStyle::TBone);
    check(tb.size() == 4, "a rectangle pocket gets 4 T-bones");
    check(hasBite(tb, {0, 0}, {r, 0}) && hasBite(tb, {20, 0}, {20 - r, 0})
              && hasBite(tb, {20, 10}, {20 - r, 10}) && hasBite(tb, {0, 10}, {r, 10}),
          "T-bone centres on the longer walls");

    // The relieved hole: grown past the corner along the bisector, not along the walls far off.
    int n = 0;
    const QPainterPath dh = withCornerRelief(hole, false, d, CornerStyle::Dogbone, 0, 135, &n);
    check(n == 4, "4 bites cut");
    check(dh.contains(QPointF(1, -0.5)) && dh.contains(QPointF(-0.5, 1)) && !hole.contains(QPointF(1, -0.5)),
          "the dogbone pokes past the walls at the corner");
    check(!dh.contains(QPointF(-0.3, -0.3)), "but not diagonally out of it");
    check(!dh.contains(QPointF(10, -0.5)) && !dh.contains(QPointF(-0.5, 5)),
          "the walls away from the corners stay");
    const QPainterPath th = withCornerRelief(hole, false, d, CornerStyle::TBone);
    check(th.contains(QPointF(3, -2.5)) && !th.contains(QPointF(-0.5, 2)),
          "a T-bone goes into the long wall, the short wall stays");

    // Angle range: a square corner is 90 degrees.
    check(cornerBites(hole, false, d, CornerStyle::Dogbone, 0, 80).isEmpty(), "90 is outside 0..80");
    check(cornerBites(hole, false, d, CornerStyle::Sharp).isEmpty(), "sharp: no bites");
    check(cornerBites(hole, false, 0, CornerStyle::Dogbone).isEmpty(), "no bit: no bites");

    // An outside cut of the rectangle: the part has no inside corner.
    check(cornerBites(hole, true, d, CornerStyle::Dogbone).isEmpty(), "a rectangle part has no inside corners");

    // A plate with a rectangular hole cut as a part: the hole's corners are inside corners.
    QPainterPath plate = rect(0, 0, 80, 80);
    plate.addRect(QRectF(30, 30, 20, 10));
    check(cornerBites(plate, true, 4, CornerStyle::Dogbone).size() == 4, "every corner of a hole in a part");
    const QPainterPath pr = withCornerRelief(plate, true, 4, CornerStyle::Dogbone);
    check(!pr.contains(QPointF(31, 29.5)) && plate.contains(QPointF(31, 29.5)),
          "the bite goes into the part past the hole corner");
    check(pr.contains(QPointF(29.5, 29.5)), "but not diagonally out of it");
    check(pr.contains(QPointF(1, 1)), "the outline's convex corners are untouched");

    // An L-shaped part: one reflex corner.
    QPainterPath ell;
    ell.setFillRule(Qt::OddEvenFill);
    ell.addPolygon(QPolygonF({{0, 0}, {40, 0}, {40, 10}, {10, 10}, {10, 40}, {0, 40}, {0, 0}}));
    const QList<CornerBite> lb = cornerBites(ell, true, d, CornerStyle::Dogbone);
    check(lb.size() == 1 && dist(lb.first().corner, {10, 10}) < 1e-9
              && dist(lb.first().centre, {10 + k, 10 + k}) < 1e-6,
          "an L part: one bite at its inside corner, out into the empty side");

    // Export: a pocket reaches the bite centres; sharp stops at r from both walls.
    const QVector<QPointF> sharp = cutXY(exportOne("pocket_toolpath", {}));
    const QVector<QPointF> bone = cutXY(exportOne("pocket_toolpath", {{"corners", "dogbone"}}));
    const QVector<QPointF> tee = cutXY(exportOne("pocket_toolpath", {{"corners", "tbone"}}));
    check(closest(sharp, {k, k}) > 1.0, "a sharp pocket stays r off both walls");
    check(closest(bone, {k, k}) < 0.1 && closest(bone, {20 - k, 10 - k}) < 0.1,
          "a dogbone pocket takes the bit into the corners");
    check(closest(tee, {r, 0}) < 0.1 && closest(tee, {20 - r, 10}) < 0.1,
          "a T-bone pocket takes the bit onto the long walls");
    // Inside contour too; an outside contour of a rectangle is unchanged.
    const QVector<QPointF> ins = cutXY(exportOne("contour", {{"ofset_dir", -1}, {"corners", "dogbone"}}));
    check(closest(ins, {k, k}) < 0.1, "an inside contour with dogbones reaches the corner");
    check(exportOne("contour", {{"ofset_dir", 1}, {"corners", "dogbone"}})
              == exportOne("contour", {{"ofset_dir", 1}}),
          "an outside contour of a rectangle has nothing to relieve");

    std::printf("dogbone: %d checks passed\n", g_checks);
    return 0;
}
