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
#include <QJsonArray>
#include <QJsonObject>

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
        r = c2d::importDxfData("garbage\n", opts(0, 0), QStringLiteral("x"));
        check(!r.ok, "non-dxf rejected");
        r = c2d::importSvgData("<svg xmlns='http://www.w3.org/2000/svg'/>", opts(0, 0));
        check(r.ok && r.elements.isEmpty(), "empty svg imports nothing, no error");
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

    std::printf("test_import: %d checks OK\n", g_checks);
    return 0;
}
