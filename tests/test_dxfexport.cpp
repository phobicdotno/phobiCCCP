// DXF export: writes R12 entities in mm and reads back through our own DXF
// importer with the same geometry. Plain asserts; exits 0 on success.

#include "../src/dxfexport.h"
#include "../src/element.h"
#include "../src/importers.h"

#include <QCoreApplication>
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

static bool near(double a, double b, double eps = 0.02) { return std::fabs(a - b) < eps; }

static QJsonObject layer(const QString &name)
{
    QJsonObject l;
    l.insert("blue", 0); l.insert("green", 0); l.insert("red", 0);
    l.insert("locked", false); l.insert("name", name);
    l.insert("uuid", QString()); l.insert("visible", true);
    return l;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const QJsonObject cut = layer(QStringLiteral("Cut: outside"));
    QVector<Element> els;
    els.append(Element::makeRectangle({110, 65}, 100, 50, cut));          // 60..160 x 40..90
    els.append(Element::makeCircle({20, 20}, 10, layer(QStringLiteral("Holes"))));
    els.append(Element::makePath({{0, 100}, {30, 130}, {60, 100}}, false, cut));
    PathModel bez;
    SubPath s;
    PathNode a; a.p = {200, 0}; a.out = {200, 40};
    PathNode b; b.p = {240, 0}; b.in = {240, 40};
    a.in = a.p; b.out = b.p;
    s.nodes = {a, b};
    s.closed = false;
    bez.subs = {s};
    els.append(Element::makeBezierPath(bez, cut));
    els.append(Element::withConstruction(
        Element::makePath({{0, 0}, {300, 300}}, false, cut), true));

    const DxfExportResult r = exportDxf(els);
    const QString text = QString::fromUtf8(r.data);
    check(r.skipped == 1, "construction line left out");
    check(r.entities == 4, "four entities written");
    check(text.contains("$INSUNITS\n70\n4\n"), "units are millimetres");
    check(text.count("\nCIRCLE\n") == 1, "circle stays a CIRCLE");
    check(text.count("\nPOLYLINE\n") == 3, "rectangle, path and bezier as polylines");
    check(text.contains("\nCut_ outside\n"), "':' replaced in the layer name");
    check(text.contains("\nHoles\n"), "circle on its own layer");
    check(text.endsWith("0\nEOF\n"), "ends with EOF");
    check(!text.contains("-0\n"), "no negative zero");

    ImportOptions opt;
    opt.autoPlace = false;
    const ImportResult back = importDxfData(r.data, opt, QStringLiteral("roundtrip.dxf"));
    check(back.ok, "our own importer reads it");
    check(back.elements.size() == 4, "four elements come back");
    // Bounds: the open path starts at x 0 and peaks at y 130, the bezier sits
    // on y 0 and ends at x 240.
    check(near(back.bounds.left(), 0) && near(back.bounds.top(), 0), "bounds bottom-left");
    check(near(back.bounds.right(), 240) && near(back.bounds.bottom(), 130), "bounds top-right");

    int closed = 0;
    QRectF rect, circ;
    for (const Element &e : back.elements) {
        const QRectF b = e.painterPath.boundingRect();
        const QList<QPolygonF> polys = e.painterPath.toSubpathPolygons();
        if (!polys.isEmpty() && polys.first().isClosed())
            ++closed;
        if (near(b.width(), 100) && near(b.height(), 50))
            rect = b;
        if (near(b.width(), 20) && near(b.height(), 20))
            circ = b;
    }
    check(closed == 2, "rectangle and circle closed, path and bezier open");
    check(near(rect.left(), 60) && near(rect.top(), 40), "rectangle in place");
    check(near(circ.center().x(), 20) && near(circ.center().y(), 20), "circle in place");

    std::printf("dxfexport: %d checks passed\n", g_checks);
    return 0;
}
