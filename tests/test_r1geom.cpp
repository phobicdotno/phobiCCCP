// Headless checks for a round of geometry / input hardening fixes:
//  - Offset of a shape selected together with its exact duplicate (the copy
//    must not nest inside the original as a hole).
//  - Offset nesting is the same rule at any element count (no even-odd
//    fallback above 200 shapes: overlaps weld, they do not become holes).
//  - appsettings::machine clamps / rejects a custom machine size.
//  - A malformed path element draws the shape its node model edits.
//  - Model3D JSON and grayField bound blur / tile / resolution.
// Plain asserts, no test framework; exits 0 on success.

#include "../src/element.h"
#include "../src/model3d.h"
#include "../src/settingsdialog.h"
#include "../src/vectorops.h"

#include <QCoreApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

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

static QRectF bounds(const QVector<Element> &els)
{
    QRectF r;
    for (const Element &e : els)
        r = r.united(e.painterPath.boundingRect());
    return r;
}

static double netArea(const QVector<Element> &els)
{
    double a = 0;
    for (const Element &e : els)
        for (const QPolygonF &p : e.painterPath.toSubpathPolygons())
            a += vec::ringArea(p);
    return std::fabs(a);
}

static QJsonArray xy(double x, double y) { return QJsonArray{x, y}; }

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QJsonObject layer;
    layer.insert("name", QStringLiteral("DEFAULT"));
    layer.insert("uuid", QStringLiteral("{layer}"));

#ifdef HAVE_CLIPPER2   // offsetElements needs Clipper2
    // --- G1: a shape and its exact duplicate offset as one shape ----------
    {
        const Element sq = Element::makeRectangle({10, 10}, 20, 20, layer);   // 0..20
        const QVector<Element> one = vec::offsetElements({sq}, 1.0);
        const QVector<Element> two = vec::offsetElements({sq, sq}, 1.0);
        const QVector<Element> three = vec::offsetElements({sq, sq, sq}, 1.0);
        check(one.size() == 1, "single square offsets to one ring");
        check(two.size() == 1, "square + duplicate offsets to one ring (not nothing)");
        check(three.size() == 1, "square + two duplicates offsets to one ring");
        check(approx(bounds(two).left(), -1, 1e-3) && approx(bounds(two).right(), 21, 1e-3),
              "duplicate offset is the square grown by d");
        check(approx(netArea(two), netArea(one), 1e-3), "duplicate offset has the single's area");

        // A ring (outer + inner) with the inner duplicated stays a ring.
        const Element inner = Element::makeRectangle({10, 10}, 8, 8, layer);
        const QVector<Element> ring = vec::offsetElements({sq, inner}, 1.0);
        const QVector<Element> ringDup = vec::offsetElements({sq, inner, inner}, 1.0);
        check(ring.size() == 2 && ringDup.size() == 2, "ring with a duplicated hole keeps its hole");
        check(approx(netArea(ring), netArea(ringDup), 1e-3), "duplicated hole: same area");
    }

    // --- G2: many overlapping shapes weld exactly like two -----------------
    {
        // Squares 20 wide whose centres step from x=0 to x=10: their union is
        // the same as the first and last alone.
        auto row = [&](int n) {
            QVector<Element> v;
            for (int i = 0; i < n; ++i)
                v.append(Element::makeRectangle({10.0 * i / (n - 1), 0}, 20, 20, layer));
            return v;
        };
        const QVector<Element> two = vec::offsetElements(row(2), 1.0);
        const QVector<Element> many = vec::offsetElements(row(201), 1.0);
        check(two.size() == 1, "two overlapping squares offset to one ring");
        check(many.size() == 1, "201 overlapping squares offset to one ring");
        const QRectF b2 = bounds(two), bm = bounds(many);
        check(approx(b2.left(), bm.left(), 1e-3) && approx(b2.right(), bm.right(), 1e-3)
                  && approx(b2.top(), bm.top(), 1e-3) && approx(b2.bottom(), bm.bottom(), 1e-3),
              "201 squares weld to the same outline as 2");
        check(approx(netArea(two), netArea(many), 1e-2), "201 squares: same area as 2");
        const QVector<Element> lots = vec::offsetElements(row(2000), 1.0);
        check(lots.size() == 1 && approx(netArea(lots), netArea(two), 1e-2),
              "2000 overlapping squares still weld to one ring");

        // Nesting above the old cap: 250 scattered squares plus one hole.
        QVector<Element> v;
        for (int i = 0; i < 250; ++i)
            v.append(Element::makeRectangle({100.0 + 30 * i, 0}, 20, 20, layer));
        v.append(Element::makeRectangle({0, 0}, 40, 40, layer));
        v.append(Element::makeRectangle({0, 0}, 10, 10, layer));
        check(vec::offsetElements(v, 1.0).size() == 252,
              "251 separate shapes + one hole offset as 250 squares and a ring");
    }

#endif

    // --- G4: custom machine size is clamped / rejected ---------------------
    {
        QTemporaryDir dir;
        check(dir.isValid(), "temp dir");
        QSettings s(dir.filePath("t.ini"), QSettings::IniFormat);
        s.setValue("machine/size", "custom");
        s.setValue("machine/customWidth", 600.0);
        s.setValue("machine/customHeight", 400.0);
        appsettings::Machine m = appsettings::machine(s);
        check(m.key == "custom" && approx(m.width, 600) && approx(m.height, 400),
              "custom size in range is kept");
        s.setValue("machine/customWidth", 1e9);
        s.setValue("machine/customHeight", 0.0);
        m = appsettings::machine(s);
        check(m.key == "custom" && approx(m.width, 5000) && approx(m.height, 10),
              "custom size clamps to [10, 5000]");
        s.setValue("machine/customWidth", std::numeric_limits<double>::quiet_NaN());
        m = appsettings::machine(s);
        check(m.key == "s5pro-4x4" && approx(m.width, 1237), "NaN custom width -> default machine");
        s.setValue("machine/customWidth", QStringLiteral("wide"));
        s.setValue("machine/customHeight", 400.0);
        m = appsettings::machine(s);
        check(m.key == "s5pro-4x4" && approx(m.height, 1237),
              "non-numeric custom width -> default machine");
    }

    // --- G5: malformed path rows draw what the node model edits ------------
    {
        QJsonObject o;
        o.insert("geometryType", "path");
        o.insert("position", xy(0, 0));
        // No first row, and a cubic with no handles at all.
        o.insert("points", QJsonArray{xy(5, 5), xy(10, 5), xy(10, 10)});
        o.insert("point_type", QJsonArray{1, 1, 3});
        const Element e = Element::fromJson(o);
        const QRectF bb = e.painterPath.boundingRect();
        check(approx(bb.left(), 5) && approx(bb.top(), 5) && approx(bb.right(), 10)
                  && approx(bb.bottom(), 10),
              "stray first row starts the path; handle-less cubic is a line (no spike to 0,0)");
        const QRectF nb = Element::pathModel(e).painterPath().boundingRect();
        check(approx(nb.left(), bb.left()) && approx(nb.right(), bb.right())
                  && approx(nb.top(), bb.top()) && approx(nb.bottom(), bb.bottom()),
              "painter path and node model agree");

        // A cubic with only cp2 (cp1 array short): cp1 is the previous anchor.
        QJsonObject c = o;
        c.insert("points", QJsonArray{xy(0, 0), xy(10, 0)});
        c.insert("point_type", QJsonArray{0, 3});
        c.insert("cp1", QJsonArray{xy(0, 0)});
        c.insert("cp2", QJsonArray{xy(0, 0), xy(10, 8)});
        const Element ce = Element::fromJson(c);
        const QRectF cb = ce.painterPath.boundingRect();
        const QRectF cn = Element::pathModel(ce).painterPath().boundingRect();
        check(approx(cb.left(), cn.left(), 1e-6) && approx(cb.right(), cn.right(), 1e-6)
                  && approx(cb.top(), cn.top(), 1e-6) && approx(cb.bottom(), cn.bottom(), 1e-6),
              "cubic missing cp1 agrees with the node model");

        // CC's well-formed rows are drawn exactly as before.
        QJsonObject w = o;
        w.insert("points", QJsonArray{xy(0, 0), xy(10, 0), xy(10, 10), xy(0, 0)});
        w.insert("point_type", QJsonArray{0, 1, 1, 4});
        const QRectF wb = Element::fromJson(w).painterPath.boundingRect();
        check(approx(wb.left(), 0) && approx(wb.right(), 10) && approx(wb.bottom(), 10),
              "well-formed path unchanged");
    }

    // --- G3: model JSON and blur are bounded --------------------------------
    {
        QJsonObject c;
        c.insert("blur", 1e12);
        c.insert("tile_width", -5.0);
        const ModelComponent mc = ModelComponent::fromJson(c);
        check(approx(mc.blurPx, 50) && approx(mc.tileWidth, 0.1), "blur / tile width clamped");
        check(approx(Model3D::fromJson(QJsonObject{{"resolution", -1.0}}).resolution, 0),
              "negative resolution -> auto");
        check(approx(Model3D::fromJson(QJsonObject{{"resolution", 1e9}}).resolution, 10),
              "huge resolution capped");
        check(approx(Model3D::fromJson(QJsonObject{{"resolution", 0.25}}).resolution, 0.25),
              "normal resolution kept");
        QImage img(4, 4, QImage::Format_Grayscale8);
        img.fill(128);
        const GrayField g = grayField(img, false, 1e12);
        check(g.cols == 4 && g.rows == 4 && std::isfinite(g.v.at(5)), "huge blur sigma is safe");
    }

    std::printf("r1geom: %d checks passed\n", g_checks);
    return 0;
}
