// Headless checks for the vector operations (Booleans / Offset / Align):
// union area, subtract producing a hole ring, offset growing the bbox by d,
// stroke-outline offset of an open path, the alignment arithmetic, and the
// rigid transforms behind Mirror and the grid / circular arrays.
// Plain asserts, no test framework; exits 0 on success.

#include "../src/element.h"
#include "../src/vectorops.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QTransform>

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

static bool nearPt(QPointF a, QPointF b, double eps = 1e-6)
{
    return approx(a.x(), b.x(), eps) && approx(a.y(), b.y(), eps);
}

static bool nearRect(const QRectF &a, const QRectF &b, double eps = 1e-6)
{
    return nearPt(a.topLeft(), b.topLeft(), eps) && nearPt(a.bottomRight(), b.bottomRight(), eps);
}

// Signed area of an element's first subpath, flattened (orientation check).
static double firstRingArea(const Element &e)
{
    const auto polys = e.painterPath.toSubpathPolygons();
    return polys.isEmpty() ? 0 : vec::ringArea(polys.first());
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
    // Text needs fonts, so a GUI application; offscreen when there is no display.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && qEnvironmentVariableIsEmpty("DISPLAY")
        && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
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

    // ---- mirror and array ----------------------------------------------------
    {
        const QTransform mh = vec::mirrorTransform(QRectF(0, 0, 20, 10), vec::Axis::Horizontal);
        check(nearPt(mh.map(QPointF(0, 3)), QPointF(20, 3)) &&
              nearPt(mh.map(QPointF(5, 7)), QPointF(15, 7)), "mirror H flips x about the box center");
        const QTransform mv = vec::mirrorTransform(QRectF(0, 0, 20, 10), vec::Axis::Vertical);
        check(nearPt(mv.map(QPointF(4, 0)), QPointF(4, 10)), "mirror V flips y about the box center");

        // Path: every point maps, the id stays, the winding survives the flip.
        const Element tri = Element::makePath({{0, 0}, {10, 0}, {0, 5}}, true, layer);
        const Element triM = vec::transformElement(tri, mh);
        check(triM.id == tri.id && triM.geometryType == "path", "mirrored path keeps id and type");
        check(nearRect(triM.painterPath.boundingRect(), QRectF(10, 0, 10, 5)), "mirrored path bounds");
        check(firstRingArea(tri) > 0 && firstRingArea(triM) > 0, "mirror keeps the winding (CCW)");
        check(approx(firstRingArea(triM), firstRingArea(tri)), "mirror keeps the area");
        const PathModel tm = Element::pathModel(triM);
        check(tm.subs.size() == 1 && tm.subs.first().closed && tm.subs.first().nodes.size() == 3 &&
              nearPt(tm.subs.first().nodes.first().p, QPointF(20, 0)),
              "mirrored closed path keeps its start node");

        // Bezier path under a reflection: the shape is the reflected shape.
        PathModel bm;
        SubPath sp;
        PathNode a, b;
        a.p = QPointF(0, 0); a.in = a.p; a.out = QPointF(3, 6); a.kind = PathNode::Corner;
        b.p = QPointF(10, 0); b.in = QPointF(8, 6); b.out = b.p; b.kind = PathNode::Corner;
        sp.nodes = {a, b};
        bm.subs.append(sp);
        const Element arc = Element::makeBezierPath(bm, layer);
        const Element arcM = vec::transformElement(arc, mv);
        const QPainterPath expect = mv.map(arc.painterPath);
        check(nearRect(arcM.painterPath.boundingRect(), expect.boundingRect(), 1e-6) &&
              nearPt(arcM.painterPath.pointAtPercent(0.3), expect.pointAtPercent(0.7), 1e-6),
              "mirrored open bezier traces the reflected curve, reversed");

        // Circle: stays a circle, center moves, radius kept.
        const Element circ = Element::makeCircle({5, 5}, 3, layer);
        const Element circM = vec::transformElement(circ, mh);
        const QJsonArray cc = circM.raw["center"].toArray();
        check(circM.geometryType == "circle" && circM.id == circ.id &&
              nearPt(QPointF(cc[0].toDouble(), cc[1].toDouble()), QPointF(15, 5)) &&
              approx(circM.raw["radius"].toDouble(), 3), "mirrored circle is the same circle, moved");
        QTransform turn33;
        turn33.rotate(33);
        const Element circR = vec::transformElement(circ, turn33);
        check(circR.geometryType == "circle" &&
              nearRect(circR.painterPath.boundingRect(),
                       QRectF(turn33.map(QPointF(5, 5)) - QPointF(3, 3), QSizeF(6, 6)), 1e-6),
              "rotated circle stays a circle");

        // Rectangle: symmetric under a flip (stays parametric); a 30 deg turn
        // makes it a path with the same id.
        const Element rect = Element::makeRectangle({4, 3}, 8, 6, layer);
        const Element rectM = vec::transformElement(rect, mh);
        check(rectM.geometryType == "rectangle" && rectM.id == rect.id &&
              nearRect(rectM.painterPath.boundingRect(), QRectF(12, 0, 8, 6)),
              "mirrored rectangle stays a rectangle");
        QTransform turn30;
        turn30.rotate(30);
        const Element rectR = vec::transformElement(rect, turn30);
        check(rectR.geometryType == "path" && rectR.id == rect.id &&
              nearRect(rectR.painterPath.boundingRect(), turn30.map(rect.painterPath).boundingRect(), 1e-6) &&
              approx(std::fabs(firstRingArea(rectR)), 48, 1e-6), "rotated rectangle becomes a path");

        // Regular polygon: a triangle flipped is a triangle with a new rotation.
        const Element trig = Element::makePolygon({0, 0}, 10, 3, layer, 0);
        const Element trigM = vec::transformElement(trig, QTransform::fromScale(-1, 1));
        check(trigM.geometryType == "regular_polygon" && trigM.id == trig.id &&
              approx(std::fabs(trigM.raw["rotation"].toDouble()), 180, 1e-6) &&
              nearRect(trigM.painterPath.boundingRect(),
                       QTransform::fromScale(-1, 1).map(trig.painterPath).boundingRect(), 1e-6),
              "mirrored triangle stays a regular polygon, rotated 180");
        QTransform turn60;
        turn60.rotate(60);
        const Element hex = Element::makePolygon({0, 0}, 10, 6, layer, 0);
        const Element hexR = vec::transformElement(hex, turn60);
        check(hexR.geometryType == "regular_polygon" && approx(hexR.raw["rotation"].toDouble(), 0),
              "hexagon turned by 60 deg is unchanged");

        // Text: stays text; the flip lives in its transform.
        const Element txt = Element::makeText(QStringLiteral("F"), {10, 10}, 10, "DejaVu Sans", layer);
        const QTransform tmh = vec::mirrorTransform(txt.painterPath.boundingRect(), vec::Axis::Horizontal);
        const Element txtM = vec::transformElement(txt, tmh);
        const QJsonArray xf = txtM.raw["transform"].toArray();
        check(txtM.geometryType == "text" && txtM.id == txt.id && xf.size() == 9 &&
              approx(xf[0].toDouble(), -1) && approx(xf[4].toDouble(), 1),
              "mirrored text keeps the flip in its transform");
        check(nearRect(txtM.painterPath.boundingRect(), txt.painterPath.boundingRect(), 1e-6),
              "text mirrored about its own center keeps its bounds");

        // Pure translation goes through translate().
        const Element moved = vec::transformElement(rect, QTransform::fromTranslate(5, -1));
        check(moved.geometryType == "rectangle" &&
              nearRect(moved.painterPath.boundingRect(), QRectF(5, -1, 8, 6)), "translation moves");

        // Grid: 3 x 2 of a 10 x 4 box with gaps 2 / 1 -> 5 copies, original excluded.
        const auto g = vec::gridTransforms(QRectF(0, 0, 10, 4), 3, 2, 2, 1);
        check(g.size() == 5, "grid 3x2 = 5 copies");
        QSet<QString> offs;
        for (const QTransform &t : g)
            offs.insert(QString::asprintf("%.3f,%.3f", t.dx(), t.dy()));
        check(offs.contains("12.000,0.000") && offs.contains("24.000,0.000") &&
              offs.contains("0.000,5.000") && offs.contains("24.000,5.000") &&
              !offs.contains("0.000,0.000"), "grid steps are size + gap");
        check(vec::gridTransforms(QRectF(0, 0, 1, 1), 0, 3, 0, 0).isEmpty() &&
              vec::gridTransforms(QRectF(0, 0, 1, 1), 1, 1, 0, 0).isEmpty(), "grid of one is empty");

        // Circular: 4 around (0,0) from (10,0): 90, 180, 270 deg.
        const QRectF dot(9, -1, 2, 2);
        auto c = vec::circularTransforms(dot, {0, 0}, 4, 360, true);
        check(c.size() == 3 && nearPt(c[0].map(QPointF(10, 0)), QPointF(0, 10)) &&
              nearPt(c[1].map(QPointF(10, 0)), QPointF(-10, 0)) &&
              nearPt(c[2].map(QPointF(10, 0)), QPointF(0, -10)), "circular full turn, CCW");
        check(nearPt(c[0].map(QPointF(11, 0)), QPointF(0, 11)), "rotating copies turn with the ring");
        c = vec::circularTransforms(dot, {0, 0}, 3, 90, true);
        check(c.size() == 2 && nearPt(c[0].map(QPointF(10, 0)), QPointF(std::sqrt(50.0), std::sqrt(50.0))) &&
              nearPt(c[1].map(QPointF(10, 0)), QPointF(0, 10)), "partial arc: ends on both ends");
        c = vec::circularTransforms(dot, {0, 0}, 2, 360, false);
        check(c.size() == 1 && c[0].type() <= QTransform::TxTranslate &&
              nearPt(c[0].map(QPointF(10, 0)), QPointF(-10, 0)), "non-rotating copies only move");
        c = vec::circularTransforms(dot, {0, 0}, 4, -360, true);
        check(nearPt(c[0].map(QPointF(10, 0)), QPointF(0, -10)), "negative angle runs clockwise");
        check(vec::circularTransforms(dot, {0, 0}, 1, 360, true).isEmpty(), "circular of one is empty");

        // Copies: fresh ids; a group stays a group, but a new one.
        QJsonObject ra = rect.raw, ca = circ.raw, ta = tri.raw;
        ra["group_id"] = QJsonArray{QStringLiteral("{g1}")};
        ca["group_id"] = QJsonArray{QStringLiteral("{g1}")};
        const QVector<Element> src = {Element::fromJson(ra), Element::fromJson(ca), Element::fromJson(ta)};
        const auto cp = vec::copyElements(src, QTransform::fromTranslate(100, 0));
        check(cp.size() == 3 && cp[0].id != rect.id && cp[1].id != circ.id && cp[2].id != tri.id &&
              cp[0].id != cp[1].id, "copies get fresh ids");
        const QString g0 = cp[0].raw["group_id"].toArray().at(0).toString();
        check(!g0.isEmpty() && g0 != "{g1}" && cp[1].raw["group_id"].toArray().at(0).toString() == g0 &&
              cp[2].raw["group_id"].toArray().isEmpty(), "copied group gets one new group id");
        check(nearRect(cp[0].painterPath.boundingRect(), QRectF(100, 0, 8, 6)), "copy is moved");
        const auto cp2 = vec::copyElements(src, QTransform::fromTranslate(200, 0));
        check(cp2[0].raw["group_id"].toArray().at(0).toString() != g0, "each copy is its own group");
    }

    std::printf("OK: %d checks passed\n", g_checks);
    return 0;
}
