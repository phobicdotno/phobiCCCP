// DXF export: writes R12 entities in mm and reads back through our own DXF
// importer with the same geometry. Plain asserts; exits 0 on success.

#include "../src/dxfexport.h"
#include "../src/element.h"
#include "../src/importers.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <QSet>
#include <QStringList>

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
    check(text.contains("\n8\nCUT__OUTSIDE\n"), "layer name in R12 form: upper case, ':' and ' ' as '_'");
    check(text.contains("\n8\nHOLES\n"), "circle on its own layer");
    check(text.contains("0\nTABLE\n2\nLTYPE\n") && text.contains("0\nLTYPE\n2\nCONTINUOUS\n"),
          "the CONTINUOUS line type the layers name is in an LTYPE table");
    check(text.indexOf("\nLTYPE\n") < text.indexOf("\n2\nLAYER\n"), "LTYPE table before LAYER table");
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

    // There and back again (A, B, A) reads as closed, its last point being
    // its first, but is no ring: it went out as a closed polyline of two
    // vertices. A triangle is the smallest ring.
    {
        auto entities = [](const DxfExportResult &x) {
            const QString t = QString::fromUtf8(x.data);
            return t.mid(t.indexOf(QStringLiteral("\nENTITIES\n")));
        };
        const DxfExportResult back2 = exportDxf({Element::makePath({{0, 0}, {10, 0}, {0, 0}}, false, cut)});
        QString t = entities(back2);
        check(back2.entities == 1 && t.count("\nVERTEX\n") == 3, "there and back: all three vertices written");
        check(t.contains("\n70\n0\n") && !t.contains("\n70\n1\n"), "there and back: an open polyline");
        const ImportResult again = importDxfData(back2.data, opt, QStringLiteral("thereandback.dxf"));
        check(again.ok && again.elements.size() == 1
                  && near(again.elements.first().painterPath.length(), 20),
              "there and back: read back as 10 mm out and 10 mm home");

        const DxfExportResult tri = exportDxf({Element::makePath({{0, 0}, {10, 0}, {0, 10}}, true, cut)});
        t = entities(tri);
        check(tri.entities == 1 && t.count("\nVERTEX\n") == 3 && t.contains("\n70\n1\n"),
              "a triangle is still a closed polyline of three vertices");
    }

    // Layer names: R12 compares them without regard to case and takes at
    // most 31 of A-Z 0-9 _ $ -. Names that map onto one another stay
    // different layers, and the same name always maps the same way.
    {
        const QString longA = QStringLiteral("A very long layer name for the outside cuts, part one");
        const QString longB = QStringLiteral("A very long layer name for the outside cuts, part two");
        const QVector<Element> ls = {
            Element::makePath({{0, 0}, {10, 0}}, false, layer(QStringLiteral("Holes"))),
            Element::makePath({{0, 1}, {10, 1}}, false, layer(QStringLiteral("holes"))),
            Element::makePath({{0, 2}, {10, 2}}, false, layer(QStringLiteral("Holes"))),
            Element::makePath({{0, 3}, {10, 3}}, false, layer(longA)),
            Element::makePath({{0, 4}, {10, 4}}, false, layer(longB)),
            Element::makePath({{0, 5}, {10, 5}}, false, layer(QStringLiteral("Tiefe ä/ö"))),
            Element::makePath({{0, 6}, {10, 6}}, false, layer(QString())),
        };
        const DxfExportResult lr = exportDxf(ls);
        const QString t = QString::fromUtf8(lr.data);
        const QString tables = t.mid(t.indexOf(QStringLiteral("\n2\nLAYER\n")),
                                     t.indexOf(QStringLiteral("\nENTITIES\n"))
                                         - t.indexOf(QStringLiteral("\n2\nLAYER\n")));
        QStringList names;
        const QStringList lines = tables.split(QLatin1Char('\n'));
        for (int i = 0; i + 2 < lines.size(); ++i)
            if (lines[i] == QLatin1String("LAYER") && lines[i + 1] == QLatin1String("2"))
                names.append(lines[i + 2]);
        bool safe = true;
        QSet<QString> upper;
        for (const QString &n : names) {
            safe = safe && !n.isEmpty() && n.size() <= 31;
            for (const QChar ch : n) {
                const char16_t c = ch.unicode();
                safe = safe && ((c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9') || c == u'_'
                                || c == u'$' || c == u'-');
            }
            upper.insert(n.toUpper());
        }
        check(safe, "layer names: at most 31 of A-Z 0-9 _ $ -");
        check(upper.size() == names.size(), "no two LAYER entries differ only by case");
        // 0, HOLES, holes, two long names, Tiefe, and none (layer 0 again).
        check(names.size() == 6, "every distinct layer once, the unnamed one as layer 0");
        check(names.contains(QStringLiteral("0")) && names.contains(QStringLiteral("HOLES"))
                  && names.contains(QStringLiteral("TIEFE____")),
              "plain names keep their R12 form");
        check(t.count(QStringLiteral("\n8\nHOLES\n")) == 8,
              "both 'Holes' paths on HOLES (POLYLINE, 2 VERTEX, SEQEND each), 'holes' not");
        check(exportDxf(ls).data == lr.data, "the same layers map the same way every time");
    }

    std::printf("dxfexport: %d checks passed\n", g_checks);
    return 0;
}
