// Headless checks for the bezier node model and text-on-arc rendering:
// - a bezier path round-trips through toJson/fromJson with identical control
//   points, in exactly CC's row layout (cp1 = previous out-handle, cp2 = own
//   in-handle, duplicate-start row + point_type 4 for closed paths);
// - node insert/delete keep the endpoints and the curve;
// - arc text is centred on arc_center and spans the expected angle.
//
// Optional: `test_paths <in.c2d> <out.c2d>` additionally loads a document,
// adds a bezier path plus straight/arc text specimens and saves it — handy for
// a `--shot` of the result.
//
// Plain asserts, no test framework; exits 0 on success.

#include "../src/c2ddocument.h"
#include "../src/element.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineF>
#include <QUuid>
#include <QtMath>

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

static bool near(const QPointF &a, const QPointF &b, double eps = 1e-9)
{
    return std::fabs(a.x() - b.x()) < eps && std::fabs(a.y() - b.y()) < eps;
}

static c2d::PathNode node(QPointF p, QPointF in, QPointF out, c2d::PathNode::Kind k)
{
    c2d::PathNode n;
    n.p = p; n.in = in; n.out = out; n.kind = k;
    return n;
}

// A five-node shape mixing a symmetric node, a corner, a smooth node and a
// straight segment (both handles retracted).
static c2d::PathModel specimen(bool closed)
{
    c2d::SubPath s;
    s.closed = closed;
    s.nodes.append(node({10, 10}, {5, 15}, {15, 5}, c2d::PathNode::Symmetric));
    s.nodes.append(node({60, 10}, {50, -4}, {70, 20}, c2d::PathNode::Corner));
    s.nodes.append(node({60, 50}, {75, 40}, {37.5, 65}, c2d::PathNode::Smooth));
    s.nodes.append(node({10, 50}, {10, 50}, {10, 50}, c2d::PathNode::Corner));
    s.nodes.append(node({10, 30}, {10, 30}, {10, 30}, c2d::PathNode::Corner));
    c2d::PathModel m;
    m.subs.append(s);
    return m;
}

static bool sameModel(const c2d::PathModel &a, const c2d::PathModel &b)
{
    if (a.subs.size() != b.subs.size())
        return false;
    for (int s = 0; s < a.subs.size(); ++s) {
        const c2d::SubPath &x = a.subs.at(s), &y = b.subs.at(s);
        if (x.closed != y.closed || x.nodes.size() != y.nodes.size())
            return false;
        for (int i = 0; i < x.nodes.size(); ++i)
            if (!near(x.nodes.at(i).p, y.nodes.at(i).p) || !near(x.nodes.at(i).in, y.nodes.at(i).in)
                || !near(x.nodes.at(i).out, y.nodes.at(i).out))
                return false;
    }
    return true;
}

static QPointF jpt(const QJsonArray &a, int i)
{
    const QJsonArray p = a.at(i).toArray();
    return QPointF(p.at(0).toDouble(), p.at(1).toDouble());
}

// Angle (degrees, 0..360) and radius of every rendered point about `c`.
struct Polar { double minA, maxA, minR, maxR, meanA; };
static Polar polar(const c2d::Element &e, QPointF c)
{
    Polar r{1e9, -1e9, 1e9, -1e9, 0};
    double sx = 0, sy = 0;
    int n = 0;
    for (const QJsonValue &cv : e.raw.value("rendered").toArray()) {
        for (const QJsonValue &pv : cv.toArray()) {
            const QJsonArray p = pv.toArray();
            const QPointF q(p.at(0).toDouble() - c.x(), p.at(1).toDouble() - c.y());
            double a = qRadiansToDegrees(std::atan2(q.y(), q.x()));
            if (a < 0) a += 360;
            r.minA = std::min(r.minA, a); r.maxA = std::max(r.maxA, a);
            const double rad = std::hypot(q.x(), q.y());
            r.minR = std::min(r.minR, rad); r.maxR = std::max(r.maxR, rad);
            sx += q.x(); sy += q.y(); ++n;
        }
    }
    // Mid of the angular extent: a point-density mean would lean towards the
    // glyphs with the most outline points.
    r.meanA = (r.minA + r.maxA) / 2;
    (void)sx; (void)sy; (void)n;
    return r;
}

int main(int argc, char *argv[])
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && qEnvironmentVariableIsEmpty("DISPLAY")
        && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    const QJsonObject layer;

    // --- closed bezier path round-trips through JSON ------------------------
    {
        const c2d::PathModel m = specimen(true);
        const c2d::Element e = c2d::Element::makeBezierPath(m, layer);
        check(e.geometryType == QLatin1String("path") && e.behavior == c2d::Element::Path,
              "bezier path is a path element");
        const QJsonObject o = QJsonDocument::fromJson(e.toJson()).object();
        const QJsonArray pts = o.value("points").toArray(), cp1 = o.value("cp1").toArray(),
                         cp2 = o.value("cp2").toArray(), pt = o.value("point_type").toArray();
        // 5 anchors + return-to-start row + close row.
        check(pts.size() == 7 && cp1.size() == 7 && cp2.size() == 7 && pt.size() == 7,
              "closed path row count");
        check(pt.at(0).toInt() == 0 && pt.at(6).toInt() == 4, "first/close point types");
        check(pt.at(5).toInt() == 3, "closing segment into a curved first node is a curve");
        check(pt.at(1).toInt() == 3 && pt.at(2).toInt() == 3 && pt.at(3).toInt() == 3,
              "curved segments are point_type 3");
        check(pt.at(4).toInt() == 1, "retracted handles give a point_type 1 row");
        // Row i carries the segment arriving at anchor i: cp1 = out-handle of
        // i-1, cp2 = in-handle of i.
        check(near(jpt(cp1, 1), {15, 5}) && near(jpt(cp2, 1), {50, -4}), "row 1 control points");
        check(near(jpt(cp1, 2), {70, 20}) && near(jpt(cp2, 2), {75, 40}), "row 2 control points");
        check(near(jpt(pts, 5), {10, 10}) && near(jpt(pts, 6), {10, 10}),
              "return-to-start and close rows sit on the first anchor");
        check(near(jpt(cp1, 4), {10, 50}) && near(jpt(cp2, 4), {10, 30}),
              "line row uses the anchors as control points");
        check(near(jpt(cp1, 5), {10, 30}) && near(jpt(cp2, 5), {5, 15}),
              "closing row carries the first node's in-handle");
        check(o.value("smooth").toArray().at(0).toInt() == 1
              && o.value("smooth").toArray().at(1).toInt() == 0, "smooth flags");

        const c2d::Element back = c2d::Element::fromJson(o);
        const c2d::PathModel m2 = c2d::Element::pathModel(back);
        check(sameModel(m, m2), "closed bezier path round-trips with identical control points");
        check(m2.subs.first().nodes.at(0).kind == c2d::PathNode::Symmetric
              && m2.subs.first().nodes.at(1).kind == c2d::PathNode::Corner
              && m2.subs.first().nodes.at(2).kind == c2d::PathNode::Smooth,
              "node kinds survive the round trip");
        check(back.painterPath.elementCount() == m.painterPath().elementCount()
              && near(back.painterPath.pointAtPercent(0.37), m.painterPath().pointAtPercent(0.37), 1e-6),
              "loaded painter path matches the model's curve");
        // Re-encoding the decoded model reproduces the JSON byte for byte.
        const c2d::Element again = c2d::Element::withPathModel(back, m2);
        check(again.raw == back.raw, "decode/encode is idempotent");
    }

    // --- open path, and lines-only paths match makePath -----------------------
    {
        c2d::PathModel m = specimen(false);
        // An open path has no segment arriving at its first node or leaving
        // its last, so those two handles have no home in CC's schema.
        m.subs.first().nodes.first().in = m.subs.first().nodes.first().p;
        m.subs.first().nodes.last().out = m.subs.first().nodes.last().p;
        const c2d::Element e = c2d::Element::makeBezierPath(m, layer);
        const QJsonObject o = QJsonDocument::fromJson(e.toJson()).object();
        check(o.value("points").toArray().size() == 5, "open path has one row per anchor");
        check(sameModel(m, c2d::Element::pathModel(c2d::Element::fromJson(o))),
              "open bezier path round-trips");

        c2d::SubPath s;
        for (const QPointF &p : {QPointF(400, 80), QPointF(430, 130), QPointF(460, 80)})
            s.nodes.append(node(p, p, p, c2d::PathNode::Corner));
        s.closed = true;
        c2d::PathModel lines;
        lines.subs.append(s);
        const c2d::Element viaModel = c2d::Element::makeBezierPath(lines, layer);
        const c2d::Element viaLegacy =
            c2d::Element::makePath({{400, 80}, {430, 130}, {460, 80}}, true, layer);
        for (const char *k : {"points", "cp1", "cp2", "point_type", "smooth"})
            check(viaModel.raw.value(QLatin1String(k)) == viaLegacy.raw.value(QLatin1String(k)),
                  "polyline encoding matches makePath");
    }

    // --- circle / rectangle decode; edited shapes become paths ---------------
    {
        const c2d::Element c = c2d::Element::makeCircle({50, 50}, 20, layer);
        const c2d::PathModel m = c2d::Element::pathModel(c);
        check(m.subs.size() == 1 && m.subs.first().closed && m.subs.first().nodes.size() == 4,
              "circle decodes to four closed nodes");
        check(near(m.subs.first().nodes.at(0).p, {30, 50}) && m.subs.first().nodes.at(0).kind == c2d::PathNode::Symmetric,
              "circle nodes are absolute and symmetric");
        check(near(m.painterPath().boundingRect().topLeft(), {30, 30}, 1e-3)
              && near(m.painterPath().boundingRect().bottomRight(), {70, 70}, 1e-3),
              "circle model reproduces the circle");
        const QVector<c2d::Element> paths = c2d::Element::toPaths(c);
        check(paths.size() == 1 && paths.first().geometryType == QLatin1String("path")
              && paths.first().id == c.id, "circle converts to one path keeping its id");
        check(near(paths.first().painterPath.boundingRect().center(), {50, 50}, 1e-6),
              "converted circle stays in place");

        const c2d::Element r = c2d::Element::makeRectangle({30, 20}, 40, 20, layer);
        const c2d::PathModel rm = c2d::Element::pathModel(r);
        check(rm.subs.first().nodes.size() == 4 && rm.subs.first().closed,
              "rectangle decodes to four nodes");
        for (const c2d::PathNode &n : rm.subs.first().nodes)
            check(n.kind == c2d::PathNode::Corner && !n.hasIn() && !n.hasOut(), "rectangle corners");
    }

    // --- insert / delete preserve endpoints and the curve ---------------------
    {
        c2d::PathModel m = specimen(false);
        c2d::SubPath &s = m.subs.first();
        const QPointF first = s.nodes.first().p, last = s.nodes.last().p;
        const QPointF onCurve = s.pointAt(0, 0.4);
        const QPointF later = s.pointAt(1, 0.8);
        const int idx = s.insertNode(0, 0.4);
        check(idx == 1 && s.nodes.size() == 6, "insert adds a node after segment start");
        check(near(s.nodes.at(idx).p, onCurve), "inserted node sits on the curve");
        check(near(s.pointAt(2, 0.8), later, 1e-9), "segments after the insert are untouched");
        check(near(s.nodes.first().p, first) && near(s.nodes.last().p, last),
              "insert keeps the endpoints");
        // The split halves trace the original curve: sample the first half.
        const c2d::SubPath orig = specimen(false).subs.first();
        bool trace = true;
        for (int k = 1; k < 8; ++k) {
            const double u = k / 8.0;
            // First half at u == original at 0.4 * u; second half at u ==
            // original at 0.4 + 0.6 * u.
            if (!near(s.pointAt(0, u), orig.pointAt(0, 0.4 * u), 1e-9)
                || !near(s.pointAt(1, u), orig.pointAt(0, 0.4 + 0.6 * u), 1e-9))
                trace = false;
        }
        check(trace, "split halves trace the original curve");
        int seg; double tt;
        check(orig.closest(s.nodes.at(idx).p, &seg, &tt) < 1e-3 && seg == 0 && std::fabs(tt - 0.4) < 1e-3,
              "closest() finds the segment and parameter");

        check(s.removeNode(idx), "remove interior node");
        check(s.nodes.size() == 5 && near(s.nodes.first().p, first) && near(s.nodes.last().p, last),
              "delete keeps the endpoints");
        check(s.removeNode(0) && s.nodes.size() == 4, "remove first node allowed while > 2 nodes");
        c2d::SubPath two;
        two.nodes.append(node({0, 0}, {0, 0}, {0, 0}, c2d::PathNode::Corner));
        two.nodes.append(node({5, 0}, {5, 0}, {5, 0}, c2d::PathNode::Corner));
        check(!two.removeNode(0), "a path never drops below two nodes");

        // Closed path: insert on the closing segment (last -> first).
        c2d::PathModel cm = specimen(true);
        c2d::SubPath &cs = cm.subs.first();
        const int closing = cs.segmentCount() - 1;
        const QPointF mid = cs.pointAt(closing, 0.5);
        cs.insertNode(closing, 0.5);
        check(cs.nodes.size() == 6 && near(cs.nodes.last().p, mid), "insert on the closing segment");
        const c2d::Element ce = c2d::Element::makeBezierPath(cm, layer);
        check(sameModel(cm, c2d::Element::pathModel(ce)), "closed path with inserted node round-trips");
    }

    // --- node kinds -------------------------------------------------------------
    {
        c2d::PathModel m = specimen(false);
        c2d::SubPath &s = m.subs.first();
        s.setKind(1, c2d::PathNode::Symmetric);
        const c2d::PathNode &n = s.nodes.at(1);
        check(near(n.in - n.p, -(n.out - n.p), 1e-9), "symmetric aligns and equalizes handles");
        s.setKind(4, c2d::PathNode::Smooth);       // last node: only an in-handle exists
        check(s.nodes.at(4).hasIn() && !s.nodes.at(4).hasOut(), "smoothing an endpoint grows one handle");
        s.moveHandle(1, true, {80, 30}, false);
        const c2d::PathNode &n2 = s.nodes.at(1);
        check(near(n2.out, {80, 30}) && near(n2.in, n2.p - (n2.out - n2.p)), "symmetric drag mirrors");
        s.moveHandle(1, true, {90, 10}, true);
        check(near(s.nodes.at(1).out, {90, 10}) && near(s.nodes.at(1).in, n2.in)
              && s.nodes.at(1).kind == c2d::PathNode::Corner, "Alt-drag breaks symmetry");
        s.retract(1);
        check(!s.nodes.at(1).hasIn() && !s.nodes.at(1).hasOut(), "retract pulls both handles in");
    }

    // --- text: straight, arc top, arc bottom, offset -------------------------------
    {
        const double h = 12;
        const c2d::Element t = c2d::Element::makeText(QStringLiteral("PHOBIC"), {100, 100}, h,
                                                      QStringLiteral("Helvetica"), layer);
        check(!t.painterPath.isEmpty() && t.raw.value("rendered").toArray().size() >= 6,
              "straight text renders one contour per glyph");
        const QRectF bb = t.painterPath.boundingRect();
        check(bb.left() > 99 && bb.left() < 103 && bb.top() > 99 && bb.top() < 101
              && bb.height() < h * 1.05 && bb.height() > h * 0.6,
              "straight text sits on its baseline at pos, ascent = font_height");
        // Straight glyph outlines are stored in local space (baseline y = 0).
        const QJsonArray firstContour = t.raw.value("rendered").toArray().at(0).toArray();
        double minY = 1e9;
        for (const QJsonValue &pv : firstContour)
            minY = std::min(minY, pv.toArray().at(1).toDouble());
        check(minY > -1 && minY < 1, "rendered outlines are local to the transform");
        const double width = bb.width();

        // Arc on top: baseline on the circle, glyphs outside, centred on 90 deg.
        const double R = 40;
        const QPointF C(30, -20);
        QJsonObject arc;
        arc.insert("arc_enabled", true);
        arc.insert("arc_radius", R);
        arc.insert("arc_center", QJsonArray{C.x(), C.y()});
        const c2d::Element top = c2d::Element::regenText(t, arc);
        check(top.id == t.id && top.raw.value("arc_enabled").toBool(), "arc regen keeps identity");
        Polar p = polar(top, C);
        const double span = qRadiansToDegrees(width / R);
        std::fprintf(stderr, "arc top: angles %.1f..%.1f (mean %.1f) radius %.2f..%.2f, expected span %.1f\n",
                     p.minA, p.maxA, p.meanA, p.minR, p.maxR, span);
        check(std::fabs(p.meanA - 90) < 2.0, "arc text is centred on the top of the circle");
        check(p.minR > R - 0.5 && p.maxR < R + h + 0.5, "arc text baseline sits on the arc, glyphs outside");
        check(std::fabs((p.maxA - p.minA) - span) < 6.0, "arc text spans width / radius");
        // Glyph bbox is centred on arc_center in X (in absolute coordinates the
        // transform's translation applies).
        const QRectF abb = top.painterPath.boundingRect();
        check(std::fabs(abb.center().x() - (C.x() + 100)) < 0.5, "arc glyph bbox centred on arc_center.x");
        check(abb.top() > C.y() + 100 + R * std::cos(qDegreesToRadians(span / 2)) - h - 0.5,
              "arc bbox bottom matches the arc's chord");

        // Bottom: centred on 270 deg, glyphs inside the circle.
        arc.insert("arc_text_on_bottom", true);
        const c2d::Element bottom = c2d::Element::regenText(t, arc);
        p = polar(bottom, C);
        check(std::fabs(p.meanA - 270) < 2.0, "bottom text is centred on the bottom of the circle");
        check(p.maxR < R + 0.5 && p.minR > R - h - 0.5, "bottom text hangs inside the arc");
        // Reading order: the first glyph ("P") is at the smaller angle
        // (left when viewed upright).
        Polar pFirst = polar(c2d::Element::fromJson([&] {
            QJsonObject o = bottom.raw;
            QJsonArray one; one.append(o.value("rendered").toArray().at(0));
            o.insert("rendered", one); return o; }()), C);
        check(pFirst.meanA < 270, "bottom text reads left to right");

        // Offset turns the whole string.
        arc.insert("arc_text_on_bottom", false);
        arc.insert("arc_angle_offset", 35);
        p = polar(c2d::Element::regenText(t, arc), C);
        check(std::fabs(p.meanA - 125) < 2.0, "arc_angle_offset rotates the text");

        // Font edits rewrite font / qtfont and re-render.
        QJsonObject fontEdit;
        fontEdit.insert("bold", true);
        fontEdit.insert("family", QStringLiteral("DejaVu Sans"));
        const c2d::Element bold = c2d::Element::regenText(t, fontEdit);
        check(bold.raw.value("font").toString() == QLatin1String("DejaVu Sans-Bold")
              && c2d::Element::textFont(bold.raw).bold(), "bold/family edit updates the face");
        check(bold.raw.value("qtfont").toString().startsWith(QLatin1String("DejaVu Sans,100,")),
              "qtfont written at 100 pt like CC");

        // A text without `rendered` is laid out on load.
        QJsonObject bare = t.raw;
        bare.remove("rendered");
        check(!c2d::Element::fromJson(bare).painterPath.isEmpty(), "missing rendered is regenerated");

        // Convert to paths: one closed polygon per contour, first keeps the id.
        const QVector<c2d::Element> glyphs = c2d::Element::toPaths(t);
        check(glyphs.size() == t.raw.value("rendered").toArray().size()
              && glyphs.first().id == t.id && glyphs.at(1).id != t.id,
              "text converts to one path per contour");
        check(c2d::Element::pathModel(glyphs.first()).subs.first().closed, "glyph paths are closed");
    }

    // --- optional: write a specimen document --------------------------------------
    if (argc > 2) {
        c2d::Document doc;
        QString err;
        check(doc.load(QString::fromLocal8Bit(argv[1]), &err), "load sample");
        const QJsonObject l = doc.defaultLayer();
        c2d::PathModel m = specimen(true);
        for (c2d::PathNode &n : m.subs.first().nodes) {
            n.p += QPointF(60, 320); n.in += QPointF(60, 320); n.out += QPointF(60, 320);
        }
        doc.addElement(c2d::Element::makeBezierPath(m, l));
        c2d::Element str = c2d::Element::makeText(QStringLiteral("ARC TEXT"), {160, 330}, 14,
                                                  QStringLiteral("Helvetica"), l);
        doc.addElement(str);
        QJsonObject arc;
        arc.insert("arc_enabled", true);
        arc.insert("arc_radius", 45.0);
        arc.insert("arc_center", QJsonArray{45.0, -20.0});
        c2d::Element top = c2d::Element::regenText(str, arc);
        top.translate(150, 0);
        top.raw.insert("id", QUuid::createUuid().toString());
        doc.addElement(c2d::Element::fromJson(top.raw));
        arc.insert("arc_text_on_bottom", true);
        c2d::Element bot = c2d::Element::regenText(str, arc);
        bot.translate(300, 40);
        bot.raw.insert("id", QUuid::createUuid().toString());
        doc.addElement(c2d::Element::fromJson(bot.raw));
        check(doc.save(QString::fromLocal8Bit(argv[2]), &err), "save specimen");
        c2d::Document back;
        check(back.load(QString::fromLocal8Bit(argv[2]), &err), "reload specimen");
        int arcs = 0;
        for (const c2d::Element &e : back.elements())
            if (e.raw.value("arc_enabled").toBool())
                ++arcs;
        check(arcs == 2, "arc texts survive save/load");
        std::fprintf(stderr, "specimen written: %s\n", argv[2]);
    }

    std::printf("OK: %d checks passed\n", g_checks);
    return 0;
}
