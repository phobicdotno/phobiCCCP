// Headless checks for the SVG / DXF importers: element counts, bounding
// boxes in mm (hand-computed from the fixtures in tests/data), CC row-model
// schema, placement rule, and the `d` grammar / arc conversion.
//
// Optional: `test_import <in.c2d> <out.c2d>` additionally imports every
// fixture into the given document and saves it, for a visual check with
// `phobicccp --shot out.c2d out.png`. Plain asserts; exits 0 on success.

#include "../src/c2ddocument.h"
#include "../src/element.h"
#include "../src/importers.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>

#include <cmath>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QFile>
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

static bool near(double a, double b, double eps = 1e-3)
{
    return std::fabs(a - b) < eps;
}

static bool rectNear(const QRectF &r, double x0, double y0, double x1, double y1, double eps = 1e-3)
{
    const bool ok = near(r.left(), x0, eps) && near(r.top(), y0, eps)
                    && near(r.right(), x1, eps) && near(r.bottom(), y1, eps);
    if (!ok)
        std::fprintf(stderr, "  bbox = (%.4f, %.4f)-(%.4f, %.4f), expected (%.4f, %.4f)-(%.4f, %.4f)\n",
                     r.left(), r.top(), r.right(), r.bottom(), x0, y0, x1, y1);
    return ok;
}

static QString fixture(const char *name)
{
    return QDir(QStringLiteral(TEST_DATA_DIR)).filePath(QLatin1String(name));
}

static c2d::ImportOptions opts(double w, double h)
{
    c2d::ImportOptions o;
    o.stockWidth = w;
    o.stockHeight = h;
    QJsonObject l;
    l.insert("blue", 0); l.insert("green", 0); l.insert("red", 0);
    l.insert("locked", false); l.insert("name", QStringLiteral("DEFAULT"));
    l.insert("uuid", QString()); l.insert("visible", true);
    o.layer = l;
    return o;
}

// Every imported element must look exactly like a CC path row model.
static void checkSchema(const c2d::ImportResult &r, const char *tag)
{
    for (const c2d::Element &e : r.elements) {
        const QJsonObject &o = e.raw;
        check(o.value("geometryType").toString() == QLatin1String("path"), tag);
        check(o.value("behavior").toInt() == 0, tag);
        check(o.value("position").toArray() == QJsonArray({0, 0}), tag);
        check(o.contains("layer") && o.contains("tabs") && o.contains("group_id"), tag);
        check(o.value("id").toString().startsWith(QLatin1Char('{')), tag);
        const int n = o.value("points").toArray().size();
        check(n >= 2, tag);
        check(o.value("cp1").toArray().size() == n, tag);
        check(o.value("cp2").toArray().size() == n, tag);
        check(o.value("point_type").toArray().size() == n, tag);
        check(o.value("smooth").toArray().size() == n, tag);
        const QJsonArray pt = o.value("point_type").toArray();
        check(pt.first().toInt() == 0, tag);
        for (int i = 1; i < n; ++i) {
            const int t = pt.at(i).toInt();
            check(t == 1 || t == 3 || (t == 4 && i == n - 1), tag);
        }
        check(!e.painterPath.isEmpty(), tag);
    }
}

// IMPORT_DUMP=1 prints every result (handy when a bbox assertion fails).
static void dump(const c2d::ImportResult &r, const char *tag)
{
    if (!std::getenv("IMPORT_DUMP"))
        return;
    std::printf("== %s: %s\n", tag, qPrintable(r.summary()));
    for (const QString &n : r.notes)
        std::printf("   note: %s\n", qPrintable(n));
    for (const c2d::Element &e : r.elements) {
        const QRectF b = e.painterPath.boundingRect();
        QString types;
        for (const QJsonValue &v : e.raw.value("point_type").toArray())
            types += QString::number(v.toInt());
        std::printf("   el bbox (%.4f, %.4f)-(%.4f, %.4f) types %s\n", b.left(), b.top(),
                    b.right(), b.bottom(), qPrintable(types));
    }
}

static int countType(const c2d::Element &e, int type)
{
    int c = 0;
    for (const QJsonValue &v : e.raw.value("point_type").toArray())
        if (v.toInt() == type) ++c;
    return c;
}

static bool isClosed(const c2d::Element &e)
{
    const QJsonArray pt = e.raw.value("point_type").toArray();
    return !pt.isEmpty() && pt.last().toInt() == 4;
}

// The O(n^2) joinSubpaths as it was before its endpoints were indexed: the
// indexed one must make exactly the same path.
static bool samePt(const QPointF &a, const QPointF &b, double eps)
{
    return std::fabs(a.x() - b.x()) < eps && std::fabs(a.y() - b.y()) < eps;
}

static QPainterPath joinSubpathsSlow(const QPainterPath &p, double tol)
{
    struct Seg { bool curve; QPointF a, c1, c2, b; };
    struct Sub { QVector<Seg> segs; bool closed; };
    QVector<Sub> subs;
    QPointF start;
    for (int i = 0; i < p.elementCount(); ++i) {
        const QPainterPath::Element e = p.elementAt(i);
        if (e.isMoveTo()) {
            start = QPointF(e.x, e.y);
            subs.append({{}, false});
        } else if (subs.isEmpty()) {
            continue;
        } else if (e.isLineTo()) {
            const QPointF prev = subs.last().segs.isEmpty() ? start : subs.last().segs.last().b;
            subs.last().segs.append({false, prev, prev, QPointF(e.x, e.y), QPointF(e.x, e.y)});
        } else if (e.isCurveTo() && i + 2 < p.elementCount()) {
            const QPointF prev = subs.last().segs.isEmpty() ? start : subs.last().segs.last().b;
            const QPainterPath::Element c2 = p.elementAt(i + 1), end = p.elementAt(i + 2);
            subs.last().segs.append({true, prev, QPointF(e.x, e.y), QPointF(c2.x, c2.y),
                                     QPointF(end.x, end.y)});
            i += 2;
        }
    }
    for (Sub &s : subs)
        s.closed = !s.segs.isEmpty() && samePt(s.segs.first().a, s.segs.last().b, tol);

    auto reversed = [](QVector<Seg> v) {
        std::reverse(v.begin(), v.end());
        for (Seg &g : v) {
            std::swap(g.a, g.b);
            std::swap(g.c1, g.c2);
        }
        return v;
    };
    QVector<bool> used(subs.size(), false);
    QPainterPath out;
    auto write = [&out](const QVector<Seg> &segs, bool close) {
        out.moveTo(segs.first().a);
        for (const Seg &g : segs)
            if (g.curve)
                out.cubicTo(g.c1, g.c2, g.b);
            else
                out.lineTo(g.b);
        if (close)
            out.closeSubpath();
    };
    for (int i = 0; i < subs.size(); ++i) {
        if (used[i] || subs[i].segs.isEmpty())
            continue;
        used[i] = true;
        if (subs[i].closed) {
            QVector<Seg> segs = subs[i].segs;
            if (segs.size() > 1)
                segs.last().b = segs.first().a;
            write(segs, segs.size() > 1);
            continue;
        }
        QVector<Seg> chain = subs[i].segs;
        for (bool grew = true; grew;) {
            grew = false;
            for (int j = 0; j < subs.size(); ++j) {
                if (used[j] || subs[j].closed || subs[j].segs.isEmpty())
                    continue;
                const QVector<Seg> &o = subs[j].segs;
                if (samePt(chain.last().b, o.first().a, tol)) {
                    chain += o;
                } else if (samePt(chain.last().b, o.last().b, tol)) {
                    chain += reversed(o);
                } else if (samePt(chain.first().a, o.last().b, tol)) {
                    chain = o + chain;
                } else if (samePt(chain.first().a, o.first().a, tol)) {
                    chain = reversed(o) + chain;
                } else {
                    continue;
                }
                used[j] = true;
                grew = true;
            }
        }
        const bool close = chain.size() > 1 && samePt(chain.first().a, chain.last().b, tol);
        if (close)
            chain.last().b = chain.first().a;
        write(chain, close);
    }
    return out;
}

static bool samePath(const QPainterPath &a, const QPainterPath &b)
{
    if (a.elementCount() != b.elementCount())
        return false;
    for (int i = 0; i < a.elementCount(); ++i) {
        const QPainterPath::Element x = a.elementAt(i), y = b.elementAt(i);
        if (x.type != y.type || x.x != y.x || x.y != y.y)
            return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // ---- path grammar + arc conversion -----------------------------------
    {
        bool ok = false;
        QPainterPath p = c2d::parseSvgPathData(
            QStringLiteral("M10 10h20a10,10 0 0 1 0,20H10z"), &ok);
        check(ok, "d grammar parses");
        check(rectNear(p.boundingRect(), 10, 10, 40, 30, 1e-9),
              "arc bulges to +x (sweep=1 in Y-down), endpoint exact");
        check(p.elementCount() == 1 + 1 + 3 * 2 + 1 + 1, "arc of 180 deg = 2 cubics");
        p = c2d::parseSvgPathData(
            QStringLiteral("M10,10 m40-5 c5,0 5,10 10,10 s5 10 10 10 q5 5 10 0 t10 0 v-20 "
                           "l-30 0 A5 5 0 1 0 60 5 Z"), &ok);
        check(ok, "d grammar: relative/smooth/quadratic/arc commands parse");
        check(near(p.currentPosition().x(), 50) && near(p.currentPosition().y(), 5),
              "Z returns to the subpath start");
        QPainterPath bad = c2d::parseSvgPathData(QStringLiteral("M0 0 L 10 10 X 5"), &ok);
        check(!ok && bad.elementCount() == 2, "unknown command stops parsing, keeps prefix");
        c2d::parseSvgPathData(QStringLiteral("M0 0 L10 10 Z 5 5"), &ok);
        check(!ok, "numbers after Z are rejected (no infinite loop)");

        // Full circle through appendArc: 4 beziers, exact extremes.
        QPainterPath c;
        c2d::appendArc(c, QPointF(5, 5), 3, 3, 0, 0, 2 * M_PI, true);
        check(rectNear(c.boundingRect(), 2, 2, 8, 8, 1e-9), "appendArc circle bbox");
        check(c.elementCount() == 1 + 4 * 3, "appendArc: 4 cubic segments");
    }

    // ---- SVG 1: beziers + transforms, mm page ----------------------------
    {
        const c2d::ImportResult r = c2d::importFile(fixture("bezier_transform.svg"), opts(200, 200));
        check(r.ok, "svg1 ok");
        checkSchema(r, "svg1 schema"); dump(r, "svg1");
        check(r.elements.size() == 2, "svg1: 2 elements (path + rect)");
        check(r.skipped == 2, "svg1: text + gradient reported as skipped");
        check(!r.relocated, "svg1 fits the 200x200 stock: not moved");
        check(rectNear(r.bounds, 10, 10, 80, 70), "svg1 bbox mm (Y flipped)");
        // The cubic survives as a type-3 row; closed contour ends with a 4.
        const c2d::Element &path = r.elements.at(0);
        check(countType(path, 3) == 1 && isClosed(path), "svg1 path keeps its bezier + close row");
        check(rectNear(path.painterPath.boundingRect(), 10, 30, 50, 70), "svg1 path bbox");
        const c2d::Element &rect = r.elements.at(1);
        check(countType(rect, 3) == 4 && isClosed(rect), "svg1 rounded rect: 4 corner beziers");
        check(rectNear(rect.painterPath.boundingRect(), 70, 10, 80, 30), "svg1 rotated rect bbox");
        // cp1/cp2 of a line row: previous anchor / self (CC's convention).
        const QJsonObject &o = path.raw;
        const QJsonArray pts = o.value("points").toArray(), cp1 = o.value("cp1").toArray(),
                         cp2 = o.value("cp2").toArray(), pt = o.value("point_type").toArray();
        for (int i = 1; i < pts.size(); ++i)
            if (pt.at(i).toInt() == 1)
                check(cp1.at(i) == pts.at(i - 1) && cp2.at(i) == pts.at(i), "svg1 line row cps");
    }

    // ---- SVG 2: inches + placement ---------------------------------------
    {
        c2d::ImportResult r = c2d::importFile(fixture("inch_shapes.svg"), opts(200, 200));
        check(r.ok && r.elements.size() == 2 && r.skipped == 0, "svg2: rect + circle");
        checkSchema(r, "svg2 schema"); dump(r, "svg2");
        check(!r.relocated, "svg2 fits");
        check(rectNear(r.bounds, 2.54, 2.54, 48.26, 22.86), "svg2 bbox in mm from inches");
        check(rectNear(r.elements.at(1).painterPath.boundingRect(), 27.94, 2.54, 48.26, 22.86),
              "svg2 circle bbox");
        check(countType(r.elements.at(1), 3) == 4, "svg2 circle is 4 beziers");

        r = c2d::importFile(fixture("inch_shapes.svg"), opts(20, 20));
        check(r.ok && r.relocated, "svg2 on a 20x20 stock is moved");
        check(rectNear(r.bounds, 10, 10, 55.72, 30.32), "svg2 relocated bbox at (10,10)");

        r = c2d::importFile(fixture("inch_shapes.svg"), opts(0, 0));
        check(r.ok && r.relocated && near(r.bounds.left(), 10) && near(r.bounds.top(), 10),
              "svg2 unknown stock: always moved to (10,10)");
    }

    // ---- SVG 3: px units, relative commands, arcs, polygon, line ---------
    {
        const c2d::ImportResult r = c2d::importFile(fixture("px_arcs.svg"), opts(200, 200));
        check(r.ok, "svg3 ok");
        checkSchema(r, "svg3 schema"); dump(r, "svg3");
        check(r.elements.size() == 3 && r.skipped == 1, "svg3: path + polygon + line, image skipped");
        const double px = 25.4 / 96;
        check(rectNear(r.bounds, 0, 25.4 - 90 * px, 90 * px, 25.4), "svg3 bbox (px @ 96 dpi)");
        check(rectNear(r.elements.at(0).painterPath.boundingRect(), 10 * px, 25.4 - 30 * px,
                       40 * px, 25.4 - 10 * px), "svg3 arc path bbox");
        check(isClosed(r.elements.at(0)) && isClosed(r.elements.at(1)) && !isClosed(r.elements.at(2)),
              "svg3 closed flags");
    }

    // ---- DXF 1: LWPOLYLINE bulge + LINE (mm) -----------------------------
    {
        const c2d::ImportResult r = c2d::importFile(fixture("lwpoly_bulge.dxf"), opts(200, 200));
        check(r.ok, "dxf1 ok");
        checkSchema(r, "dxf1 schema"); dump(r, "dxf1");
        check(r.elements.size() == 2 && r.skipped == 0, "dxf1: polyline + line");
        check(!r.relocated, "dxf1 fits (touches the origin)");
        check(rectNear(r.bounds, 0, 0, 50, 40), "dxf1 bbox");
        const c2d::Element &poly = r.elements.at(0);
        check(isClosed(poly) && countType(poly, 3) == 2 && countType(poly, 1) == 3,
              "dxf1 bulge = two 90-degree beziers, three lines, closed");
        check(rectNear(poly.painterPath.boundingRect(), 0, 0, 50, 20), "dxf1 polyline bbox");
        check(!isClosed(r.elements.at(1)), "dxf1 line is open");
    }

    // ---- DXF 2: CIRCLE + ARC in inches -----------------------------------
    {
        c2d::ImportResult r = c2d::importFile(fixture("circle_arc_inch.dxf"), opts(200, 200));
        check(r.ok, "dxf2 ok");
        checkSchema(r, "dxf2 schema"); dump(r, "dxf2");
        check(r.elements.size() == 2 && r.skipped == 0, "dxf2: circle + arc (POINT silent)");
        check(rectNear(r.bounds, 12.7, 12.7, 101.6, 50.8), "dxf2 bbox ($INSUNITS=1 -> 25.4)");
        check(isClosed(r.elements.at(0)) && countType(r.elements.at(0), 3) == 4, "dxf2 circle");
        check(!isClosed(r.elements.at(1)) && countType(r.elements.at(1), 3) == 1, "dxf2 arc: 1 bezier");
        check(rectNear(r.elements.at(1).painterPath.boundingRect(), 76.2, 25.4, 101.6, 50.8),
              "dxf2 arc bbox");
        r = c2d::importFile(fixture("circle_arc_inch.dxf"), opts(50, 50));
        check(r.ok && r.relocated && rectNear(r.bounds, 10, 10, 98.9, 48.1), "dxf2 relocated");
    }

    // ---- DXF 3: SPLINE, BLOCK/INSERT, TEXT skipped -----------------------
    {
        const c2d::ImportResult r = c2d::importFile(fixture("spline_block.dxf"), opts(200, 200));
        check(r.ok, "dxf3 ok");
        checkSchema(r, "dxf3 schema"); dump(r, "dxf3");
        check(r.elements.size() == 3, "dxf3: spline + 2 inserted circles");
        check(r.skipped == 1 && r.notes.join(' ').contains(QLatin1String("TEXT")),
              "dxf3: TEXT skipped and reported");
        check(rectNear(r.bounds, 0, 0, 110, 110), "dxf3 bbox");
        const c2d::Element &sp = r.elements.at(0);
        check(countType(sp, 3) == 0 && countType(sp, 1) >= 12 && !isClosed(sp),
              "dxf3 spline flattened to lines");
        const QRectF sb = sp.painterPath.boundingRect();
        check(near(sb.left(), 0) && near(sb.right(), 40) && near(sb.top(), 0)
              && sb.bottom() <= 22.5 + 1e-6 && sb.bottom() >= 22.5 - 0.03,
              "dxf3 spline peak within 0.02 mm of the exact bezier");
        check(rectNear(r.elements.at(1).painterPath.boundingRect(), 90, 40, 110, 60),
              "dxf3 INSERT scaled 2x at (100,50)");
        check(rectNear(r.elements.at(2).painterPath.boundingRect(), 90, 90, 110, 110),
              "dxf3 rotated INSERT at (100,100)");
    }

    // ---- error paths -----------------------------------------------------
    {
        c2d::ImportResult r = c2d::importSvgData("<html/>", opts(0, 0), QStringLiteral("x"));
        check(!r.ok, "non-svg root rejected");
        // A SPLINE carrying fit-point X codes with no matching Y codes:
        // malformed but readable. It must be dropped, not dereferenced.
        {
            const QByteArray dxf =
                "0\nSECTION\n2\nENTITIES\n"
                "0\nSPLINE\n8\n0\n70\n8\n71\n3\n74\n3\n"
                "11\n0.0\n11\n10.0\n11\n20.0\n"
                "0\nLINE\n8\n0\n10\n0.0\n20\n0.0\n11\n5.0\n21\n5.0\n"
                "0\nENDSEC\n0\nEOF\n";
            const c2d::ImportResult bad = c2d::importDxfData(dxf, opts(0, 0),
                                                             QStringLiteral("halfspline"));
            check(bad.ok, "dxf: half a fit-point spline still imports the rest");
            check(bad.elements.size() == 1, "dxf: the unusable spline is dropped, the line kept");
        }
        // Four LINEs that make a square (in any order and direction) plus one
        // loose line: the square comes back as one closed path.
        {
            const QByteArray dxf =
                "0\nSECTION\n2\nENTITIES\n"
                "0\nLINE\n8\n0\n10\n0\n20\n0\n11\n10\n21\n0\n"
                "0\nLINE\n8\n0\n10\n0\n20\n10\n11\n0\n21\n0\n"
                "0\nLINE\n8\n0\n10\n10\n20\n10\n11\n10\n21\n0\n"
                "0\nLINE\n8\n0\n10\n0\n20\n10\n11\n10\n21\n10\n"
                "0\nLINE\n8\n0\n10\n30\n20\n30\n11\n40\n21\n35\n"
                "0\nENDSEC\n0\nEOF\n";
            const c2d::ImportResult sq = c2d::importDxfData(dxf, opts(0, 0), QStringLiteral("square"));
            check(sq.ok && sq.elements.size() == 2, "dxf: four touching lines join into one shape");
            check(isClosed(sq.elements.at(0)) && countType(sq.elements.at(0), 1) == 4,
                  "dxf: the joined square is closed with four sides");
            check(!isClosed(sq.elements.at(1)), "dxf: the loose line stays open");
        }
        {
            // A MINSERT array turns with the insert.
            const QByteArray dxf =
                "0\nSECTION\n2\nBLOCKS\n0\nBLOCK\n2\nSQ\n10\n0\n20\n0\n"
                "0\nLWPOLYLINE\n90\n4\n70\n1\n10\n0\n20\n0\n10\n10\n20\n0\n10\n10\n20\n10\n10\n0\n20\n10\n"
                "0\nENDBLK\n0\nENDSEC\n0\nSECTION\n2\nENTITIES\n"
                "0\nINSERT\n2\nSQ\n10\n0\n20\n0\n50\n90\n70\n3\n71\n1\n44\n20\n45\n0\n"
                "0\nENDSEC\n0\nEOF\n";
            const c2d::ImportResult m = c2d::importDxfData(dxf, opts(0, 0), QStringLiteral("minsert"));
            check(m.ok && m.elements.size() == 3 && std::abs(m.bounds.width() - 10.0) < 0.1
                      && std::abs(m.bounds.height() - 50.0) < 0.1, "dxf: a rotated MINSERT array turns with it");
        }
        {
            const QByteArray dxf = QByteArray("\xEF\xBB\xBF") +
                "0\nSECTION\n2\nENTITIES\n0\nLINE\n8\n0\n10\n0\n20\n0\n11\n10\n21\n0\n0\nENDSEC\n0\nEOF\n";
            const c2d::ImportResult b = c2d::importDxfData(dxf, opts(0, 0), QStringLiteral("bom"));
            check(b.ok && b.elements.size() == 1, "dxf: a UTF-8 byte-order mark is skipped");
        }
        {
            // No viewBox and only a width (or height): user units stay px,
            // the content box is not stretched to the page.
            for (const char *attr : {"width='100mm'", "height='100mm'"}) {
                const c2d::ImportResult w = c2d::importSvgData(
                    QByteArray("<svg xmlns='http://www.w3.org/2000/svg' ") + attr
                        + "><rect width='10' height='10'/></svg>", opts(0, 0), QStringLiteral("wonly"));
                check(w.ok && std::abs(w.bounds.width() - 10 * 25.4 / 96) < 0.01
                          && std::abs(w.bounds.height() - 10 * 25.4 / 96) < 0.01,
                      "svg: width or height alone without a viewBox keeps px user units");
            }
        }
        {
            // A percentage with only a width on the root is of that width;
            // "defer" before the alignment does not centre the drawing.
            const c2d::ImportResult p = c2d::importSvgData(
                "<svg xmlns='http://www.w3.org/2000/svg' width='100mm'><rect width='100%' height='10'/></svg>",
                opts(0, 0), QStringLiteral("pct"));
            check(p.ok && std::abs(p.bounds.width() - 100) < 0.01, "svg: a width percentage with only a root width");
            auto left = [&](const char *par) {
                const c2d::ImportResult d = c2d::importSvgData(
                    QByteArray("<svg xmlns='http://www.w3.org/2000/svg' width='200mm' height='100mm' viewBox='0 0 10 10' "
                               "preserveAspectRatio='") + par + "'><rect width='10' height='10'/></svg>",
                    opts(0, 0), QStringLiteral("defer"));
                return d.ok ? d.bounds.left() : -1.0;
            };
            check(std::abs(left("defer xMinYMin meet") - left("xMinYMin meet")) < 1e-6,
                  "svg: preserveAspectRatio 'defer' is skipped");
            // A symbol <use>d under a root with only a width stays at 0, not
            // centred in a strip as wide as the root.
            auto useLeft = [&](const char *root) {
                const c2d::ImportResult u = c2d::importSvgData(
                    QByteArray("<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink'") + root
                        + "><symbol id='s' viewBox='0 0 10 10'><rect width='10' height='10'/></symbol>"
                          "<use xlink:href='#s'/><rect x='0' y='0' width='1' height='1'/></svg>",
                    opts(0, 0), QStringLiteral("use"));
                return u.ok ? u.bounds.width() : -1.0;
            };
            // The 1 px rect at 0 pins the left edge: the width is the symbol's reach.
            check(std::abs(useLeft(" width='100mm'") - useLeft("")) < 1e-6,
                  "svg: a symbol under a width-only root is not shifted");
            // A <use> of a nested <svg> keeps that svg's place and viewBox.
            const c2d::ImportResult ns = c2d::importSvgData(
                "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' width='100mm' "
                "height='100mm' viewBox='0 0 100 100'><defs><svg id='n' x='10' y='10' width='20' height='20' "
                "viewBox='0 0 10 10'><rect width='10' height='10'/></svg></defs><use xlink:href='#n' x='50'/></svg>",
                opts(0, 0), QStringLiteral("nested"));
            check(ns.ok && std::abs(ns.bounds.width() - 20) < 0.01, "svg: a used nested svg keeps its size");
            auto usedW = [&](const char *body) {
                const c2d::ImportResult u = c2d::importSvgData(
                    QByteArray("<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' "
                               "width='100mm' height='100mm' viewBox='0 0 100 100'>") + body + "</svg>",
                    opts(0, 0), QStringLiteral("nested"));
                return u.ok ? u.bounds.width() : -1.0;
            };
            // % inside it is of its own viewport, also nested in nested...
            check(std::abs(usedW("<defs><svg id='n' x='10' y='10' width='20' height='20' viewBox='0 0 10 10'>"
                                 "<rect width='100%' height='100%'/></svg></defs><use xlink:href='#n'/>") - 20) < 0.01,
                  "svg: % lengths in a used nested svg are of its viewport");
            check(std::abs(usedW("<defs><svg id='o' width='50' height='50' viewBox='0 0 10 10'><svg viewBox='0 0 1 1'>"
                                 "<rect width='1' height='1'/></svg></svg></defs><use xlink:href='#o'/>") - 50) < 0.01,
                  "svg: a used svg nested in a nested svg");
            // The nested svg's own transform applies outside its viewport.
            check(std::abs(usedW("<rect width='1' height='1'/><defs><svg id='t' transform='translate(60,0)' x='10' "
                                 "y='10' width='20' height='20' viewBox='0 0 10 10'><rect width='10' height='10'/>"
                                 "</svg></defs><use xlink:href='#t'/>") - 90) < 0.01,
                  "svg: a used nested svg keeps its own transform");
            // ...and the <use>'s width and height resize it.
            check(std::abs(usedW("<defs><svg id='q' width='20' height='20' viewBox='0 0 10 10'><rect width='10' "
                                 "height='10'/></svg></defs><use xlink:href='#q' width='40' height='40'/>") - 40) < 0.01,
                  "svg: a use's width and height resize a nested svg");
        }
        {
            // <use> places what it refers to (pdftocairo's text, clones).
            const c2d::ImportResult u = c2d::importSvgData(
                "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' "
                "width='100mm' height='100mm' viewBox='0 0 100 100'>"
                "<defs><symbol id='g1' overflow='visible'><path d='M0 0 L10 0 L10 10 Z'/></symbol></defs>"
                "<rect x='0' y='0' width='5' height='5'/>"
                "<use xlink:href='#g1' x='50' y='50'/><use href='#g1' x='70' y='50' transform='translate(0 20)'/>"
                "</svg>", opts(0, 0), QStringLiteral("use"));
            check(u.ok && u.elements.size() == 3, "svg: each <use> draws what it refers to");
            check(std::abs(u.bounds.width() - 80.0) < 0.1 && std::abs(u.bounds.height() - 80.0) < 0.1,
                  "svg: placed at its x,y inside its transform");
        }
        {
            // A <use> of a group leaves out its hidden children; a <use> of a
            // <symbol viewBox> fits it into the use's width x height.
            const c2d::ImportResult h = c2d::importSvgData(
                "<svg xmlns='http://www.w3.org/2000/svg' width='200mm' height='200mm' viewBox='0 0 200 200'>"
                "<defs><g id='a'><rect width='10' height='10'/>"
                "<rect y='50' width='10' height='10' style='display:none'/></g></defs>"
                "<use href='#a' x='50'/></svg>", opts(0, 0), QStringLiteral("usehidden"));
            check(h.ok && h.elements.size() == 1 && std::abs(h.bounds.height() - 10.0) < 0.1,
                  "svg: a <use> leaves out hidden children");
            const c2d::ImportResult y = c2d::importSvgData(
                "<svg xmlns='http://www.w3.org/2000/svg' width='200mm' height='200mm' viewBox='0 0 200 200'>"
                "<defs><symbol id='i' viewBox='0 0 10 10'><rect width='10' height='10'/></symbol></defs>"
                "<use href='#i' x='20' y='20' width='50' height='50'/></svg>", opts(0, 0), QStringLiteral("usesym"));
            check(y.ok && y.elements.size() == 1 && std::abs(y.bounds.width() - 50.0) < 0.1
                      && std::abs(y.bounds.height() - 50.0) < 0.1,
                  "svg: a symbol's viewBox fits the use's size");
        }
        {
            // A clone leaves out the clip paths and definitions inside what it
            // clones; a <use> may refer to a symbol defined after it, and a
            // percentage inside definitions is of the viewport.
            const c2d::ImportResult c = c2d::importSvgData(
                "<svg xmlns='http://www.w3.org/2000/svg' width='200mm' height='200mm' viewBox='0 0 200 200'>"
                "<defs><symbol id='s' viewBox='0 0 100 100'><g><defs><rect id='C' width='100' height='100'/></defs>"
                "<clipPath id='k'><use href='#C'/></clipPath><rect x='40' y='40' width='20' height='20'/></g></symbol></defs>"
                "<use href='#s' width='100' height='100'/></svg>", opts(0, 0), QStringLiteral("useclip"));
            check(c.ok && c.elements.size() == 1 && std::abs(c.bounds.width() - 20.0) < 0.1,
                  "svg: a clone leaves out inline clip paths");
            const c2d::ImportResult f = c2d::importSvgData(
                "<svg xmlns='http://www.w3.org/2000/svg' width='200mm' height='200mm' viewBox='0 0 200 200'>"
                "<defs><g id='g'><use href='#s' width='50%' height='50%'/></g>"
                "<symbol id='s' viewBox='0 0 10 10'><rect width='10' height='10'/></symbol></defs>"
                "<use href='#g'/></svg>", opts(0, 0), QStringLiteral("usefwd"));
            check(f.ok && f.elements.size() == 1 && std::abs(f.bounds.width() - 100.0) < 0.1,
                  "svg: a symbol defined later, at a percentage of the viewport");
        }
        // Extrusion 0,0,-1 mirrors OCS entities only: a LINE is stored in
        // world coordinates and must stay at +X.
        {
            const QByteArray dxf =
                "0\nSECTION\n2\nENTITIES\n"
                "0\nLINE\n8\n0\n10\n100\n20\n50\n11\n120\n21\n60\n230\n-1\n"
                "0\nENDSEC\n0\nEOF\n";
            const c2d::ImportResult ln = c2d::importDxfData(dxf, opts(200, 200), QStringLiteral("mirline"));
            check(ln.ok && ln.elements.size() == 1, "dxf: mirrored-extrusion line imports");
            check(!ln.relocated && rectNear(ln.bounds, 100, 50, 120, 60), "dxf: a LINE's world coordinates are not flipped");
        }
        // A half ellipse (0..pi) with extrusion 0,0,-1: centre and axis stay
        // put; the minor axis points the other way, so it is the lower half.
        {
            const QByteArray dxf =
                "0\nSECTION\n2\nENTITIES\n"
                "0\nELLIPSE\n8\n0\n10\n100\n20\n50\n11\n20\n21\n0\n40\n0.5\n"
                "41\n0\n42\n3.14159265358979\n230\n-1\n"
                "0\nENDSEC\n0\nEOF\n";
            const c2d::ImportResult el = c2d::importDxfData(dxf, opts(200, 200), QStringLiteral("mirell"));
            check(el.ok && el.elements.size() == 1 && !el.relocated
                      && rectNear(el.bounds, 80, 40, 120, 50, 0.05),
                  "dxf: mirrored-extrusion ellipse arc is the lower half, in place");
        }
        // A spline-fit POLYLINE: the frame (flag 16) is skipped, the fitted
        // vertices (flag 8) are the curve.
        {
            const QByteArray dxf =
                "0\nSECTION\n2\nENTITIES\n"
                "0\nPOLYLINE\n8\n0\n66\n1\n70\n4\n"
                "0\nVERTEX\n8\n0\n10\n0\n20\n0\n70\n16\n"
                "0\nVERTEX\n8\n0\n10\n10\n20\n10\n70\n16\n"
                "0\nVERTEX\n8\n0\n10\n0\n20\n0\n70\n8\n"
                "0\nVERTEX\n8\n0\n10\n5\n20\n4\n70\n8\n"
                "0\nVERTEX\n8\n0\n10\n10\n20\n10\n70\n8\n"
                "0\nSEQEND\n8\n0\n"
                "0\nENDSEC\n0\nEOF\n";
            const c2d::ImportResult sp = c2d::importDxfData(dxf, opts(0, 0), QStringLiteral("splinefit"));
            check(sp.ok && sp.elements.size() == 1, "dxf: a spline-fit polyline keeps its fitted curve");
        }
        r = c2d::importDxfData("garbage\n", opts(0, 0), QStringLiteral("x"));
        check(!r.ok, "non-dxf rejected");
        r = c2d::importSvgData("<svg xmlns='http://www.w3.org/2000/svg'/>", opts(0, 0));
        check(r.ok && r.elements.isEmpty(), "empty svg imports nothing, no error");
        // One path whose ends meet only within the join tolerance: closed.
        r = c2d::importSvgData("<svg xmlns='http://www.w3.org/2000/svg' width='100mm' height='100mm' "
                               "viewBox='0 0 100 100'><path d='M10 10 L50 10 L50 50 L10.005 10'/></svg>",
                               opts(100, 100));
        check(r.ok && r.elements.size() == 1
                  && c2d::Element::pathModel(r.elements.first()).subs.value(0).closed,
              "svg: a path ending within tolerance of its start is closed");
        // A class an internal <style> sheet hides (Illustrator's hidden
        // layer) is hidden, as style='display:none' is; an inline style wins.
        r = c2d::importSvgData("<svg xmlns='http://www.w3.org/2000/svg' width='100mm' height='100mm' "
                               "viewBox='0 0 100 100'><style><![CDATA[ /* x */ .st1{fill:red} "
                               ".st0, .st2{display : none;} ]]></style>"
                               "<g class='a st0'><rect x='0' y='0' width='5' height='5'/></g>"
                               "<rect class='st2' style='display:inline' x='20' y='20' width='5' height='5'/>"
                               "<rect class='st1' x='50' y='50' width='5' height='5'/></svg>", opts(100, 100));
        check(r.ok && r.elements.size() == 2 && rectNear(r.bounds, 20, 45, 55, 80),
              "svg: a class hidden by <style> is not imported");
        // A nested <svg> is a viewport: placed at x,y, its viewBox fitted into width x height.
        r = c2d::importSvgData("<svg xmlns='http://www.w3.org/2000/svg' width='100mm' height='100mm' "
                               "viewBox='0 0 100 100'><svg x='50' y='50' width='50' height='50' "
                               "viewBox='0 0 10 10'><rect width='10' height='10'/></svg>"
                               "<svg x='10' y='10'><rect width='5' height='5'/></svg></svg>", opts(100, 100));
        check(r.ok && r.elements.size() == 2, "svg: nested viewports import");
        check(r.ok && rectNear(r.elements.at(0).painterPath.boundingRect(), 50, 0, 100, 50),
              "svg: nested <svg> viewBox scales into its viewport");
        check(r.ok && rectNear(r.elements.at(1).painterPath.boundingRect(), 10, 85, 15, 90),
              "svg: nested <svg> without a viewBox is moved to x,y");
        // The root's preserveAspectRatio: alignment and slice, not only "none".
        r = c2d::importSvgData("<svg xmlns='http://www.w3.org/2000/svg' width='100mm' height='100mm' "
                               "viewBox='0 0 100 50' preserveAspectRatio='xMinYMin meet'>"
                               "<rect width='100' height='50'/></svg>", opts(100, 100));
        check(r.ok && rectNear(r.bounds, 0, 50, 100, 100),
              "svg: root xMinYMin meet puts the drawing at the top (Y-up)");
        r = c2d::importSvgData("<svg xmlns='http://www.w3.org/2000/svg' width='100mm' height='100mm' "
                               "viewBox='0 0 50 100' preserveAspectRatio='xMaxYMax meet'>"
                               "<rect width='50' height='100'/></svg>", opts(100, 100));
        check(r.ok && rectNear(r.bounds, 50, 0, 100, 100), "svg: root xMaxYMax meet aligns right");
        {
            c2d::ImportOptions fixed = opts(100, 50);
            fixed.autoPlace = false;
            r = c2d::importSvgData("<svg xmlns='http://www.w3.org/2000/svg' width='100mm' height='50mm' "
                                   "viewBox='0 0 100 100' preserveAspectRatio='xMidYMid slice'>"
                                   "<rect width='100' height='100'/></svg>", fixed);
            check(r.ok && rectNear(r.bounds, 0, -25, 100, 75), "svg: root slice fills the page");
        }
        r = c2d::importSvgData("<svg xmlns='http://www.w3.org/2000/svg' width='100mm' height='100mm' "
                               "viewBox='0 0 100 100'><rect width='100' height='50'/></svg>", opts(100, 100));
        check(r.ok && rectNear(r.bounds, 0, 50, 100, 100), "svg: default root fit unchanged");
        // Shape lengths with a unit convert as CSS px; a percentage is of the viewport.
        r = c2d::importSvgData("<svg xmlns='http://www.w3.org/2000/svg' width='100mm' height='100mm' "
                               "viewBox='0 0 100 100'><rect width='10mm' height='1in'/></svg>",
                               opts(100, 100));
        check(r.ok && rectNear(r.bounds, 0, 100 - 96, 10 / (25.4 / 96), 100, 1e-6),
              "svg: rect width='10mm' height='1in' are 37.8 and 96 user units");
        r = c2d::importSvgData("<svg xmlns='http://www.w3.org/2000/svg' width='100mm' height='100mm' "
                               "viewBox='0 0 200 100' preserveAspectRatio='none'>"
                               "<rect x='10%' width='50%' height='20%'/><circle cx='50%' cy='50%' r='5'/>"
                               "<line x1='0' y1='0' x2='12px' y2='0'/></svg>", opts(100, 100));
        check(r.ok && r.elements.size() == 3
                  && rectNear(r.elements.at(0).painterPath.boundingRect(), 10, 80, 60, 100, 1e-6),
              "svg: percentage lengths are of the viewport");
        // Inkscape's own metadata is not reported as skipped.
        r = c2d::importSvgData("<svg xmlns='http://www.w3.org/2000/svg' "
                               "xmlns:sodipodi='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd' "
                               "viewBox='0 0 100 100'><sodipodi:namedview id='n'/>"
                               "<rect x='1' y='1' width='5' height='5'/></svg>", opts(100, 100));
        check(r.ok && r.elements.size() == 1 && r.skipped == 0, "svg: editor metadata is not 'skipped'");
        // Illustrator's layout: a <switch> whose first child needs an extension
        // nobody has, then the drawing. Only the first child that passes is drawn.
        r = c2d::importSvgData("<svg xmlns='http://www.w3.org/2000/svg' width='100mm' height='100mm' "
                               "viewBox='0 0 100 100'><switch>"
                               "<foreignObject requiredExtensions='http://ns.adobe.com/AdobeIllustrator/10.0/' "
                               "x='0' y='0' width='1' height='1'/>"
                               "<g><rect x='10' y='10' width='50' height='30'/><circle cx='70' cy='70' r='10'/></g>"
                               "<rect x='80' y='5' width='5' height='5'/></switch></svg>", opts(100, 100));
        check(r.ok && r.elements.size() == 2, "svg: a <switch> draws its first child whose conditions pass");
        // Paper space (67 = 1): a layout's frame, not part of the drawing.
        {
            const QByteArray dxf =
                "0\nSECTION\n2\nENTITIES\n"
                "0\nLINE\n8\n0\n10\n0\n20\n0\n11\n10\n21\n0\n"
                "0\nLINE\n67\n1\n8\n0\n10\n0\n20\n50\n11\n400\n21\n50\n"
                "0\nENDSEC\n0\nEOF\n";
            const c2d::ImportResult ps = c2d::importDxfData(dxf, opts(0, 0), QStringLiteral("paper"));
            check(ps.ok && ps.elements.size() == 1, "dxf: paper-space entities are left out");
        }
    }

    // ---- optional: write a .c2d with everything for a --shot -------------
    if (argc >= 3) {
        c2d::Document doc;
        QString err;
        if (!doc.load(QString::fromLocal8Bit(argv[1]), &err)) {
            std::fprintf(stderr, "load failed: %s\n", qPrintable(err));
            return 2;
        }
        c2d::ImportOptions o = opts(doc.boardWidth(), doc.boardHeight());
        o.layer = doc.defaultLayer();
        double dx = 0;
        for (const char *f : {"bezier_transform.svg", "inch_shapes.svg", "px_arcs.svg",
                              "lwpoly_bulge.dxf", "circle_arc_inch.dxf", "spline_block.dxf"}) {
            o.autoPlace = true;
            const c2d::ImportResult r = c2d::importFile(fixture(f), o);
            check(r.ok, f);
            for (c2d::Element e : r.elements) {
                e.translate(dx, 0);   // lay the fixtures out side by side
                doc.addElement(e);
            }
            dx += r.bounds.right() + 20;
            std::printf("%-22s %s\n", f, qPrintable(r.summary()));
        }
        if (!doc.save(QString::fromLocal8Bit(argv[2]), &err)) {
            std::fprintf(stderr, "save failed: %s\n", qPrintable(err));
            return 3;
        }
        c2d::Document back;
        check(back.load(QString::fromLocal8Bit(argv[2]), &err), "reload saved c2d");
        check(back.elements().size() == doc.elements().size(), "all imported elements round-trip");
        std::printf("wrote %s (%d elements)\n", argv[2], int(back.elements().size()));
    }

    // HPGL: a 40 x 40 mm square (40 plotter units per mm) and a circle.
    {
        const c2d::ImportResult r = c2d::importHpglData(
            "IN;SP1;PU0,0;PD1600,0,1600,1600,0,1600,0,0;PU2400,800;CI400;PU;", opts(0, 0), "t.plt");
        check(r.ok && r.elements.size() == 2, "HPGL: square and circle imported");
        check(r.ok && std::abs(r.bounds.width() - 70.0) < 0.1 && std::abs(r.bounds.height() - 40.0) < 0.1,
              "HPGL: 40 units per mm");
    }
    // HP-GL/2 lets a sign separate numbers: "400-0" is 400 then -0.
    {
        const c2d::ImportResult r = c2d::importHpglData("IN;PU0,0;PD400-0,400+200;PU;", opts(0, 0), "s.plt");
        check(r.ok && std::abs(r.bounds.width() - 10.0) < 0.01 && std::abs(r.bounds.height() - 5.0) < 0.01,
              "HPGL: a sign separates numbers");
    }
    // A closed fit-point spline closes along the curve, not a straight line.
    {
        const QByteArray dxf = "0\nSECTION\n2\nENTITIES\n0\nSPLINE\n8\n0\n70\n11\n71\n3\n"
                               "11\n10\n21\n0\n11\n0\n21\n10\n11\n-10\n21\n0\n11\n0\n21\n-10\n"
                               "0\nENDSEC\n0\nEOF\n";
        const c2d::ImportResult r = c2d::importDxfData(dxf, opts(0, 0), "closedfit.dxf");
        bool round = r.ok && r.elements.size() == 1;
        if (round) {
            const QPainterPath &p = r.elements.first().painterPath;
            const QPointF c = p.boundingRect().center();
            for (const QPointF d : {QPointF(5.5, 5.5), QPointF(5.5, -5.5), QPointF(-5.5, 5.5), QPointF(-5.5, -5.5)})
                round = round && p.contains(c + d);
        }
        check(round, "dxf: a closed fit-point spline is smooth all the way round");
    }
    // DXF with bare CR line endings (old Mac exports).
    {
        const QByteArray dxf = "0\rSECTION\r2\rENTITIES\r0\rLINE\r8\r0\r10\r0\r20\r0\r11\r25\r21\r0\r"
                               "0\rENDSEC\r0\rEOF\r";
        const c2d::ImportResult r = c2d::importDxfData(dxf, opts(0, 0), "cr.dxf");
        check(r.ok && r.elements.size() == 1 && std::abs(r.bounds.width() - 25.0) < 0.01,
              "DXF: bare CR line endings");
    }
    // EPS through Ghostscript + pdftocairo: a 50 x 30 mm rectangle.
    if (!QStandardPaths::findExecutable(QStringLiteral("gs")).isEmpty()
        && !QStandardPaths::findExecutable(QStringLiteral("pdftocairo")).isEmpty()) {
        QTemporaryDir dir;
        const QString eps = dir.filePath(QStringLiteral("rect.eps"));
        QFile f(eps);
        f.open(QIODevice::WriteOnly);
        f.write("%!PS-Adobe-3.0 EPSF-3.0\n%%BoundingBox: 0 0 200 200\n"
                "newpath 28.3465 28.3465 moveto 170.0787 28.3465 lineto 170.0787 113.3858 lineto "
                "28.3465 113.3858 lineto closepath 0.5 setlinewidth stroke showpage\n");
        f.close();
        const c2d::ImportResult r = c2d::importFile(eps, opts(0, 0));
        check(r.ok && !r.elements.isEmpty(), "EPS: converted and imported");
        check(r.ok && std::abs(r.bounds.width() - 50.0) < 0.5 && std::abs(r.bounds.height() - 30.0) < 0.5,
              "EPS: 50 x 30 mm, the size it was drawn");
    } else {
        std::printf("test_import: gs/pdftocairo missing, EPS/PDF import not tested\n");
    }
    check(c2d::isImportableFile(QStringLiteral("a.PLT")) && c2d::isImportableFile(QStringLiteral("b.ai"))
              && c2d::isImportableFile(QStringLiteral("c.pdf")) && !c2d::isImportableFile(QStringLiteral("d.c2d")),
          "importable suffixes");

    // joinSubpaths with its endpoint grid: same output as the old O(n^2)
    // sweep on scrambled pieces snapped to a coarse grid (many shared ends,
    // forks, reversed and near-miss pieces, curves, closed loops).
    {
        std::srand(7);
        for (int round = 0; round < 6; ++round) {
            QPainterPath in;
            const double tol = round % 2 ? 0.01 : 0.5;
            for (int k = 0; k < 400; ++k) {
                auto pt = [&] {
                    const double jit = (std::rand() % 3 - 1) * tol * 0.4;
                    return QPointF(std::rand() % 12 * 1.0 + jit, std::rand() % 12 * 1.0 - jit);
                };
                const QPointF a = pt();
                in.moveTo(a);
                const int segs = 1 + std::rand() % 3;
                for (int g = 0; g < segs; ++g) {
                    if (std::rand() % 4 == 0)
                        in.cubicTo(pt(), pt(), pt());
                    else
                        in.lineTo(pt());
                }
                if (std::rand() % 10 == 0)
                    in.lineTo(a);
            }
            check(samePath(c2d::joinSubpaths(in, tol), joinSubpathsSlow(in, tol)),
                  "joinSubpaths: same output as the quadratic version");
        }
        // 100k loose lines forming one long zig-zag, given in scrambled order.
        QVector<int> order(100000);
        for (int i = 0; i < order.size(); ++i)
            order[i] = i;
        for (int i = order.size() - 1; i > 0; --i)
            std::swap(order[i], order[std::rand() % (i + 1)]);
        QPainterPath lines;
        for (int i : order) {
            lines.moveTo(i * 0.1, (i % 2) * 1.0);
            lines.lineTo((i + 1) * 0.1, ((i + 1) % 2) * 1.0);
        }
        QElapsedTimer t;
        t.start();
        const QPainterPath joined = c2d::joinSubpaths(lines, 0.01);
        int moves = 0;
        for (int i = 0; i < joined.elementCount(); ++i)
            moves += joined.elementAt(i).isMoveTo();
        check(moves == 1 && joined.elementCount() == 100001, "joinSubpaths: 100k lines into one chain");
        check(t.elapsed() < 5000, "joinSubpaths: 100k lines in seconds, not a minute");
    }

    // SPLINE flattening is bounded: a 1000-point spline zig-zagging over a
    // hundred metres stays within the point budget and finishes quickly;
    // coordinates no machine has are refused.
    {
        auto spline = [](double amp) {
            QByteArray d = "0\nSECTION\n2\nENTITIES\n0\nSPLINE\n70\n8\n71\n3\n";
            for (int i = 0; i < 1000; ++i)
                d += "10\n" + QByteArray::number(i * 10) + "\n20\n"
                     + QByteArray::number(i % 2 ? amp : -amp) + "\n";
            return d + "0\nENDSEC\n0\nEOF\n";
        };
        QElapsedTimer t;
        t.start();
        const c2d::ImportResult big = c2d::importDxfData(spline(5e4), opts(0, 0), "big.dxf");
        int rows = 0;
        for (const c2d::Element &e : big.elements)
            rows += e.raw.value("points").toArray().size();
        check(big.ok && rows <= 1000002, "SPLINE: flattened points capped");
        check(t.elapsed() < 10000, "SPLINE: a hostile spline imports in seconds");
        const c2d::ImportResult absurd = c2d::importDxfData(spline(1e9), opts(0, 0), "absurd.dxf");
        check(absurd.ok && absurd.elements.isEmpty() && absurd.skipped == 1,
              "SPLINE: kilometre-scale coordinates refused");
        check(t.elapsed() < 10000, "SPLINE: refused without flattening");
    }

    // HPGL IN resets the plotter: pen up (PA after it moves, not draws) and
    // absolute coordinates.
    {
        const c2d::ImportResult r = c2d::importHpglData(
            "IN;PD;PA400,0;IN;PA800,800;PD1600,800;PU;", opts(0, 0), "in.plt");
        check(r.ok && r.elements.size() == 2, "HPGL IN: pen up after it (no stray line to 800,800)");
        const c2d::ImportResult a = c2d::importHpglData(
            "PR;PU400,400;IN;PD800,0;PU;", opts(0, 0), "in2.plt");
        check(a.ok && std::abs(a.bounds.width() - 10.0) < 0.1, "HPGL IN: back to absolute coordinates");
    }

    // HPGL SC user units: mapped onto IP's points (type 0), as plotter units
    // per user unit (type 2), or noted as skipped with no IP to scale into.
    {
        const c2d::ImportResult r = c2d::importHpglData(
            "IN;IP0,0,4000,4000;SC0,100,0,100;PU0,0;PD100,0,100,100,0,100,0,0;PU;", opts(0, 0), "sc.plt");
        check(r.ok && std::abs(r.bounds.width() - 100.0) < 0.1 && std::abs(r.bounds.height() - 100.0) < 0.1,
              "HPGL SC: user units map onto IP");
        const c2d::ImportResult f = c2d::importHpglData(
            "IN;SC0,40,0,40,2;PU0,0;PD10,0,10,10,0,10,0,0;PU;", opts(0, 0), "sc2.plt");
        check(f.ok && std::abs(f.bounds.width() - 10.0) < 0.1, "HPGL SC type 2: plotter units per user unit");
        const c2d::ImportResult n = c2d::importHpglData(
            "IN;SC0,100,0,100;PU0,0;PD100,0,100,100;PU;", opts(0, 0), "sc3.plt");
        check(n.ok && n.skipped >= 1, "HPGL SC without IP is noted, not ignored");
        const c2d::ImportResult later = c2d::importHpglData(
            "IN;IP0,0,4000,4000;SC0,100,0,100;IP0,0,8000,8000;PU0,0;PD100,0,100,100,0,100,0,0;PU;", opts(0, 0), "sc4.plt");
        check(later.ok && std::abs(later.bounds.width() - 200.0) < 0.1, "HPGL: an IP after SC re-maps the user units");
        const c2d::ImportResult first = c2d::importHpglData(
            "IN;SC0,100,0,100;IP0,0,4000,4000;PU0,0;PD100,0,100,100,0,100,0,0;PU;", opts(0, 0), "sc5.plt");
        check(first.ok && std::abs(first.bounds.width() - 100.0) < 0.1 && first.skipped == 0,
              "HPGL: SC before IP scales once IP comes");
        const c2d::ImportResult one = c2d::importHpglData(
            "IN;IP4000,4000;SC0,100,0,100;PU0,0;PD100,0,100,100,0,100,0,0;PU;", opts(0, 0), "sc6.plt");
        check(one.ok && one.skipped >= 1 && one.bounds.width() > 1, "HPGL: IP with only P1 and no frame leaves SC skipped");
    }

    // HPGL label text and PE data are not commands: "Project" is not PR.
    {
        const c2d::ImportResult r = c2d::importHpglData(
            QByteArray("IN;PU;LBProject 7\x03;PU8000,0;PD12000,0,12000,4000,8000,4000,8000,0;PU;"),
            opts(0, 0), "lb.plt");
        check(r.ok && std::abs(r.bounds.width() - 100.0) < 0.1 && std::abs(r.bounds.height() - 100.0) < 0.1,
              "HPGL LB: label text is skipped, the square stays absolute");
        const c2d::ImportResult t = c2d::importHpglData(
            QByteArray("IN;DT@;LBPrinted@;PU0,0;PD400,0;PU;"), opts(0, 0), "dt.plt");
        check(t.ok && std::abs(t.bounds.width() - 10.0) < 0.1, "HPGL DT: the label ends at its own terminator");
        const c2d::ImportResult q = c2d::importHpglData(
            QByteArray("IN;SP1;PU0,0;PD4000,0;PU;CO\"Project 7\";PU8000,0;PD12000,0,12000,4000,8000,4000,8000,0;PU;"),
            opts(0, 0), "co.plt");
        check(q.ok && std::abs(q.bounds.width() - 300.0) < 0.1 && std::abs(q.bounds.height() - 100.0) < 0.1,
              "HPGL CO: a quoted comment is not read as commands");
        const c2d::ImportResult w = c2d::importHpglData(
            QByteArray("IN;WDPress any key\x03;PU0,0;PD4000,0,4000,4000;PU;"), opts(0, 0), "wd.plt");
        check(w.ok && std::abs(w.bounds.width() - 100.0) < 0.1 && std::abs(w.bounds.height() - 100.0) < 0.1,
              "HPGL WD: display text is skipped like a label");
        // A PCL wrapper (Windows drivers): ESC%1B's B is not the start of BP.
        const c2d::ImportResult e = c2d::importHpglData(
            QByteArray("\x1b%-12345X@PJL ENTER LANGUAGE=HPGL2\r\n\x1b%1BBP1,\"Project\";IN;"
                       "PU8000,0;PD12000,0,12000,4000,8000,4000,8000,0;PU;\x1b%0A"),
            opts(0, 0), "pcl.plt");
        check(e.ok && std::abs(e.bounds.width() - 100.0) < 0.1 && std::abs(e.bounds.height() - 100.0) < 0.1,
              "HPGL in PCL: escape sequences are skipped whole");
        const c2d::ImportResult e2 = c2d::importHpglData(
            QByteArray("\x1b%1BPU8000,0;PD12000,0,12000,4000;PU;"), opts(0, 0), "pcl2.plt");
        check(e2.ok && std::abs(e2.bounds.width() - 100.0) < 0.1, "HPGL in PCL: the first PU is kept");
        const c2d::ImportResult in = c2d::importHpglData(
            QByteArray("IN;DT@;LBa@;IN;LBNext\x03;PU0,0;PD4000,0,4000,4000;PU;"), opts(0, 0), "in3.plt");
        check(in.ok && in.elements.size() == 1, "HPGL IN: the label terminator is back to ETX");
        // Plotter-on (ESC.( / ESC.Y) takes no parameters and has no ':'.
        const c2d::ImportResult on = c2d::importHpglData(
            QByteArray("\x1b.(IN;SP1;PU0,0;PD4000,0,4000,4000;PU;"), opts(0, 0), "on.plt");
        check(on.ok && std::abs(on.bounds.width() - 100.0) < 0.1 && std::abs(on.bounds.height() - 100.0) < 0.1,
              "HPGL ESC.(: plotter-on is one letter");
        const c2d::ImportResult on2 = c2d::importHpglData(
            QByteArray("\x1b.Y;IN;PU0,0;PD4000,0,4000,4000;PU;LBTime: 12\x03;PU8000,0;PD12000,0;PU;"
                       "\x1b.I81;;17:PU0,8000;PD400,8000;PU;"), opts(0, 0), "on2.plt");
        check(on2.ok && on2.elements.size() == 3, "HPGL ESC.Y: nothing after it is dropped; ESC.I to ':'");
    }

    std::printf("test_import: %d checks OK\n", g_checks);
    return 0;
}
