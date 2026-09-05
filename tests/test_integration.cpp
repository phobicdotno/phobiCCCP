// Cross-feature integration checks. The unit tests each cover one module in
// isolation; these drive the paths where two modules meet, which is where the
// contracts between them actually get exercised:
//
//   1. modeller -> CAM: a relief built by model3d reaches the 3D toolpaths
//      through the real heightmodel.h provider (not a test stub), and the
//      material simulation confirms the cut follows it.
//   2. save -> reopen -> machine: the same document written to a .c2d, closed,
//      and reloaded from disk still machines the same relief.
//   3. import -> vector op -> toolpath -> g-code: an SVG becomes elements,
//      a boolean welds them, a pocket cuts the result.
//
// Plain asserts, no framework; exits 0 on success.

#include "../src/c2ddocument.h"
#include "../src/element.h"
#include "../src/gcodeexport.h"
#include "../src/heightmodel.h"
#include "../src/importers.h"
#include "../src/model3d.h"
#include "../src/simulation.h"
#include "../src/vectorops.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUuid>

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

static bool approx(double a, double b, double eps)
{
    return std::fabs(a - b) <= eps;
}

// A toolpath JSON in the shape the exporter reads, with a ball cutter.
static QJsonObject finishToolpath(const QString &name)
{
    QJsonObject tool;
    tool.insert("number", 101);
    tool.insert("diameter", 3.0);
    tool.insert("angle", 0);
    tool.insert("corner_radius", 1.5);
    tool.insert("type", 1);
    QJsonObject speeds;
    speeds.insert("feedrate", 1000.0);
    speeds.insert("plungerate", 300.0);
    speeds.insert("rpm", 16000.0);
    QJsonObject j;
    j.insert("type", QStringLiteral("3d_finish_toolpath"));
    j.insert("uuid", QUuid::createUuid().toString());
    j.insert("name", name);
    j.insert("enabled", true);
    j.insert("tool", tool);
    j.insert("speeds", speeds);
    j.insert("stepover", 0.6);
    j.insert("raster_angle", 0.0);
    j.insert("stock_to_leave", 0.0);
    j.insert("elements", QJsonArray{});
    return j;
}

static void addToolpath(c2d::Document &doc, const QJsonObject &j)
{
    c2d::Toolpath t;
    t.uuid = j.value("uuid").toString();
    t.type = j.value("type").toString();
    t.json = j;
    doc.addToolpath(t);
}

// Deepest cut anywhere in the program.
static double lowestFeedZ(const QVector<c2d::Op> &ops)
{
    double z = 0;
    for (const c2d::Op &o : ops)
        if (o.kind == c2d::Op::Feed || o.kind == c2d::Op::Arc)
            z = std::fmin(z, o.z);
    return z;
}

int main(int argc, char *argv[])
{
    const QString sample = argc > 1
        ? QString::fromLocal8Bit(argv[1])
        : QDir::homePath() + QStringLiteral("/Documents/c2d-samples/"
                                            "Shapeoko-3-XXL-Baseplate-18-Mounting-Holes-No-Path.c2d");
    if (!QFile::exists(sample)) {
        std::fprintf(stderr, "note: sample %s not found, integration checks skipped\n",
                     sample.toUtf8().constData());
        return 77;  // CTest SKIP_RETURN_CODE: visible as skipped, never as passed
    }

    QTemporaryDir tmp;
    check(tmp.isValid(), "temp dir");
    c2d::installModelProvider();

    // ---- 1. modeller -> CAM through the real provider -----------------------
    QString modelledPath;
    {
        c2d::Document doc;
        QString err;
        check(doc.load(sample, &err), "sample loads");

        // A dome over a circle that exists in the document.
        QString circleId;
        double cx = 0, cy = 0, cr = 0;
        for (const c2d::Element &e : doc.elements())
            if (e.geometryType == QLatin1String("circle")) {
                const QRectF bb = e.painterPath.boundingRect();
                if (bb.width() > cr * 2) {
                    circleId = e.id;
                    cr = bb.width() / 2;
                    cx = bb.center().x();
                    cy = bb.center().y();
                }
            }
        check(!circleId.isEmpty() && cr > 1.0, "sample has a circle to model");

        c2d::ModelStore *store = c2d::ModelStore::forDocument(doc);
        check(store != nullptr, "a store exists for the document");
        store->attach(&doc);
        c2d::ModelComponent comp;
        comp.id = c2d::Model3D::newId();
        comp.name = QStringLiteral("dome");
        comp.kind = c2d::ModelComponent::FromVectors;
        comp.shape = c2d::ModelComponent::Round;
        comp.height = 4.0;
        comp.vectorIds = QStringList{circleId};
        store->model.components.append(comp);
        store->model.resolution = 0.4;
        store->invalidate();

        // The CAM side must see it through heightModelFor(), with no test stub.
        const c2d::HeightModel *hm = c2d::heightModelFor(doc);
        check(hm && hm->valid(), "provider returns the composited relief");
        check(approx(hm->baseZ, -4.0, 1e-6), "relief floor is the component height");
        check(approx(hm->sample(cx, cy), 0.0, 0.05), "dome peaks at the stock top");
        check(hm->sample(cx, cy) > hm->sample(cx + cr * 0.9, cy) + 1.0,
              "dome falls away towards the rim");

        addToolpath(doc, finishToolpath(QStringLiteral("finish")));
        const c2d::GcodeResult g = c2d::exportGcode(doc);
        check(g.done.contains(QStringLiteral("finish")) && g.skipped.isEmpty(),
              "3D finish exports over the modelled relief");
        check(lowestFeedZ(g.ops) < -3.0, "the finish reaches down the dome");

        // The simulated cut must reproduce the relief, never break through it.
        c2d::SimSettings ss;
        ss.stockW = doc.boardWidth();
        ss.stockH = doc.boardHeight();
        ss.stockT = 19.05;
        ss.minCell = 0.4;
        const c2d::SimResult sim = c2d::simulate(g.ops, c2d::toolGeometry(doc), ss);
        check(!sim.map.isNull(), "simulated");
        double worst = 1e9, peak = -1e9;
        for (int iy = 0; iy < sim.map.height(); ++iy)
            for (int ix = 0; ix < sim.map.width(); ++ix) {
                const double x = sim.map.cellCenterX(ix), y = sim.map.cellCenterY(iy);
                if (std::hypot(x - cx, y - cy) > cr * 0.6)
                    continue;                       // dome interior only
                const double z = sim.map.at(ix, iy);
                peak = std::fmax(peak, z);
                // Half a cell of lateral slop, as elsewhere: the map holds one
                // depth per cell while the model is sampled at its centre.
                double m = hm->sample(x, y);
                for (int k = 0; k < 8; ++k) {
                    const double a = k * M_PI / 4, d = 0.5 * M_SQRT2 * sim.map.cellSize();
                    m = std::fmin(m, hm->sample(x + d * std::cos(a), y + d * std::sin(a)));
                }
                worst = std::fmin(worst, z - m);
            }
        std::printf("modelled dome: closest approach %.4f mm, cut peak %.4f mm\n", worst, peak);
        check(worst >= -0.05, "the cut never breaks through the modelled surface");
        check(peak > -0.5, "the dome's peak is left standing");

        modelledPath = tmp.filePath(QStringLiteral("modelled.c2d"));
        check(doc.save(modelledPath, &err), "document with a model saves");
        check(store->model.saveTo(modelledPath, &err), "model saves into the .c2d");
        store->detach();
    }

    // ---- 2. reopen the saved file and machine it again -----------------------
    {
        c2d::Document doc;
        QString err;
        check(doc.load(modelledPath, &err), "saved document reloads");
        // No store is attached: the provider must lazily read the model back
        // out of the file, which is what the app does on File > Open.
        const c2d::HeightModel *hm = c2d::heightModelFor(doc);
        check(hm && hm->valid(), "the relief survives save and reopen");
        check(approx(hm->baseZ, -4.0, 1e-6), "reloaded relief keeps its depth");

        addToolpath(doc, finishToolpath(QStringLiteral("finish2")));
        const c2d::GcodeResult g = c2d::exportGcode(doc);
        check(g.done.contains(QStringLiteral("finish2")), "reloaded model still machines");
        check(lowestFeedZ(g.ops) < -3.0, "reloaded finish reaches down the dome");
    }

    // ---- 3. import -> boolean -> toolpath -> g-code --------------------------
    {
        const QString svgPath = tmp.filePath(QStringLiteral("two.svg"));
        QFile f(svgPath);
        check(f.open(QIODevice::WriteOnly), "write svg");
        // Two overlapping squares, 40x40 mm page, 10 mm each, overlapping 5 mm.
        f.write("<svg xmlns='http://www.w3.org/2000/svg' width='40mm' height='40mm'"
                " viewBox='0 0 40 40'>"
                "<rect x='5' y='5' width='10' height='10'/>"
                "<rect x='10' y='10' width='10' height='10'/></svg>");
        f.close();

        c2d::Document doc;
        QString err;
        check(doc.load(sample, &err), "sample loads for import");
        const int before = doc.elements().size();
        c2d::ImportOptions opt;
        opt.stockWidth = doc.boardWidth();
        opt.stockHeight = doc.boardHeight();
        opt.layer = doc.defaultLayer();
        const c2d::ImportResult ir = c2d::importFile(svgPath, opt);
        check(ir.ok && ir.elements.size() == 2, "svg imports two rectangles");
        for (const c2d::Element &e : ir.elements)
            doc.addElement(e);
        check(doc.elements().size() == before + 2, "imported elements land in the document");

        // Weld them: overlapping squares make one region, so one outline.
        const QVector<c2d::Element> welded =
            c2d::vec::booleanElements(ir.elements, c2d::vec::BoolOp::Union);
        check(welded.size() == 1, "union of two overlapping squares is one ring");
        const QRectF wb = welded.first().painterPath.boundingRect();
        check(wb.width() > 14.0 && wb.height() > 14.0, "the welded outline spans both squares");

        // Replace the inputs with the weld, then pocket it.
        for (const c2d::Element &e : ir.elements)
            doc.removeElementById(e.id);
        for (const c2d::Element &e : welded)
            doc.addElement(e);

        QJsonObject tool;
        tool.insert("number", 201);
        tool.insert("diameter", 3.175);
        tool.insert("angle", 0);
        QJsonObject speeds;
        speeds.insert("feedrate", 1000.0);
        speeds.insert("plungerate", 300.0);
        speeds.insert("rpm", 16000.0);
        QJsonArray refs;
        for (const c2d::Element &e : welded)
            refs.append(QJsonObject{{"uuid", e.id}});
        QJsonObject j;
        j.insert("type", QStringLiteral("pocket_toolpath"));
        j.insert("uuid", QUuid::createUuid().toString());
        j.insert("name", QStringLiteral("welded pocket"));
        j.insert("enabled", true);
        j.insert("tool", tool);
        j.insert("speeds", speeds);
        j.insert("elements", refs);
        j.insert("start_depth", QStringLiteral("0.000"));
        j.insert("end_depth", QStringLiteral("-2.000"));
        j.insert("stepdown", 1.0);
        j.insert("stepover", 1.5);
        addToolpath(doc, j);

        const c2d::GcodeResult g = c2d::exportGcode(doc);
        check(g.done.contains(QStringLiteral("welded pocket")), "the welded outline pockets");
        check(approx(lowestFeedZ(g.ops), -2.0, 1e-3), "pocket reaches its end depth");
    }

    std::printf("test_integration: %d checks OK\n", g_checks);
    return 0;
}
