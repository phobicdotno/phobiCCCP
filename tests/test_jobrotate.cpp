// Rotate 90° (src/jobrotate.*): the whole design a quarter turn clockwise on
// the same stock. Four turns give back exactly what was there; one turn cuts
// exactly what the original cut, turned (the material-removal simulation of
// both, compared cell by cell); constraints still hold, so a solve moves
// nothing; the 3D model's images, meshes and textures turn with the vectors.
// Plain asserts, exits 0 on success.

#include "../src/backgroundimage.h"
#include "../src/c2ddocument.h"
#include "../src/element.h"
#include "../src/gcodeexport.h"
#include "../src/jobrotate.h"
#include "../src/model3d.h"
#include "../src/parametric.h"
#include "../src/simulation.h"

#include <QBuffer>
#include <QGuiApplication>
#include <QImage>
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

static bool approx(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

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
    if (c.isDimension())
        c.name = d.parametric().nextDimensionName();
    param::prime(d, c);
    if (!expr.isEmpty())
        c.expr = expr;
    else if (c.isDimension())
        c.expr = param::formatNumber(c.value);
    d.parametricRef().constraints.append(c);
    return c.uid;
}

static void addToolpath(Document &doc, const QString &type, const QString &name,
                        const QStringList &ids, QJsonObject extra)
{
    Toolpath tp;
    tp.uuid = QStringLiteral("{tp-%1}").arg(name);
    tp.type = type;
    QJsonArray els;
    for (const QString &id : ids)
        els.append(QJsonObject{{"uuid", id}});
    QJsonObject j{{"type", type}, {"name", name}, {"enabled", true}, {"uuid", tp.uuid},
                  {"start_depth", QStringLiteral("0.000")}, {"end_depth", QStringLiteral("3.000")},
                  {"stepdown", 1.0}, {"stepover", 2.0}, {"elements", els},
                  {"speeds", QJsonObject{{"feedrate", 1000}, {"plungerate", 300}, {"rpm", 10000}}},
                  {"tool", QJsonObject{{"diameter", 3.175}, {"number", 201}}}};
    for (auto it = extra.begin(); it != extra.end(); ++it)
        j.insert(it.key(), it.value());
    tp.json = j;
    doc.addToolpath(tp);
}

static QByteArray png(const QImage &img)
{
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return bytes;
}

// The test design: every kind of vector, a toolpath of each kind that has a
// direction or tabs, and constraints of every sort the turn has to handle.
// Its bounds (and so the turn) are whole millimetres.
struct Design {
    Document doc;
    QString rect, circle, tri, hex, point, line, text;
    QString originDim, boundDim;
};

static Design makeDesign(bool withText)
{
    Design d;
    Document &doc = d.doc;
    doc.setParam("width", "200");
    doc.setParam("height", "200");
    doc.setParam("thickness", "20");
    const QJsonObject layer = doc.defaultLayer();
    const Element rect = Element::makeRectangle({50, 40}, 60, 30, layer);
    const Element circle = Element::makeCircle({130, 50}, 15, layer);
    const Element tri = Element::makePath({{30, 90}, {80, 90}, {60, 130}}, true, layer);
    const Element hex = Element::makePolygon({140, 110}, 20, 6, layer);
    const Element point = Element::makePoint({100, 150}, layer);
    const Element line = Element::makePath({{20, 160}, {70, 170}}, false, layer);
    for (const Element &e : {rect, circle, tri, hex, point, line})
        doc.addElement(e);
    d.rect = rect.id; d.circle = circle.id; d.tri = tri.id; d.hex = hex.id;
    d.point = point.id; d.line = line.id;
    if (withText) {
        const Element text = Element::makeText(QStringLiteral("Rq"), {90, 60}, 12,
                                               QStringLiteral("Sans"), layer);
        doc.addElement(text);
        d.text = text.id;
    }

    addToolpath(doc, "contour", "outline", {d.rect}, {{"ofset_dir", 1}, {"ignore_tabs", false},
                {"tab_width", 6.0}, {"tab_height", 1.5}, {"climb", true}, {"lead_radius", 0.0}});
    addToolpath(doc, "contour", "hexagon", {d.hex}, {{"ofset_dir", -1}, {"climb", false}});
    addToolpath(doc, "pocket_toolpath", "hole", {d.circle}, {{"angle", 20.0}, {"helix_entry", false}});
    addToolpath(doc, "engrave_toolpath", "hatch", {d.tri}, {{"mode", "fill"}, {"line_spacing", 1.5},
                {"angle", 30.0}, {"crosshatch", true}, {"end_depth", QStringLiteral("0.500")}});
    addToolpath(doc, "engrave_toolpath", "stroke", {d.line}, {{"mode", "outline"},
                {"end_depth", QStringLiteral("0.500")}});
    addToolpath(doc, "drilling_toolpath", "drill", {d.point}, {{"angle", 0.0}, {"peck_distance", 0}});

    // Geometry: on the triangle (horizontal edge, its length, a pinned
    // corner), between shapes, and two tied to the board's origin.
    addCon(doc, Constraint::Horizontal, {ref(GeoRef::Line, d.tri, 0)});
    addCon(doc, Constraint::Distance, {ref(GeoRef::Point, d.tri, 0), ref(GeoRef::Point, d.tri, 1)}, "50");
    addCon(doc, Constraint::Fix, {ref(GeoRef::Point, d.tri, 2)});
    addCon(doc, Constraint::HDistance, {ref(GeoRef::Point, d.rect, 0), ref(GeoRef::Point, d.circle, -1)});
    addCon(doc, Constraint::VDistance, {ref(GeoRef::Point, d.hex, -1), ref(GeoRef::Point, d.point, -1)});
    d.originDim = addCon(doc, Constraint::HDistance, {GeoRef::origin(), ref(GeoRef::Point, d.circle, -1)}, "130");
    return d;
}

static void expectSameDoc(const Document &a, const Document &b, const char *what)
{
    check(a.elements().size() == b.elements().size(), what);
    for (int i = 0; i < a.elements().size(); ++i) {
        const bool same = a.elements().at(i).raw == b.elements().at(i).raw;
        if (!same)
            std::fprintf(stderr, "element %d (%s) differs\n", i,
                         qPrintable(a.elements().at(i).geometryType));
        check(same, what);
    }
    check(a.toolpaths().size() == b.toolpaths().size(), what);
    for (int i = 0; i < a.toolpaths().size(); ++i)
        check(a.toolpaths().at(i).json == b.toolpaths().at(i).json, what);
    const auto &ca = a.parametric().constraints, &cb = b.parametric().constraints;
    check(ca.size() == cb.size(), what);
    for (int i = 0; i < ca.size(); ++i) {
        check(ca.at(i).type == cb.at(i).type && ca.at(i).refs == cb.at(i).refs
                  && ca.at(i).sense == cb.at(i).sense && ca.at(i).expr == cb.at(i).expr, what);
        check(approx(ca.at(i).value, cb.at(i).value, 1e-9) && QLineF(ca.at(i).at, cb.at(i).at).length() < 1e-9
                  && QLineF(ca.at(i).label, cb.at(i).label).length() < 1e-9, what);
    }
}

// The material the design's program removes, simulated.
static SimResult cut(Document &doc)
{
    const GcodeResult g = exportGcode(doc);
    check(!g.cancelled && g.skipped.isEmpty(), "every toolpath exports");
    SimSettings s;
    s.stockW = doc.boardWidth();
    s.stockH = doc.boardHeight();
    s.stockT = 20;
    s.minCell = 0.25;
    return simulate(g.ops, toolGeometry(doc), s);
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    // ---- the turn itself ---------------------------------------------------
    {
        const QRectF b(20, 25, 140, 145);
        const QTransform q = jobrotate::quarterTurn(b);
        check(q.mapRect(b) == QRectF(20, 25, 145, 140), "the turn keeps the bottom-left corner, swaps the size");
        check(q.map(QPointF(20, 25)) == QPointF(20, 165) && q.map(QPointF(160, 25)) == QPointF(20, 25),
              "clockwise: the bottom-left goes to the top-left, the bottom-right to the bottom-left");
        check(jobrotate::turnAngle(0) == -90 && jobrotate::turnAngle(-90) == 180
                  && jobrotate::turnAngle(180) == 90 && jobrotate::turnAngle(90) == 0,
              "a direction turns clockwise and comes back after four turns");
    }

    // ---- four turns are no turn --------------------------------------------
    {
        Design d = makeDesign(true);
        const Document original = d.doc;
        check(param::unsatisfied(original).isEmpty(), "the test design holds every constraint");
        Document doc = d.doc;
        QRectF firstBounds;
        for (int k = 0; k < 4; ++k) {
            const QRectF b = jobrotate::designBounds(doc);
            if (k == 0)
                firstBounds = b;
            const jobrotate::Report rep = jobrotate::turnDocument(doc, jobrotate::quarterTurn(b));
            check(rep.vectors == 7, "every vector turns");
            check(rep.toolpaths == 2, "the pocket's and the hatch's directions turn, the drill's does not");
            check(rep.remeasured == 1 && rep.notHeld.isEmpty(),
                  "the plain dimension from the origin takes its new value; nothing else breaks");
            check(param::unsatisfied(doc).isEmpty(), "every constraint still holds after a turn");
            const QRectF nb = jobrotate::designBounds(doc);
            check(approx(nb.left(), b.left(), 1e-6) && approx(nb.top(), b.top(), 1e-6)
                      && approx(nb.width(), b.height(), 1e-6) && approx(nb.height(), b.width(), 1e-6),
                  "the turned design keeps its corner and lies the other way");
            const Element *r = doc.elementById(d.rect);
            check(r && r->geometryType == QLatin1String("rectangle"), "a rectangle stays a rectangle");
            check(doc.elementById(d.hex)->geometryType == QLatin1String("regular_polygon"),
                  "a regular polygon stays one");
            if (k == 0) {
                check(r->raw.value("width").toDouble() == 30 && r->raw.value("height").toDouble() == 60,
                      "its width and height swap");
                const Constraint &h = doc.parametric().constraints.at(0);
                check(h.type == Constraint::Vertical, "Horizontal turns into Vertical");
                const Toolpath *hatch = doc.toolpathByUuid(QStringLiteral("{tp-hatch}"));
                check(hatch->json.value("angle").toDouble() == -60, "the hatch angle turns 90° clockwise");
                check(doc.toolpathByUuid(QStringLiteral("{tp-drill}"))->json.value("angle").toDouble() == 0,
                      "a drill's angle is no direction and stays");
                // Toolpaths still machine the same vectors.
                check(doc.toolpathByUuid(QStringLiteral("{tp-outline}"))->json == original.toolpaths().at(0).json,
                      "a toolpath without a direction is untouched");
            }
        }
        expectSameDoc(original, doc, "four turns give back the design exactly");
        check(jobrotate::designBounds(doc) == firstBounds, "and where it was");
    }

    // ---- an expression bound to a direction, and one tied to the origin -----
    {
        Design d = makeDesign(false);
        Document &doc = d.doc;
        Toolpath *pocket = doc.toolpathByUuid(QStringLiteral("{tp-hole}"));
        pocket->json.insert("phobic_expr", QJsonObject{{"angle", QJsonObject{{"expr", "a"}}}});
        // A dimension from the origin other expressions use cannot just
        // take a new value: it is reported, and left as it was.
        d.boundDim = addCon(doc, Constraint::VDistance, {GeoRef::origin(), ref(GeoRef::Point, d.point, -1)});
        const Constraint &bound = doc.parametric().constraints.last();
        doc.parametricRef().params.append({QStringLiteral("a"), QStringLiteral("20"), QString()});
        doc.parametricRef().params.append({QStringLiteral("twice"), bound.name + QStringLiteral(" * 2"), QString()});
        const Document original = doc;
        QStringList exprs;
        for (int k = 0; k < 4; ++k) {
            const jobrotate::Report rep = jobrotate::turnDocument(doc, jobrotate::quarterTurn(jobrotate::designBounds(doc)));
            // Red after three turns, true again after the fourth.
            check(rep.notHeld == (k < 3 ? QStringList{d.boundDim} : QStringList()),
                  "a used dimension from the origin is reported while it does not hold");
            exprs << doc.toolpathByUuid(QStringLiteral("{tp-hole}"))->json.value("phobic_expr").toObject()
                         .value("angle").toObject().value("expr").toString();
            const int k2 = doc.parametric().indexOfUid(d.boundDim);
            check(doc.parametric().constraints.at(k2).expr == original.parametric().constraints.at(k2).expr,
                  "and keeps its value");
        }
        check(exprs == QStringList({"(a) - 90", "(a) - 180", "(a) - 270", "a"}),
              "a bound direction's expression turns too, and comes back");
        for (int i = 0; i < doc.elements().size(); ++i)
            check(doc.elements().at(i).raw == original.elements().at(i).raw, "geometry back after four turns");
    }

    // ---- one turn cuts the same ---------------------------------------------
    {
        Design d = makeDesign(false);
        const SimResult before = cut(d.doc);
        Document turned = d.doc;
        const QRectF b = jobrotate::designBounds(turned);
        check(b == QRectF(20, 25, 140, 145), "the cut test's bounds are whole millimetres");
        const QTransform q = jobrotate::quarterTurn(b);
        jobrotate::turnDocument(turned, q);
        const SimResult after = cut(turned);
        check(before.map.width() == after.map.width() && approx(before.map.cellSize(), 0.25),
              "both simulated on the same grid");
        // Cell (i, j) of the original lies at cell (j + dx/c, dy/c - 1 - i) turned.
        const double c = before.map.cellSize();
        const int dx = int(std::lround(q.dx() / c)), dy = int(std::lround(q.dy() / c));
        int cutCells = 0, differ = 0;
        double removedA = 0, removedB = 0, worst = 0;
        for (int j = 0; j < before.map.height(); ++j)
            for (int i = 0; i < before.map.width(); ++i) {
                const float za = before.map.at(i, j);
                removedA -= za;
                if (za < 0)
                    ++cutCells;
                const int ti = j + dx, tj = dy - 1 - i;
                if (ti < 0 || tj < 0 || ti >= after.map.width() || tj >= after.map.height()) {
                    check(za == 0, "nothing cut that would turn off the stock");
                    continue;
                }
                const double diff = std::fabs(za - after.map.at(ti, tj));
                worst = std::max(worst, diff);
                if (diff > 0.02)
                    ++differ;
            }
        for (float z : after.map.data())
            removedB -= z;
        std::printf("cut cells %d, differing %d, worst %.3f mm, removed %.1f / %.1f\n",
                    cutCells, differ, worst, removedA, removedB);
        check(cutCells > 10000, "the test design cuts something");
        check(differ <= cutCells / 1000, "the turned design cuts the same cells, turned");
        check(std::fabs(removedA - removedB) <= 0.002 * removedA, "and removes the same material");
        check(before.rapidHitCount == 0 && after.rapidHitCount == 0, "no rapid through material either way");
    }

    // ---- constraints hold: a solve moves nothing ------------------------------
    {
        Design d = makeDesign(false);
        Document doc = d.doc;
        jobrotate::turnDocument(doc, jobrotate::quarterTurn(jobrotate::designBounds(doc)));
        const QVector<Element> turned = doc.elements();
        const param::SolveResult r = param::solve(doc);
        check(r.ok, "the turned design solves");
        for (int i = 0; i < turned.size(); ++i) {
            const QRectF a = turned.at(i).painterPath.boundingRect(), b = doc.elements().at(i).painterPath.boundingRect();
            check(QLineF(a.topLeft(), b.topLeft()).length() < 1e-6 && QLineF(a.bottomRight(), b.bottomRight()).length() < 1e-6,
                  "and the solve leaves every shape where the turn put it");
        }
    }

    // ---- the 3D model turns with the vectors -----------------------------------
    {
        Document doc;
        doc.setParam("width", "200");
        doc.setParam("height", "200");
        const QJsonObject layer = doc.defaultLayer();
        const Element blob = Element::makeRectangle({60, 140}, 40, 20, layer);
        doc.addElement(blob);
        QImage img(4, 2, QImage::Format_Grayscale8);
        const uchar shades[8] = {0, 40, 90, 140, 255, 200, 150, 10};   // nothing symmetric
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 4; ++x)
                img.setPixel(x, y, qRgb(shades[y * 4 + x], shades[y * 4 + x], shades[y * 4 + x]));
        Model3D model;
        model.resolution = 0.5;
        ModelComponent pic;
        pic.id = Model3D::newId(); pic.kind = ModelComponent::ImageHeightmap; pic.data = png(img);
        pic.x = 10; pic.y = 20; pic.width = 40; pic.height = 4; pic.combine = ModelComponent::Merge;
        ModelComponent mesh;
        mesh.id = Model3D::newId(); mesh.kind = ModelComponent::StlMesh; mesh.combine = ModelComponent::Merge;
        mesh.data = QByteArrayLiteral("solid w\nfacet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex 10 0 5\n"
                                      "vertex 0 6 2\nendloop\nendfacet\nendsolid w\n");
        mesh.x = 100; mesh.y = 30; mesh.height = 0;
        ModelComponent vec;
        vec.id = Model3D::newId(); vec.kind = ModelComponent::FromVectors; vec.vectorIds = {blob.id};
        vec.shape = ModelComponent::Round; vec.height = 3; vec.combine = ModelComponent::Merge;
        model.components = {pic, mesh, vec};
        check(model.components.at(0).footprint() == QRectF(10, 20, 40, 20), "an image's footprint");
        check(model.components.at(1).footprint() == QRectF(100, 30, 10, 6), "a mesh's footprint");

        const QRectF b = jobrotate::designBounds(doc, &model);
        check(b == QRectF(10, 20, 100, 130), "the model's images and meshes count in the bounds");
        const QTransform q = jobrotate::quarterTurn(b);
        const HeightModel hmA = buildHeightModel(model, doc.elements(), 200, 200);
        Document doc2 = doc;
        jobrotate::turnDocument(doc2, q);
        Model3D model2 = model;
        jobrotate::turnModel(model2, q);
        check(model2.components.at(0).turns == 1 && model2.components.at(0).footprint().size() == QSizeF(20, 40),
              "a turned image lies the other way");
        const HeightModel hmB = buildHeightModel(model2, doc2.elements(), 200, 200);
        check(hmA.valid() && hmB.valid() && approx(hmA.cell, 0.5) && hmA.cols == hmB.cols, "both reliefs built");
        const int dx = int(std::lround(q.dx() / hmA.cell)), dy = int(std::lround(q.dy() / hmA.cell));
        int inside = 0, differ = 0;
        for (int r = 0; r < hmA.rows; ++r)
            for (int c = 0; c < hmA.cols; ++c) {
                const float za = hmA.at(c, r);
                const float zb = hmB.at(r + dx, dy - 1 - c);
                if (za == HeightModel::NoModel) {
                    if (zb != HeightModel::NoModel)
                        ++differ;
                    continue;
                }
                ++inside;
                if (zb == HeightModel::NoModel || std::fabs(za - zb) > 1e-3)
                    ++differ;
            }
        std::printf("relief cells %d, differing %d\n", inside, differ);
        check(inside > 3000, "the relief covers something");
        check(differ <= inside / 200, "the turned relief is the original, turned");

        Model3D back = model;
        Document docBack = doc;
        for (int k = 0; k < 4; ++k) {
            const QTransform qk = jobrotate::quarterTurn(jobrotate::designBounds(docBack, &back));
            jobrotate::turnDocument(docBack, qk);
            jobrotate::turnModel(back, qk);
        }
        for (int i = 0; i < 3; ++i)
            check(approx(back.components.at(i).x, model.components.at(i).x, 1e-9)
                      && approx(back.components.at(i).y, model.components.at(i).y, 1e-9)
                      && back.components.at(i).turns == 0,
                  "four turns put every model part back");
        check(ModelComponent::fromJson(model2.components.at(0).toJson()).turns == 1, "the turn is saved");
    }

    // ---- the background turns about its own centre --------------------------
    {
        BackgroundImage bg;
        bg.setImage(QImage(100, 50, QImage::Format_RGB32));
        bg.x = 10; bg.y = 20; bg.scale = 1; bg.rotationDeg = 0;
        const QRectF r0 = bg.rectMm();
        const QTransform q = jobrotate::quarterTurn(QRectF(0, 0, 100, 100));
        jobrotate::turnBackground(bg, q);
        check(QLineF(bg.rectMm().center(), q.map(r0.center())).length() < 1e-9 && bg.rotationDeg == -90,
              "the background turns with the design");
        for (int k = 0; k < 3; ++k)
            jobrotate::turnBackground(bg, q);
        check(bg.rotationDeg == 0, "and comes back to its own angle after four turns");
    }

    std::printf("test_jobrotate: %d checks OK\n", g_checks);
    return 0;
}
