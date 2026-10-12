// Headless checks for the 3D modeller (src/model3d.*): component profiles
// from vectors, combine modes, image heightmaps, STL rasterisation, the
// distance transform and .c2d persistence. Plain asserts; exits 0 on success.

#include "../src/c2ddocument.h"
#include "../src/element.h"
#include "../src/heightmodel.h"
#include "../src/model3d.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

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

static bool approx(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

static double zAt(const HeightModel &hm, double x, double y)
{
    const int c = int(std::floor((x - hm.originX) / hm.cell));
    const int r = int(std::floor((y - hm.originY) / hm.cell));
    return hm.at(c, r);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);   // QSqlDatabase + QImage plugins
    const QJsonObject layer;

    // ---- distance transform: circle inradius ---------------------------
    {
        const int n = 41;
        QVector<unsigned char> m(n * n, 0);
        for (int r = 0; r < n; ++r)
            for (int c = 0; c < n; ++c)
                if ((c - 20) * (c - 20) + (r - 20) * (r - 20) <= 15 * 15)
                    m[r * n + c] = 1;
        const QVector<float> d = distanceTransform(m, n, n);
        check(approx(d.at(20 * n + 20), 16.0, 1.0), "EDT: centre distance ~ radius");
        check(d.at(0) == 0.0f, "EDT: outside is 0");
        check(d.at(20 * n + 6) >= 0.9f && d.at(20 * n + 6) <= 2.1f, "EDT: rim cell ~1");
    }

    // ---- round component from a circle ---------------------------------
    {
        QVector<Element> els;
        Element circle = Element::makeCircle(QPointF(50, 40), 20, layer);
        els << circle;
        Model3D m;
        m.resolution = 0.5;
        ModelComponent c;
        c.id = Model3D::newId();
        c.kind = ModelComponent::FromVectors;
        c.shape = ModelComponent::Round;
        c.height = 8;
        c.vectorIds << circle.id;
        m.components << c;
        const HeightModel hm = buildHeightModel(m, els, 100, 80);
        check(hm.valid(), "round: model built");
        check(approx(hm.cell, 0.5, 1e-9), "round: requested cell size honoured");
        check(approx(hm.baseZ, -8, 0.05), "round: baseZ = -height");
        check(approx(zAt(hm, 50, 40), 0, 0.05), "round: z at centre = 0");
        // rim cell centre is cell/2 inside: a hemisphere rises h*sqrt(2*e/R) there
        {
            const double rim = zAt(hm, 69.5, 40);
            const double rise = 8 * std::sqrt(2 * (hm.cell / 2) / 20);
            check(rim >= -8 - 1e-6 && rim <= -8 + rise + 0.6, "round: z at rim = -h (+/- cell)");
            check(zAt(hm, 70.3, 40) == HeightModel::NoModel, "round: just outside the rim is NoModel");
        }
        // half-way to the rim on a hemisphere: sqrt(1 - 0.5^2) = 0.866
        check(approx(zAt(hm, 60, 40), -8 + 8 * 0.866, 0.35), "round: circular cross-section");
        check(hm.at(1, 1) == HeightModel::NoModel, "round: outside the circle is NoModel");
        check(approx(hm.sample(50, 40), 0, 0.05), "round: bilinear sample at the top");
        check(approx(hm.sample(2, 2), hm.baseZ, 1e-6), "round: sample outside = baseZ");
    }

    // ---- angle component: 45 deg slope ---------------------------------
    {
        QVector<Element> els;
        Element rect = Element::makeRectangle(QPointF(50, 40), 40, 20, layer);
        els << rect;
        Model3D m;
        m.resolution = 0.25;
        ModelComponent c;
        c.id = Model3D::newId();
        c.shape = ModelComponent::Angle;
        c.angleDeg = 45;
        c.height = 100;   // no cap: a roof
        c.vectorIds << rect.id;
        m.components << c;
        const HeightModel hm = buildHeightModel(m, els, 100, 80);
        check(hm.valid(), "angle: model built");
        check(approx(hm.baseZ, -10, 0.4), "angle: ridge height = inradius * tan(45)");
        check(approx(zAt(hm, 50, 40), 0, 0.3), "angle: ridge at 0");
        check(approx(zAt(hm, 50, 35), -5, 0.4), "angle: 5 mm from the edge -> 5 mm down");
        check(approx(zAt(hm, 50, 32), -8, 0.4), "angle: 2 mm from the edge -> 8 mm down");
        // capped height
        m.components[0].height = 4;
        const HeightModel hc = buildHeightModel(m, els, 100, 80);
        check(approx(hc.baseZ, -4, 0.05), "angle capped: base = -4");
        check(approx(zAt(hc, 50, 40), 0, 0.05), "angle capped: plateau at 0");
        check(approx(zAt(hc, 50, 32), -2, 0.4), "angle capped: slope below the plateau");
    }

    // ---- flat + subtract -----------------------------------------------
    {
        QVector<Element> els;
        Element big = Element::makeCircle(QPointF(50, 40), 30, layer);
        Element small = Element::makeCircle(QPointF(50, 40), 10, layer);
        els << big << small;
        Model3D m;
        m.resolution = 0.5;
        ModelComponent a;
        a.id = Model3D::newId(); a.shape = ModelComponent::Flat; a.height = 10; a.vectorIds << big.id;
        ModelComponent b;
        b.id = Model3D::newId(); b.shape = ModelComponent::Flat; b.height = 4;
        b.combine = ModelComponent::Subtract; b.vectorIds << small.id;
        m.components << a << b;
        const HeightModel hm = buildHeightModel(m, els, 100, 80);
        check(approx(hm.baseZ, -10, 1e-4), "subtract: base -10");
        check(approx(zAt(hm, 50, 40), -4, 1e-4), "subtract: centre 4 mm down");
        check(approx(zAt(hm, 70, 40), 0, 1e-4), "subtract: ring untouched at 0");
        // disabled component drops out
        m.components[1].enabled = false;
        check(approx(zAt(buildHeightModel(m, els, 100, 80), 50, 40), 0, 1e-4), "disabled component ignored");
        // merge keeps the max
        m.components[1].enabled = true;
        m.components[1].combine = ModelComponent::Merge;
        m.components[1].height = 12;
        const HeightModel hmm = buildHeightModel(m, els, 100, 80);
        check(approx(hmm.baseZ, -12, 1e-4) && approx(zAt(hmm, 50, 40), 0, 1e-4)
              && approx(zAt(hmm, 70, 40), -2, 1e-4), "merge: max of the two");
        // base height lifts the whole region
        m.components[1].combine = ModelComponent::Add;
        m.components[1].height = 1;
        m.components[1].baseHeight = 2;
        const HeightModel hb = buildHeightModel(m, els, 100, 80);
        check(approx(hb.baseZ, -13, 1e-4) && approx(zAt(hb, 70, 40), -3, 1e-4), "base height added");
    }

    // ---- image heightmap gradient --------------------------------------
    {
        QImage img(64, 16, QImage::Format_RGB32);
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 64; ++x) {
                const int v = x * 255 / 63;
                img.setPixel(x, y, qRgb(v, v, v));
            }
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        img.save(&buf, "PNG");
        Model3D m;
        m.resolution = 0.5;
        ModelComponent c;
        c.id = Model3D::newId();
        c.kind = ModelComponent::ImageHeightmap;
        c.data = png;
        c.height = 10;
        c.x = 10; c.y = 10; c.width = 64;   // 1 mm per pixel, 64 x 16 mm
        m.components << c;
        const HeightModel hm = buildHeightModel(m, {}, 100, 80);
        check(hm.valid(), "image: built");
        check(approx(hm.baseZ, -10, 0.2), "image: base = -height");
        check(zAt(hm, 10.5, 18) < -9.5, "image: black edge at the floor");
        check(approx(zAt(hm, 42, 18), -5, 0.5), "image: mid-gray half way");
        check(zAt(hm, 73.5, 18) > -0.3, "image: white edge at the top");
        check(hm.at(1, 1) == HeightModel::NoModel && zAt(hm, 90, 18) == HeightModel::NoModel,
              "image: outside the picture is NoModel");
        // invert flips
        m.components[0].invert = true;
        const HeightModel hi = buildHeightModel(m, {}, 100, 80);
        check(zAt(hi, 10.5, 18) > -0.3 && zAt(hi, 73.5, 18) < -9.5, "image: invert");
        // grayField blur keeps the mean-ish, and row 0 is the bottom
        QImage two(4, 2, QImage::Format_RGB32);
        two.fill(Qt::black);
        for (int x = 0; x < 4; ++x) two.setPixel(x, 0, qRgb(255, 255, 255));   // top row white
        const GrayField g = grayField(two, false, 0);
        check(g.at(0, 1) == 1.0f && g.at(0, 0) == 0.0f, "grayField: row 0 = bottom");
        const GrayField gb = grayField(two, false, 1.0);
        check(gb.at(0, 0) > 0.05f && gb.at(0, 1) < 0.95f, "grayField: blur mixes rows");
    }

    // ---- texture over a region -----------------------------------------
    {
        QVector<Element> els;
        Element rect = Element::makeRectangle(QPointF(50, 40), 40, 40, layer);
        els << rect;
        QImage tile(2, 2, QImage::Format_RGB32);
        tile.fill(Qt::black);
        tile.setPixel(0, 0, qRgb(255, 255, 255));
        tile.setPixel(1, 1, qRgb(255, 255, 255));
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        tile.save(&buf, "PNG");
        Model3D m;
        m.resolution = 0.5;
        ModelComponent a;
        a.id = Model3D::newId(); a.shape = ModelComponent::Flat; a.height = 5; a.vectorIds << rect.id;
        ModelComponent t;
        t.id = Model3D::newId(); t.kind = ModelComponent::Texture; t.data = png;
        t.height = 2; t.tileWidth = 10; t.combine = ModelComponent::Add;
        m.components << a << t;
        const HeightModel hm = buildHeightModel(m, els, 100, 80);
        check(hm.valid() && approx(hm.baseZ, -7, 0.1), "texture: adds its amplitude on top");
        check(hm.at(1, 1) == HeightModel::NoModel, "texture: stays within the modelled region");
        double lo = 0, hi = -100;
        for (double x = 31; x < 69; x += 0.5) {
            const double z = zAt(hm, x, 40);
            lo = std::min(lo, z); hi = std::max(hi, z);
        }
        check(hi - lo > 1.0, "texture: tiles vary across the region");
    }

    // ---- STL pyramid -----------------------------------------------------
    {
        // square base 20 x 20 at z = 0, apex at (10, 10, 10)
        const QByteArray ascii =
            "solid pyramid\n"
            " facet normal 0 0 0\n  outer loop\n   vertex 0 0 0\n   vertex 20 0 0\n   vertex 10 10 10\n  endloop\n endfacet\n"
            " facet normal 0 0 0\n  outer loop\n   vertex 20 0 0\n   vertex 20 20 0\n   vertex 10 10 10\n  endloop\n endfacet\n"
            " facet normal 0 0 0\n  outer loop\n   vertex 20 20 0\n   vertex 0 20 0\n   vertex 10 10 10\n  endloop\n endfacet\n"
            " facet normal 0 0 0\n  outer loop\n   vertex 0 20 0\n   vertex 0 0 0\n   vertex 10 10 10\n  endloop\n endfacet\n"
            " facet normal 0 0 0\n  outer loop\n   vertex 0 0 0\n   vertex 20 20 0\n   vertex 20 0 0\n  endloop\n endfacet\n"
            " facet normal 0 0 0\n  outer loop\n   vertex 0 0 0\n   vertex 0 20 0\n   vertex 20 20 0\n  endloop\n endfacet\n"
            "endsolid pyramid\n";
        QVector<StlTriangle> tris;
        check(parseStl(ascii, &tris) && tris.size() == 6, "STL: ascii parse");
        // binary round trip of the same mesh
        QByteArray bin(80, '\0');
        quint32 n = 6;
        bin.append(reinterpret_cast<const char *>(&n), 4);
        for (const StlTriangle &t : tris) {
            float nrm[3] = {0, 0, 0};
            bin.append(reinterpret_cast<const char *>(nrm), 12);
            for (int v = 0; v < 3; ++v)
                bin.append(reinterpret_cast<const char *>(t.v[v]), 12);
            bin.append(2, '\0');
        }
        QVector<StlTriangle> trisB;
        check(parseStl(bin, &trisB) && trisB.size() == 6 && trisB.at(0).v[2][2] == 10.0f, "STL: binary parse");

        Model3D m;
        m.resolution = 0.25;
        ModelComponent c;
        c.id = Model3D::newId();
        c.kind = ModelComponent::StlMesh;
        c.data = bin;
        c.x = 30; c.y = 20;
        c.height = 0;   // natural height: 10 mm
        m.components << c;
        const HeightModel hm = buildHeightModel(m, {}, 100, 80);
        check(hm.valid(), "STL: built");
        check(approx(hm.baseZ, -10, 0.3), "STL: pyramid height 10");
        check(approx(zAt(hm, 40, 30), 0, 0.3), "STL: apex at 0");
        check(approx(zAt(hm, 35, 30), -5, 0.4), "STL: half way down the side");
        check(zAt(hm, 30.2, 20.2) < -9.5 && zAt(hm, 30.2, 20.2) != HeightModel::NoModel, "STL: corner at the floor");
        check(zAt(hm, 20, 30) == HeightModel::NoModel, "STL: outside the mesh is NoModel");
        // inch units scale the footprint 25.4x; fit-to-width overrides
        m.components[0].units = ModelComponent::Inches;
        m.components[0].height = 4;
        const HeightModel hin = buildHeightModel(m, {}, 1000, 1000);
        check(approx(hin.baseZ, -4, 0.2) && zAt(hin, 30 + 254 - 2, 20 + 254 - 2) != HeightModel::NoModel,
              "STL: inch units + explicit height");
        m.components[0].units = ModelComponent::Millimetres;
        m.components[0].width = 40;
        const HeightModel hw = buildHeightModel(m, {}, 100, 80);
        check(approx(zAt(hw, 50, 40), 0, 0.3) && zAt(hw, 69, 59) != HeightModel::NoModel,
              "STL: fit to width 40 -> 40 x 40 footprint");
    }

    // ---- auto resolution ------------------------------------------------
    {
        check(approx(modelCellSize(100, 100, 0), 0.1, 1e-9), "auto cell: small board at 0.1 mm");
        check(modelCellSize(825.5, 774.4, 0) > 0.39, "auto cell: big board coarsened to <= 4 M cells");
        check(approx(modelCellSize(100, 100, 0.05), 0.1, 1e-9), "cell never finer than 0.1");
        check(modelCellSize(2000, 2000, 0.1) > 0.99, "requested cell coarsened to the cap");
    }

    // ---- render ----------------------------------------------------------
    {
        QVector<Element> els;
        Element circle = Element::makeCircle(QPointF(50, 40), 20, layer);
        els << circle;
        Model3D m;
        m.resolution = 1;
        ModelComponent c;
        c.id = Model3D::newId(); c.height = 5; c.vectorIds << circle.id;
        m.components << c;
        const HeightModel hm = buildHeightModel(m, els, 100, 80);
        const QImage img = renderHeightModel(hm);
        check(img.width() == hm.cols && img.height() == hm.rows, "render: one pixel per cell");
        const QRgb top = img.pixel(50, hm.rows - 1 - 40);
        const QRgb out = img.pixel(2, 2);
        check(qGray(top) > qGray(out), "render: relief lighter than the background");
    }

    // ---- persistence round trip -----------------------------------------
    {
        QTemporaryDir dir;
        check(dir.isValid(), "temp dir");
        const QString path = dir.filePath(QStringLiteral("model.c2d"));
        {
            // a minimal container: just the sqlar table the model lives in
            const QString conn = QStringLiteral("mk");
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            db.setDatabaseName(path);
            check(db.open(), "create temp db");
            {
                QSqlQuery q(db);
                q.exec(QStringLiteral("CREATE TABLE sqlar(name TEXT PRIMARY KEY, mode INT, mtime INT, sz INT, data BLOB)"));
                q.exec(QStringLiteral("INSERT INTO sqlar VALUES('notes.txt', 33188, 0, 5, 'hello')"));
            }
            db.close();
            QSqlDatabase::removeDatabase(conn);
        }
        Model3D m;
        m.resolution = 0.3;
        ModelComponent a;
        a.id = Model3D::newId(); a.name = QStringLiteral("Dome"); a.shape = ModelComponent::Dome;
        a.height = 6.5; a.baseHeight = 1.25; a.angleDeg = 30; a.combine = ModelComponent::Merge;
        a.vectorIds << QStringLiteral("{aaa}") << QStringLiteral("{bbb}");
        ModelComponent b;
        b.id = Model3D::newId(); b.name = QStringLiteral("Pic"); b.kind = ModelComponent::ImageHeightmap;
        b.data = QByteArray("\x89PNG\0\0binary\xff", 14); b.sourceName = QStringLiteral("pic.png");
        b.invert = true; b.blurPx = 2.5; b.x = 12; b.y = 34; b.width = 56; b.enabled = false;
        b.combine = ModelComponent::Subtract;
        ModelComponent s;
        s.id = Model3D::newId(); s.kind = ModelComponent::StlMesh; s.units = ModelComponent::Inches;
        s.data = QByteArray(200, 'S'); s.tileWidth = 7;
        m.components << a << b << s;
        QString err;
        check(m.saveTo(path, &err), "save model");
        Model3D r;
        check(r.loadFrom(path, &err), "load model");
        check(r.components.size() == 3, "round trip: 3 components");
        check(approx(r.resolution, 0.3, 1e-12), "round trip: resolution");
        const ModelComponent &ra = r.components.at(0);
        check(ra.id == a.id && ra.name == a.name && ra.shape == ModelComponent::Dome
              && approx(ra.height, 6.5, 1e-12) && approx(ra.baseHeight, 1.25, 1e-12)
              && approx(ra.angleDeg, 30, 1e-12) && ra.combine == ModelComponent::Merge
              && ra.vectorIds == a.vectorIds && ra.data.isEmpty(), "round trip: vector component");
        const ModelComponent &rb = r.components.at(1);
        check(rb.kind == ModelComponent::ImageHeightmap && rb.data == b.data && rb.invert
              && approx(rb.blurPx, 2.5, 1e-12) && approx(rb.x, 12, 1e-12) && approx(rb.y, 34, 1e-12)
              && approx(rb.width, 56, 1e-12) && !rb.enabled && rb.combine == ModelComponent::Subtract
              && rb.sourceName == b.sourceName, "round trip: image component + blob");
        const ModelComponent &rs = r.components.at(2);
        check(rs.kind == ModelComponent::StlMesh && rs.units == ModelComponent::Inches
              && rs.data == s.data && approx(rs.tileWidth, 7, 1e-12), "round trip: STL component + blob");
        // re-saving with a component removed drops its blob; an empty model
        // leaves no rows; the unrelated sqlar row survives
        r.components.removeAt(1);
        check(r.saveTo(path, &err), "re-save");
        {
            const QString conn = QStringLiteral("chk");
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            db.setDatabaseName(path);
            check(db.open(), "reopen temp db");
            {
                QSqlQuery q(db);
                q.exec(QStringLiteral("SELECT count(*) FROM sqlar WHERE name LIKE 'phobi_model3d/%'"));
                check(q.next() && q.value(0).toInt() == 1, "re-save: orphan blob removed");
                q.exec(QStringLiteral("SELECT data FROM sqlar WHERE name='notes.txt'"));
                check(q.next() && q.value(0).toByteArray() == "hello", "other sqlar rows untouched");
            }
            db.close();
            QSqlDatabase::removeDatabase(conn);
        }
        Model3D empty;
        check(empty.saveTo(path, &err), "save empty");
        Model3D r2;
        check(r2.loadFrom(path, &err) && r2.components.isEmpty(), "empty model: no rows, loads clean");
    }

    // ---- provider ----------------------------------------------------------
    {
        installModelProvider();
        Document doc;   // no file: nothing to model
        check(heightModelFor(doc) == nullptr, "provider: empty document -> nullptr");
        ModelStore store;
        store.attach(&doc);
        ModelComponent c;
        c.id = Model3D::newId(); c.height = 3; c.shape = ModelComponent::Flat;
        store.model.components << c;
        store.invalidate();
        check(heightModelFor(doc) == nullptr, "provider: board 0 x 0 -> nullptr");
        check(ModelStore::forDocument(doc) == &store, "provider: attached store answers");
    }

    std::printf("test_model3d: %d checks OK\n", g_checks);
    return 0;
}
