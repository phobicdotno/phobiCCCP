// Parameters, constraints and driving dimensions (parametric.h): the
// expression language, parameter values, the solver on every constraint
// type, degrees of freedom, toolpath bindings and the round trip through a
// saved .c2d.
//
// Plain asserts, no test framework; exits 0 on success.

#include "../src/c2ddocument.h"
#include "../src/parametric.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineF>
#include <QTemporaryDir>
#include <QtMath>

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

static bool approx(double a, double b, double eps = 1e-5) { return std::fabs(a - b) < eps; }

static double eval(const QString &e, bool *ok = nullptr)
{
    double v = 0;
    const bool r = param::evaluate(e, [](const QString &n, double *v) {
        if (n == QLatin1String("w")) { *v = 100; return true; }
        if (n == QLatin1String("margin")) { *v = 7.5; return true; }
        return false;
    }, &v);
    if (ok) *ok = r;
    return v;
}

static GeoRef ref(GeoRef::Kind k, const QString &id, int index, int sub = 0)
{
    GeoRef r;
    r.kind = k; r.id = id; r.index = index; r.sub = sub;
    return r;
}

static QString addCon(Document &d, Constraint::Type t, const QVector<GeoRef> &refs,
                      const QString &expr = QString())
{
    Constraint c;
    c.type = t;
    c.refs = refs;
    c.uid = d.parametric().nextUid();
    if (c.isDimension()) {
        c.name = d.parametric().nextDimensionName();
        c.expr = expr;
    }
    param::prime(d, c);
    if (!expr.isEmpty())
        c.expr = expr;
    d.parametricRef().constraints.append(c);
    return c.uid;
}

static QPointF pt(const Document &d, const GeoRef &r)
{
    QPointF p;
    check(param::point(d, r, &p), "point resolves");
    return p;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QJsonObject layer;
    layer.insert("name", QStringLiteral("DEFAULT"));
    bool ok = false;

    // ---- expressions ------------------------------------------------------
    check(approx(eval("1 + 2 * 3"), 7), "precedence");
    check(approx(eval("(1 + 2) * 3"), 9), "parentheses");
    check(approx(eval("2 ^ 3 ^ 2"), 512), "power is right-associative");
    check(approx(eval("-3 + 5"), 2), "unary minus");
    check(approx(eval("w - 2 * margin"), 85), "names");
    check(approx(eval("1 in"), 25.4), "inch unit");
    check(approx(eval("1/4 in"), 6.35), "a unit scales the product before it");
    check(approx(eval("10 mm * 2"), 20), "unit then more product");
    check(approx(eval("(1/4) in"), 6.35), "unit after parentheses");
    check(approx(eval("2\""), 50.8), "inch mark");
    check(approx(eval("3 cm + 2 mm"), 32), "cm + mm");
    check(approx(eval("sin(30)"), 0.5), "sin in degrees");
    check(approx(eval("atan2(1, 1)"), 45), "atan2 in degrees");
    check(approx(eval("max(1, w, 3)"), 100), "max of three");
    check(approx(eval("sqrt(16) + abs(-2)"), 6), "sqrt and abs");
    check(approx(eval("pi"), M_PI), "pi");
    check(approx(eval("1e3"), 1000), "exponent");
    check(approx(eval("12,5"), 12.5), "decimal comma");
    eval("nope + 1", &ok);
    check(!ok, "unknown name fails");
    eval("1 / 0", &ok);
    check(!ok, "division by zero fails");
    eval("(1 + 2", &ok);
    check(!ok, "missing parenthesis fails");
    eval("", &ok);
    check(!ok, "empty fails");
    check(param::namesIn("w - 2*margin + sin(a) + 2in").size() == 3, "namesIn skips functions and units");
    check(param::isValidName("plate_w") && !param::isValidName("2x") && !param::isValidName("sin")
          && !param::isValidName("in") && !param::isValidName("stock_t"), "isValidName");
    check(param::formatNumber(12.5) == "12.5" && param::formatNumber(3) == "3"
          && param::formatNumber(1.23456) == "1.235", "formatNumber");

    // ---- parameter values -------------------------------------------------
    QTemporaryDir tmp;
    check(tmp.isValid(), "temp dir");
    const QString blank = tmp.filePath("blank.c2d");
    QString err;
    check(Document::createBlank(blank, 300, 200, 19, &err), "createBlank");
    {
        Document d;
        check(d.load(blank, &err), "load blank");
        Parametric &p = d.parametricRef();
        p.params = {{"plate", "stock_w - 2 * margin", ""}, {"margin", "10", ""},
                    {"loop_a", "loop_b + 1", ""}, {"loop_b", "loop_a", ""},
                    {"bad", "plate + nothing", ""}};
        QStringList errors;
        const auto v = param::values(d, p, &errors);
        check(approx(v.value("plate"), 280), "parameter uses a later one and the stock");
        check(approx(v.value("stock_t"), 19), "stock_t built-in");
        check(!v.contains("loop_a") && !v.contains("loop_b"), "a cycle has no value");
        check(!v.contains("bad"), "an unknown name has no value");
        check(errors.size() >= 3, "errors reported");
        p.renameEverywhere("margin", "edge");
        check(p.params.at(0).expr == "stock_w - 2 * edge" && p.params.at(1).name == "edge",
              "rename follows into expressions");
    }

    // ---- a rectangle drawn as a path, constrained like a Fusion sketch ----
    {
        Document d;
        check(d.load(blank, &err), "load");
        const Element path = Element::makePath({{10, 10}, {62, 13}, {58, 41}, {12, 37}}, true, layer);
        d.addElement(path);
        const QString id = path.id;
        auto P = [&](int i) { return ref(GeoRef::Point, id, i); };
        auto L = [&](int i) { return ref(GeoRef::Line, id, i); };
        check(param::refsOf(d, id).size() == 8, "a closed 4-node path has 4 points and 4 lines");
        addCon(d, Constraint::Horizontal, {L(0)});
        addCon(d, Constraint::Horizontal, {L(2)});
        addCon(d, Constraint::Vertical, {L(1)});
        addCon(d, Constraint::Vertical, {L(3)});
        addCon(d, Constraint::Coincident, {P(0), GeoRef::origin()});
        d.parametricRef().params = {{"plate_w", "80", ""}, {"plate_h", "plate_w / 2", ""}};
        addCon(d, Constraint::Distance, {L(0)}, "plate_w");
        addCon(d, Constraint::Distance, {L(1)}, "plate_h");
        const param::SolveResult r = param::solve(d);
        check(r.ok, "rectangle solves");
        check(r.changed.contains(id), "the path moved");
        check(QLineF(pt(d, P(0)), QPointF(0, 0)).length() < 1e-6, "corner on the origin");
        check(QLineF(pt(d, P(2)), QPointF(80, 40)).length() < 1e-5, "opposite corner at 80, 40");
        const param::Freedom f = param::freedom(d);
        check(f.dof == 0, "rectangle has no freedom left");
        check(f.fullyConstrained.contains(id), "and is fully constrained");

        // A parameter change drives the geometry.
        d.parametricRef().params[0].expr = "120";
        check(param::solve(d).ok, "re-solve after parameter change");
        check(QLineF(pt(d, P(2)), QPointF(120, 60)).length() < 1e-5, "follows plate_w = 120");

        // Dragging the corner (keep = this element) cannot break the constraints.
        Element *e = d.elementById(id);
        e->translate(15, 5);
        check(param::solve(d, {id}).ok, "solve after a move");
        check(QLineF(pt(d, P(0)), QPointF(0, 0)).length() < 1e-5, "the origin coincidence wins");

        // A conflicting dimension is reported and changes nothing.
        const QPointF before = pt(d, P(2));
        addCon(d, Constraint::HDistance, {P(0), P(1)}, "50");
        const param::SolveResult bad = param::solve(d);
        check(!bad.ok && !bad.failing.isEmpty(), "conflict reported");
        check(QLineF(pt(d, P(2)), before).length() < 1e-9, "a failed solve leaves the drawing alone");
        d.parametricRef().constraints.removeLast();

        // Without the origin it has two degrees of freedom: it can slide.
        d.parametricRef().constraints.removeAt(4);
        check(param::freedom(d).dof == 2, "two DOF without the origin");
        check(!param::freedom(d).fullyConstrained.contains(id), "not fully constrained");
    }

    // ---- circles: radius, diameter, tangent, concentric, equal ------------
    {
        Document d;
        check(d.load(blank, &err), "load");
        const Element c1 = Element::makeCircle({50, 50}, 10, layer);
        const Element c2 = Element::makeCircle({90, 52}, 14, layer);
        const Element c3 = Element::makeCircle({53, 47}, 4, layer);
        const Element ln = Element::makePath({{0, 80}, {150, 85}}, false, layer);
        for (const Element &e : {c1, c2, c3, ln})
            d.addElement(e);
        auto C = [&](const Element &e) { return ref(GeoRef::Circle, e.id, -1); };
        d.parametricRef().params = {{"hole", "12 mm", ""}};
        addCon(d, Constraint::Diameter, {C(c1)}, "2 * hole");
        addCon(d, Constraint::Tangent, {C(c1), C(c2)});
        addCon(d, Constraint::Concentric, {C(c1), C(c3)});
        addCon(d, Constraint::Equal, {C(c2), C(c3)});
        addCon(d, Constraint::Horizontal, {ref(GeoRef::Line, ln.id, 0)});
        addCon(d, Constraint::Tangent, {ref(GeoRef::Line, ln.id, 0), C(c2)});
        const param::SolveResult r = param::solve(d);
        check(r.ok, "circle set solves");
        QPointF a, b, cc1, cc2, cc3;
        double r1, r2, r3;
        param::circle(d, C(c1), &cc1, &r1);
        param::circle(d, C(c2), &cc2, &r2);
        param::circle(d, C(c3), &cc3, &r3);
        param::line(d, ref(GeoRef::Line, ln.id, 0), &a, &b);
        check(approx(r1, 12), "diameter 2 * hole");
        check(approx(QLineF(cc1, cc2).length(), r1 + r2), "outside tangent");
        check(QLineF(cc1, cc3).length() < 1e-5, "concentric");
        check(approx(r2, r3), "equal radii");
        check(approx(a.y(), b.y()), "line horizontal");
        check(approx(std::fabs(a.y() - cc2.y()), r2), "line tangent to the circle");
        check(param::measure(d, d.parametric().constraints.first()) > 23.99, "measure diameter");
    }

    // ---- lines: parallel, perpendicular, collinear, midpoint, symmetric, angle
    {
        Document d;
        check(d.load(blank, &err), "load");
        const Element a = Element::makePath({{0, 0}, {50, 5}}, false, layer);
        const Element b = Element::makePath({{0, 20}, {40, 30}}, false, layer);
        const Element c = Element::makePath({{10, -10}, {14, 30}}, false, layer);
        const Element m = Element::makePoint({20, 3}, layer);
        const Element s1 = Element::makePoint({5, 40}, layer);
        const Element s2 = Element::makePoint({30, 45}, layer);
        for (const Element &e : {a, b, c, m, s1, s2})
            d.addElement(e);
        auto L = [&](const Element &e) { return ref(GeoRef::Line, e.id, 0); };
        auto P = [&](const Element &e) { return ref(GeoRef::Point, e.id, -1); };
        addCon(d, Constraint::Fix, {ref(GeoRef::Line, a.id, 0)});         // the whole of a
        addCon(d, Constraint::Parallel, {L(a), L(b)});
        addCon(d, Constraint::Perpendicular, {L(a), L(c)});
        addCon(d, Constraint::Midpoint, {P(m), L(a)});
        addCon(d, Constraint::Symmetric, {P(s1), P(s2), L(c)});
        addCon(d, Constraint::Distance, {ref(GeoRef::Point, b.id, 0), L(a)}, "15");
        const QPointF a0 = pt(d, ref(GeoRef::Point, a.id, 0)), a1 = pt(d, ref(GeoRef::Point, a.id, 1));
        const param::SolveResult r = param::solve(d);
        check(r.ok, "line set solves");
        check(QLineF(pt(d, ref(GeoRef::Point, a.id, 0)), a0).length() < 1e-12
              && QLineF(pt(d, ref(GeoRef::Point, a.id, 1)), a1).length() < 1e-12, "fixed line stays put");
        QPointF p, q, u, v;
        param::line(d, L(a), &p, &q);
        param::line(d, L(b), &u, &v);
        const QLineF la(p, q), lb(u, v);
        check(approx(std::fabs(la.angleTo(lb)), 0, 1e-4) || approx(std::fabs(la.angleTo(lb)), 360, 1e-4)
              || approx(la.angleTo(lb), 180, 1e-4), "parallel");
        param::line(d, L(c), &u, &v);
        const double ang = QLineF(p, q).angleTo(QLineF(u, v));
        check(approx(std::fmod(ang, 180.0), 90, 1e-4), "perpendicular");
        check(QLineF(pt(d, P(m)), (p + q) / 2).length() < 1e-5, "midpoint");
        const QPointF mid = (pt(d, P(s1)) + pt(d, P(s2))) / 2;
        const QPointF dir = v - u, ds = pt(d, P(s2)) - pt(d, P(s1));
        check(std::fabs(dir.x() * ds.x() + dir.y() * ds.y()) < 1e-4, "symmetric pair perpendicular to the line");
        check(std::fabs((dir.x() * (mid - u).y() - dir.y() * (mid - u).x())) < 1e-3, "symmetric pair centred on the line");

        // Angle between two free lines.
        const Element e1 = Element::makePath({{100, 0}, {150, 0}}, false, layer);
        const Element e2 = Element::makePath({{100, 0}, {140, 20}}, false, layer);
        d.addElement(e1);
        d.addElement(e2);
        addCon(d, Constraint::Fix, {L(e1)});
        addCon(d, Constraint::Coincident, {ref(GeoRef::Point, e2.id, 0), ref(GeoRef::Point, e1.id, 0)});
        addCon(d, Constraint::Angle, {L(e1), L(e2)}, "60");
        check(param::solve(d).ok, "angle solves");
        param::line(d, L(e2), &u, &v);
        check(approx(qRadiansToDegrees(std::atan2(v.y() - u.y(), v.x() - u.x())), 60, 1e-4), "60 degrees");

        // Collinear.
        const Element f1 = Element::makePath({{0, 100}, {30, 104}}, false, layer);
        const Element f2 = Element::makePath({{40, 90}, {70, 95}}, false, layer);
        d.addElement(f1);
        d.addElement(f2);
        addCon(d, Constraint::Collinear, {L(f1), L(f2)});
        check(param::solve(d).ok, "collinear solves");
        param::line(d, L(f1), &p, &q);
        param::line(d, L(f2), &u, &v);
        const QPointF dd = q - p;
        check(std::fabs(dd.x() * (u - p).y() - dd.y() * (u - p).x()) < 1e-3
              && std::fabs(dd.x() * (v - p).y() - dd.y() * (v - p).x()) < 1e-3, "collinear");
    }

    // ---- rectangle elements and polygons as constraint geometry -----------
    {
        Document d;
        check(d.load(blank, &err), "load");
        const Element r = Element::makeRectangle({40, 30}, 20, 10, layer);
        const Element pg = Element::makePolygon({100, 30}, 15, 6, layer);
        d.addElement(r);
        d.addElement(pg);
        addCon(d, Constraint::Coincident, {ref(GeoRef::Point, r.id, 0), GeoRef::origin()});
        addCon(d, Constraint::Distance, {ref(GeoRef::Line, r.id, 0)}, "60");
        addCon(d, Constraint::Distance, {ref(GeoRef::Line, r.id, 1)}, "25");
        addCon(d, Constraint::Coincident, {ref(GeoRef::Point, pg.id, -1), ref(GeoRef::Point, r.id, 2)});
        addCon(d, Constraint::Distance, {ref(GeoRef::Line, pg.id, 0)}, "10");
        check(param::solve(d).ok, "rect + polygon solve");
        const Element *re = d.elementById(r.id);
        check(re->geometryType == "rectangle", "a rectangle stays a rectangle");
        check(approx(re->raw.value("width").toDouble(), 60) && approx(re->raw.value("height").toDouble(), 25),
              "rectangle width/height driven");
        check(QLineF(pt(d, ref(GeoRef::Point, pg.id, -1)), QPointF(60, 25)).length() < 1e-5,
              "polygon centred on the corner");
        check(approx(d.elementById(pg.id)->raw.value("radius").toDouble(), 10), "hexagon edge = radius");
        check(param::freedom(d).fullyConstrained.contains(r.id), "rectangle fully constrained");
    }

    // ---- toolpath bindings ------------------------------------------------
    {
        Document d;
        check(d.load(blank, &err), "load");
        d.parametricRef().params = {{"depth", "stock_t + 0.5", ""}};
        Toolpath t;
        t.uuid = "tp1";
        t.type = "contour";
        t.json.insert("end_depth", QStringLiteral("-3.000"));
        t.json.insert("feed", 1000);
        QJsonObject binds;
        binds.insert("end_depth", QJsonObject{{"expr", "depth"}, {"neg", true}, {"str", true}});
        binds.insert("feed", QJsonObject{{"expr", "depth * 100"}});
        t.json.insert("phobic_expr", binds);
        d.addToolpath(t);
        const QStringList ch = param::applyToolpathBindings(d);
        check(ch == QStringList{"tp1"}, "bound toolpath changes");
        check(d.toolpaths().first().json.value("end_depth").toString() == "-19.500", "depth string, negative");
        check(approx(d.toolpaths().first().json.value("feed").toDouble(), 1950), "feed number");
        check(param::applyToolpathBindings(d).isEmpty(), "second pass changes nothing");
    }

    // ---- saved and loaded --------------------------------------------------
    {
        Document d;
        check(d.load(blank, &err), "load");
        const Element c = Element::makeCircle({50, 50}, 10, layer);
        d.addElement(c);
        d.parametricRef().params = {{"r", "8", "the hole"}};
        addCon(d, Constraint::Radius, {ref(GeoRef::Circle, c.id, -1)}, "r");
        addCon(d, Constraint::Fix, {ref(GeoRef::Point, c.id, -1)});
        d.parametricRef().constraints.last().at = QPointF(50, 50);
        check(param::solve(d).ok, "solve before save");
        const QString out = tmp.filePath("saved.c2d");
        check(d.save(out, &err), "save");
        Document back;
        check(back.load(out, &err), "load saved");
        check(!back.params().contains(Parametric::paramKey()), "the row is not left in params");
        const Parametric &p = back.parametric();
        check(p.params.size() == 1 && p.params.first().comment == "the hole", "parameter kept");
        check(p.constraints.size() == 2, "constraints kept");
        check(p.constraints.first().name == "d1" && p.constraints.first().expr == "r", "dimension kept");
        check(p.constraints.last().at == QPointF(50, 50), "fix position kept");
        check(approx(back.elementById(c.id)->raw.value("radius").toDouble(), 8), "solved radius saved");
        // Clearing everything removes the row.
        back.setParametric(Parametric());
        const QString out2 = tmp.filePath("cleared.c2d");
        check(back.save(out2, &err), "save cleared");
        Document again;
        check(again.load(out2, &err) && again.parametric().isEmpty(), "cleared stays cleared");

        // Deleting the circle leaves dangling constraints for prune().
        d.removeElementById(c.id);
        check(param::prune(d) == 2 && d.parametric().constraints.isEmpty(), "prune drops dangling");
    }

    std::printf("parametric: %d checks passed\n", g_checks);
    return 0;
}
