// Headless checks for the vector operations (Booleans / Offset / Align):
// union area, subtract producing a hole ring, offset growing the bbox by d,
// stroke-outline offset of an open path, and the alignment arithmetic.
// Plain asserts, no test framework; exits 0 on success.

#include "../src/element.h"
#include "../src/vectorops.h"

#include <QCoreApplication>
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

static bool approx(double a, double b, double eps = 1e-6)
{
    return std::fabs(a - b) < eps;
}

// Total even-odd area of a ring set: |sum of signed areas| (holes come back
// with opposite orientation from Clipper).
static double netArea(const QVector<QPolygonF> &rings)
{
    double a = 0;
    for (const QPolygonF &r : rings)
        a += vec::ringArea(r);
    return std::fabs(a);
}

static QRectF bounds(const QVector<Element> &els)
{
    QRectF r;
    for (const Element &e : els)
        r = r.isNull() ? e.painterPath.boundingRect() : r.united(e.painterPath.boundingRect());
    return r;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QJsonObject layer;
    layer.insert("name", QStringLiteral("DEFAULT"));
    layer.insert("uuid", QStringLiteral("{layer}"));

    const Element sqA = Element::makeRectangle({10, 10}, 20, 20, layer);   // 0..20
    const Element sqB = Element::makeRectangle({20, 10}, 20, 20, layer);   // 10..30
    const Element hole = Element::makeCircle({10, 10}, 5, layer);

    // --- union: two overlapping 20x20 squares => 20x30 => area 600 ---------
    {
        const QVector<Element> out = vec::booleanElements({sqA, sqB}, vec::BoolOp::Union);
        check(out.size() == 1, "union yields one ring");
        check(out.first().geometryType == QLatin1String("path"), "union result is a path");
        check(vec::isClosed(out.first()), "union result is closed");
        check(out.first().raw.value("layer").toObject() == layer, "union keeps layer");
        QVector<QPolygonF> rings;
        rings.append(out.first().painterPath.toFillPolygon());
        check(approx(netArea(rings), 600.0, 0.05), "union area 600");
        const QRectF bb = bounds(out);
        check(approx(bb.left(), 0, 1e-3) && approx(bb.right(), 30, 1e-3) &&
              approx(bb.top(), 0, 1e-3) && approx(bb.bottom(), 20, 1e-3), "union bbox");
        // JSON shape: closing rows as makePath writes them.
        const QJsonArray pt = out.first().raw.value("point_type").toArray();
        check(pt.first().toInt() == 0 && pt.last().toInt() == 4, "union JSON close row");
    }

    // --- union of a shape fully inside another: the inner one vanishes ----
    {
        const QVector<QPolygonF> rings = vec::booleanRings(
            {sqA.painterPath, hole.painterPath}, {}, vec::BoolOp::Union);
        check(rings.size() == 1 && approx(netArea(rings), 400.0, 0.05), "union swallows nested");
        // ...but a text-like element with its own counter keeps the hole.
        QPainterPath ringPath = sqA.painterPath;
        ringPath.addPath(hole.painterPath);
        const Element sqC = Element::makeRectangle({26, 10}, 20, 20, layer);   // 16..36
        const QVector<QPolygonF> r2 = vec::booleanRings({ringPath, sqC.painterPath}, {},
                                                        vec::BoolOp::Union);
        check(r2.size() == 2 && approx(netArea(r2), 720.0 - M_PI * 25.0, 0.1),
              "union keeps an element's own holes");
    }

    // --- intersect: overlap strip 10..20 => area 200 -----------------------
    {
        const QVector<QPolygonF> rings = vec::booleanRings(
            {sqA.painterPath, sqB.painterPath}, {}, vec::BoolOp::Intersect);
        check(rings.size() == 1, "intersect one ring");
        check(approx(netArea(rings), 200.0, 0.05), "intersect area 200");
    }

    // --- subtract: square minus centered circle => outer + hole ring -------
    {
        const QVector<Element> out = vec::booleanElements({sqA, hole}, vec::BoolOp::Subtract);
        check(out.size() == 2, "subtract yields outer + hole");
        int holes = 0;
        double outerArea = 0, holeArea = 0;
        for (const Element &e : out) {
            const QRectF bb = e.painterPath.boundingRect();
            const double a = std::fabs(vec::ringArea(e.painterPath.toFillPolygon()));
            if (bb.width() < 15) { ++holes; holeArea = a; } else outerArea = a;
        }
        check(holes == 1, "subtract has one hole ring");
        check(approx(outerArea, 400.0, 0.05), "subtract outer ring is the square");
        check(approx(holeArea, M_PI * 25.0, 0.05), "subtract hole ring is the circle");
        // Net even-odd area = 400 - 25pi.
        QVector<QPolygonF> rings;
        for (const Element &e : out)
            rings.append(e.painterPath.toFillPolygon());
        check(approx(netArea(rings), 400.0 - M_PI * 25.0, 0.1), "subtract net area");
    }

    // --- subtract with disjoint clip leaves the subject untouched ----------
    {
        const Element far = Element::makeCircle({100, 100}, 5, layer);
        const QVector<QPolygonF> rings = vec::booleanRings({sqA.painterPath}, {far.painterPath},
                                                           vec::BoolOp::Subtract);
        check(rings.size() == 1 && approx(netArea(rings), 400.0, 0.05), "subtract disjoint");
    }

    // --- open paths are ignored by booleans --------------------------------
    {
        const Element line = Element::makePath({{0, 0}, {30, 30}}, false, layer);
        check(!vec::isClosed(line), "open path detected");
        const QVector<Element> out = vec::booleanElements({sqA, line, sqB}, vec::BoolOp::Union);
        check(out.size() == 1 && approx(bounds(out).width(), 30, 1e-3), "union skips open path");
    }

    // --- offset outward by d grows the bbox by d on every side ------------
    {
        const double d = 2.5;
        const QVector<Element> out = vec::offsetElements({sqA}, d);
        check(out.size() == 1, "offset one ring");
        const QRectF bb = bounds(out);
        check(approx(bb.left(), -d, 1e-3) && approx(bb.right(), 20 + d, 1e-3) &&
              approx(bb.top(), -d, 1e-3) && approx(bb.bottom(), 20 + d, 1e-3),
              "outward offset bbox grows by d");
        // Round joins: area = 400 + 4*20*d + pi d^2.
        const double a = std::fabs(vec::ringArea(out.first().painterPath.toFillPolygon()));
        check(approx(a, 400 + 80 * d + M_PI * d * d, 0.2), "outward offset area (round joins)");

        const QVector<Element> in = vec::offsetElements({sqA}, -d);
        check(in.size() == 1, "inward offset one ring");
        const QRectF ib = bounds(in);
        check(approx(ib.left(), d, 1e-3) && approx(ib.right(), 20 - d, 1e-3), "inward offset shrinks");
        check(vec::offsetElements({sqA}, -11).isEmpty(), "over-inset vanishes");
    }

    // --- offset of a ring (square with hole): hole shrinks when growing ---
    {
        const QVector<Element> out = vec::offsetElements({sqA, hole}, 1.0);
        check(out.size() == 2, "ring offset keeps outer + hole");
        double minW = 1e9;
        for (const Element &e : out)
            minW = std::min(minW, e.painterPath.boundingRect().width());
        check(approx(minW, 8.0, 1e-2), "hole shrinks by d when growing");
    }

    // --- open path offset: stroke outline of width 2d ---------------------
    {
        const Element line = Element::makePath({{0, 0}, {10, 0}}, false, layer);
        const QVector<Element> out = vec::offsetElements({line}, 1.0);
        check(out.size() == 1 && vec::isClosed(out.first()), "open offset is one closed outline");
        const QRectF bb = bounds(out);
        check(approx(bb.left(), -1, 1e-3) && approx(bb.right(), 11, 1e-3) &&
              approx(bb.top(), -1, 1e-3) && approx(bb.bottom(), 1, 1e-3), "stroke outline bbox");
    }

    // --- align -------------------------------------------------------------
    {
        const QVector<QRectF> boxes{QRectF(0, 0, 10, 10), QRectF(20, 5, 4, 2), QRectF(5, 30, 6, 6)};
        QRectF ref;
        for (const QRectF &b : boxes)
            ref = ref.isNull() ? b : ref.united(b);
        check(ref == QRectF(0, 0, 24, 36), "selection bbox");

        auto d = vec::alignDeltas(boxes, vec::Align::Left, ref);
        check(approx(d[0].x(), 0) && approx(d[1].x(), -20) && approx(d[2].x(), -5) &&
              approx(d[1].y(), 0), "align left");
        d = vec::alignDeltas(boxes, vec::Align::Right, ref);
        check(approx(d[0].x(), 14) && approx(d[1].x(), 0) && approx(d[2].x(), 13), "align right");
        d = vec::alignDeltas(boxes, vec::Align::HCenter, ref);
        check(approx(d[0].x(), 7) && approx(d[1].x(), -10) && approx(d[2].x(), 4), "align hcenter");
        // Y-up: Top = max y (36), Bottom = min y (0).
        d = vec::alignDeltas(boxes, vec::Align::Top, ref);
        check(approx(d[0].y(), 26) && approx(d[1].y(), 29) && approx(d[2].y(), 0) &&
              approx(d[0].x(), 0), "align top (Y-up)");
        d = vec::alignDeltas(boxes, vec::Align::Bottom, ref);
        check(approx(d[0].y(), 0) && approx(d[1].y(), -5) && approx(d[2].y(), -30), "align bottom");
        d = vec::alignDeltas(boxes, vec::Align::VCenter, ref);
        check(approx(d[0].y(), 13) && approx(d[1].y(), 12) && approx(d[2].y(), -15), "align vcenter");

        // Center on a 100x50 stock: group bbox center (12,18) -> (50,25).
        d = vec::centerDeltas(boxes, vec::Center::Both, QRectF(0, 0, 100, 50));
        check(d.size() == 3 && approx(d[0].x(), 38) && approx(d[0].y(), 7) &&
              approx(d[2].x(), 38) && approx(d[2].y(), 7), "center on stock both");
        d = vec::centerDeltas(boxes, vec::Center::Horizontal, QRectF(0, 0, 100, 50));
        check(approx(d[0].x(), 38) && approx(d[0].y(), 0), "center on stock H only");
        d = vec::centerDeltas(boxes, vec::Center::Vertical, QRectF(0, 0, 100, 50));
        check(approx(d[0].x(), 0) && approx(d[0].y(), 7), "center on stock V only");

        // Distribute horizontally: centers 5, 22, 8 -> outer stay (5, 22),
        // the middle one (index 2, center 8) moves to 13.5.
        d = vec::distributeDeltas(boxes, vec::Axis::Horizontal);
        check(approx(d[0].x(), 0) && approx(d[1].x(), 0) && approx(d[2].x(), 5.5) &&
              approx(d[2].y(), 0), "distribute horizontally");
        // Vertically: centers 5, 6, 33 -> 5, 19, 33: index 1 moves +13.
        d = vec::distributeDeltas(boxes, vec::Axis::Vertical);
        check(approx(d[0].y(), 0) && approx(d[1].y(), 13) && approx(d[2].y(), 0), "distribute vertically");
        check(vec::distributeDeltas({boxes[0], boxes[1]}, vec::Axis::Vertical)[1].isNull(),
              "distribute needs three");
    }

    std::printf("OK: %d checks passed\n", g_checks);
    return 0;
}
