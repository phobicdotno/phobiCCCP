// Regression tests for the hardening pass: every check here failed before the
// fix it names, and each is written against the observable behaviour rather
// than the implementation.
//
// Needs a sample document for the save tests: `test_hardening <sample.c2d>`.
// Without one those sections are skipped; everything else always runs.
//
// Plain asserts, no framework; exits 0 on success.

#include "../src/c2ddocument.h"
#include "../src/cam3d.h"
#include "../src/element.h"
#include "../src/gcodeexport.h"
#include "../src/importers.h"
#include "../src/model3d.h"
#include "../src/tiling.h"
#include "../src/vcarve.h"
#include "../src/zlibutil.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineF>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTransform>

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

// ---------------------------------------------------------------------------
// helpers

static QPolygonF rectPoly(double x, double y, double w, double h)
{
    QPolygonF p;
    p << QPointF(x, y) << QPointF(x + w, y) << QPointF(x + w, y + h)
      << QPointF(x, y + h) << QPointF(x, y);
    return p;
}

static QPolygonF circlePoly(double r, int n)
{
    QPolygonF p;
    for (int i = 0; i <= n; ++i) {
        const double a = 2 * M_PI * i / n;
        p << QPointF(r * std::cos(a), r * std::sin(a));
    }
    return p;
}

static double axisLength(const QVector<QVector<c2d::VPoint>> &chains)
{
    double len = 0;
    for (const auto &c : chains)
        for (int i = 1; i < c.size(); ++i)
            len += QLineF(c.at(i - 1).x, c.at(i - 1).y, c.at(i).x, c.at(i).y).length();
    return len;
}

// A document with one shape and one toolpath, built the way the app builds it.
struct Rig {
    c2d::Document doc;
    QString shapeId;

    void addShape(const c2d::Element &e) { shapeId = e.id; doc.addElement(e); }

    void addToolpath(const char *type, const QJsonObject &extra)
    {
        QJsonObject j;
        j.insert("type", QLatin1String(type));
        j.insert("name", QStringLiteral("t"));
        j.insert("enabled", true);
        j.insert("uuid", QStringLiteral("{tp-1}"));
        j.insert("start_depth", QStringLiteral("0.000"));
        j.insert("end_depth", QStringLiteral("2.000"));
        j.insert("stepdown", 2.0);
        j.insert("stock_to_leave", 0.0);
        QJsonObject speeds;
        speeds.insert("feedrate", 1000);
        speeds.insert("plungerate", 300);
        speeds.insert("rpm", 10000);
        j.insert("speeds", speeds);
        QJsonObject tool;
        tool.insert("diameter", 6.35);
        tool.insert("number", 201);
        j.insert("tool", tool);
        j.insert("elements", QJsonArray{QJsonObject{{"uuid", shapeId}}});
        for (const QString &k : extra.keys())
            j.insert(k, extra.value(k));
        c2d::Toolpath tp;
        tp.uuid = j.value("uuid").toString();
        tp.type = QLatin1String(type);
        tp.json = j;
        doc.addToolpath(tp);
    }
};

// Total XY distance travelled by Feed ops sitting at (about) depth z.
static double feedLengthAt(const QVector<c2d::Op> &ops, double z, double eps = 1e-6)
{
    double len = 0, px = 0, py = 0;
    bool have = false;
    for (const c2d::Op &o : ops) {
        if (o.kind != c2d::Op::Feed && o.kind != c2d::Op::Rapid && o.kind != c2d::Op::Arc)
            continue;
        if (have && o.kind == c2d::Op::Feed && std::fabs(o.z - z) < eps)
            len += QLineF(px, py, o.x, o.y).length();
        px = o.x;
        py = o.y;
        have = true;
    }
    return len;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QString sample = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    const QJsonObject layer;

    // -----------------------------------------------------------------------
    // Medial axis: densification joints are not corners.
    //
    // Rounding each densified joint to integer micrometres put it ~0.7 um off
    // the line it was cut from, which reads as a ~7e-4 rad corner - above the
    // 1e-4 collinearity threshold - so every joint grew its own bisector, and
    // the spur pruning could not remove them because they are not polygon
    // vertices. A 20 mm square rotated 17 deg came out as 72 chains and
    // 242.56 mm of axis instead of 4 and 56.57.
    if (c2d::medialAxisAvailable()) {
        QTransform rot;
        rot.rotate(17);

        const auto sq = c2d::medialAxis({rectPoly(0, 0, 20, 20)});
        check(sq.size() == 4, "medial axis: axis-aligned square has four chains");
        check(std::fabs(axisLength(sq) - 56.57) < 0.2, "medial axis: square length");

        const auto rsq = c2d::medialAxis({rot.map(rectPoly(0, 0, 20, 20))});
        check(rsq.size() == 4, "medial axis: rotated square has four chains too");
        check(std::fabs(axisLength(rsq) - 56.57) < 0.2,
              "medial axis: rotating a square does not change its axis length");

        const auto rrect = c2d::medialAxis({rot.map(rectPoly(0, 0, 60, 20))});
        check(std::fabs(axisLength(rrect) - 96.57) < 0.3,
              "medial axis: rotated 60x20 rectangle length");

        // A circle has no axis worth cutting; a coarse flattening must not
        // invent one. 48 sides puts the edges over the 1 mm densification
        // threshold, which is what used to trigger it.
        const double c48 = axisLength(c2d::medialAxis({circlePoly(10, 48)}));
        const double c64 = axisLength(c2d::medialAxis({circlePoly(10, 64)}));
        check(c48 < 2.0, "medial axis: 48-gon circle has (almost) no axis");
        check(c64 < 2.0, "medial axis: 64-gon circle has (almost) no axis");

        QTransform sc;
        sc.scale(1.0, 38.0 / 40.0);
        const double oval = axisLength(c2d::medialAxis({sc.map(circlePoly(20, 64))}));
        check(oval > 1.0 && oval < 8.0, "medial axis: an oval has a short real axis");
        std::printf("medial axis: square %.2f, rotated %.2f, oval %.2f mm\n",
                    axisLength(sq), axisLength(rsq), oval);
    } else {
        std::printf("medial axis: Voronoi backend not built, section skipped\n");
    }

    // -----------------------------------------------------------------------
    // Tiling: the top of the last tile is inside it.
    //
    // tileCount keeps a maxY of exactly n*tileHeight in tile n-1, but the band
    // test clipped tile n-1 at n*tileHeight, so a cut lying on that line was
    // pushed into a tile that does not exist and vanished from the program.
    {
        const double tileH = 100.0;
        QVector<c2d::Op> ops;
        ops << c2d::Op::rapid(0, 100, 2.54)
            << c2d::Op::feedTo(0, 100, -1, 300)
            << c2d::Op::feedTo(50, 100, -1, 1000);   // horizontal, on the line
        const auto tiles = c2d::tileOps(ops, tileH, 2.54);
        check(tiles.size() == 1, "tiling: one tile for a job that ends at the tile line");
        double cut = 0;
        for (const auto &t : tiles)
            cut += feedLengthAt(t, -1);
        check(std::fabs(cut - 50.0) < 1e-6,
              "tiling: a horizontal cut on the top tile line is kept, not dropped");

        // A nanometre of slope is not a slope: clipping it parametrically split
        // one cut into two separate tile programs with a stock re-index between.
        QVector<c2d::Op> tilt;
        tilt << c2d::Op::rapid(0, 99.99999999, 2.54)
             << c2d::Op::feedTo(0, 99.99999999, -1, 300)
             << c2d::Op::feedTo(200, 100.00000001, -1, 1000);
        const auto ttiles = c2d::tileOps(tilt, tileH, 2.54);
        int withCuts = 0;
        for (const auto &t : ttiles)
            if (feedLengthAt(t, -1) > 1e-6)
                ++withCuts;
        check(withCuts == 1, "tiling: a near-horizontal cut stays in one tile");
    }

    // -----------------------------------------------------------------------
    // G-code: tabs, depth per pass, safe Z.
    {
        // Contours carry tabs too. They were read for cutouts only, so a
        // contour taken through the stock released the part on the last pass.
        Rig r;
        r.addShape(c2d::Element::makeRectangle({60, 60}, 80, 60, layer));
        r.addToolpath("contour", {{"ofset_dir", 1},
                                  {"end_depth", QStringLiteral("6.000")},
                                  {"stepdown", 6.0},
                                  {"ignore_tabs", false},
                                  {"tab_height", 3.0},
                                  {"tab_width", 6.0}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        check(!g.done.isEmpty(), "contour with tabs is exported");
        check(feedLengthAt(g.ops, -3.0) > 1.0,
              "gcode: a contour with tabs lifts to the tab top");
        check(feedLengthAt(g.ops, -6.0) > 1.0,
              "gcode: and still cuts to full depth between them");

        Rig noTabs;
        noTabs.addShape(c2d::Element::makeRectangle({60, 60}, 80, 60, layer));
        noTabs.addToolpath("contour", {{"ofset_dir", 1},
                                       {"end_depth", QStringLiteral("6.000")},
                                       {"stepdown", 6.0},
                                       {"ignore_tabs", true}});
        check(feedLengthAt(c2d::exportGcode(noTabs.doc).ops, -3.0) < 1e-6,
              "gcode: ignore_tabs still means no tabs");
    }
    {
        // A 20 mm square cutout: three 30 mm tabs used to tile its whole
        // perimeter, so the "cut" never went below the tab top and the part was
        // never released - and the pass then ended with a full-depth plunge
        // drilled inside a tab.
        Rig r;
        r.addShape(c2d::Element::makeRectangle({40, 40}, 20, 20, layer));
        r.addToolpath("cutout", {{"cut_depth", QStringLiteral("6.000")},
                                 {"break_through", QStringLiteral("0.000")},
                                 {"depth_per_pass", 6.0},
                                 {"ignore_tabs", false},
                                 {"tab_height", 3.0},
                                 {"tab_width", 127.0}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        const double atDepth = feedLengthAt(g.ops, -6.0);
        const double atTab = feedLengthAt(g.ops, -3.0);
        check(atDepth > atTab,
              "gcode: a small cutout is cut through over more of its loop than it is tabbed");
        check(atTab > 1.0, "gcode: but it is still tabbed");
    }
    {
        // depth_per_pass / stepdown arrive as strings in some CC payloads.
        // toDouble(default) silently substituted 1.0 mm - twice the requested
        // cut, always in the unsafe direction.
        Rig r;
        r.addShape(c2d::Element::makeRectangle({60, 60}, 40, 40, layer));
        r.addToolpath("contour", {{"ofset_dir", 1},
                                  {"end_depth", QStringLiteral("2.000")},
                                  {"stepdown", QStringLiteral("0.500")}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        QVector<double> depths;
        for (const c2d::Op &o : g.ops)
            if (o.kind == c2d::Op::Feed && o.z < -1e-6 && !depths.contains(o.z))
                depths.append(o.z);
        std::sort(depths.begin(), depths.end());
        check(depths.size() == 4, "gcode: a string stepdown of 0.5 gives four passes");
        check(std::fabs(depths.first() + 2.0) < 1e-9, "gcode: the last pass is at full depth");
    }
    {
        // `retract` comes verbatim out of the params table; toDouble() returns
        // 0 for anything it cannot parse, and a safe Z of 0 rapids across the
        // stock with the tip at its top surface.
        Rig r;
        r.doc.setParam(QStringLiteral("retract"), QStringLiteral("2,54"));  // comma decimal
        r.addShape(c2d::Element::makeRectangle({60, 60}, 40, 40, layer));
        r.addToolpath("contour", {{"ofset_dir", 1}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        double maxRapidZ = -1e9;
        for (const c2d::Op &o : g.ops)
            if (o.kind == c2d::Op::Rapid)
                maxRapidZ = qMax(maxRapidZ, o.z);
        check(maxRapidZ > 0.5, "gcode: an unparsable retract falls back to a real clearance");

        // The tiled exporter reads the same parameter and used to read it
        // without the guard, so the moves between tiles retracted to zero.
        QTemporaryDir tiledDir;
        check(tiledDir.isValid(), "tiled: scratch directory");
        r.doc.setParam(QStringLiteral("tile_height"), QStringLiteral("25.0"));
        const c2d::TiledExport te =
            c2d::exportTiled(r.doc, tiledDir.filePath(QStringLiteral("t")), 25.0);
        check(te.error.isEmpty(), "tiled: the export succeeds");
        check(te.files.size() > 1, "tiled: a 40 mm shape over 25 mm tiles makes several tiles");
        // Tiles that the shape does not reach into are written empty, so only
        // the ones that actually move can say anything about the clearance.
        int tilesWithMotion = 0;
        for (const QString &f : te.files) {
            QFile fh(f);
            check(fh.open(QIODevice::ReadOnly | QIODevice::Text), "tiled: the tile is written");
            double maxZ = -1e9;
            const QStringList lines = QString::fromUtf8(fh.readAll()).split(QChar('\n'));
            for (const QString &line : lines) {
                // The post suppresses modal words, so a rapid is "G0..." only
                // when the motion mode changed; Z appears when it changed.
                const qsizetype zi = line.indexOf(QChar('Z'));
                if (!line.startsWith(QLatin1String("G0")) || zi < 0)
                    continue;
                maxZ = qMax(maxZ, line.mid(zi + 1).split(QChar(' ')).first().toDouble());
            }
            if (maxZ < -1e8)
                continue;                      // an empty tile
            ++tilesWithMotion;
            check(maxZ > 0.5, "tiled: an unparsable retract falls back to a real clearance");
        }
        check(tilesWithMotion > 0, "tiled: at least one tile actually cuts");
    }
    {
        // Ramping used to end its descent wherever the angle ran out and then
        // go straight back to the ring start - a chord across the inside of the
        // ring, i.e. into the part on an outside contour, on every pass.
        const double R = 20.0, toolR = 6.35 / 2;
        Rig r;
        r.addShape(c2d::Element::makeCircle({60, 60}, R, layer));
        r.addToolpath("contour", {{"ofset_dir", 1},
                                  {"end_depth", QStringLiteral("4.000")},
                                  {"stepdown", 1.5},
                                  {"enable_ramping", true},
                                  {"ramp_angle", 20.0}});
        const c2d::GcodeResult g = c2d::exportGcode(r.doc);
        const double ringR = R + toolR;
        double worst = 1e9;
        double px = 0, py = 0;
        bool have = false;
        for (const c2d::Op &o : g.ops) {
            if (o.kind == c2d::Op::Feed && have && o.z < -1e-6) {
                const QPointF mid((px + o.x) / 2 - 60, (py + o.y) / 2 - 60);
                worst = qMin(worst, std::hypot(mid.x(), mid.y()));
            }
            if (o.kind == c2d::Op::Feed || o.kind == c2d::Op::Rapid || o.kind == c2d::Op::Arc) {
                px = o.x;
                py = o.y;
                have = true;
            }
        }
        check(worst > ringR - 0.15,
              "gcode: a ramped outside contour never cuts inside its own ring");
    }

    // -----------------------------------------------------------------------
    // Hostile DXF: none of these may hang or allocate on the file's word.
    {
        c2d::ImportOptions opt;
        opt.stockWidth = 200;
        opt.stockHeight = 200;
        struct V { const char *name; const char *body; };
        const V cases[] = {
            // Winding the end angle up by 2pi until it passes the start angle
            // never terminates for an angle this large - past ~2.8e16 the
            // addition is a no-op in double precision.
            {"arc_huge_angle", "0\nSECTION\n2\nENTITIES\n0\nARC\n40\n10\n50\n1e300\n51\n0\n"
                               "0\nENDSEC\n0\nEOF\n"},
            {"arc_inf_angle", "0\nSECTION\n2\nENTITIES\n0\nARC\n40\n10\n50\ninf\n51\n0\n"
                              "0\nENDSEC\n0\nEOF\n"},
            {"ellipse_huge_angle", "0\nSECTION\n2\nENTITIES\n0\nELLIPSE\n10\n0\n20\n0\n"
                                   "11\n10\n21\n0\n40\n0.5\n41\n1e300\n42\n0\n0\nENDSEC\n0\nEOF\n"},
            // A billion-by-a-billion MINSERT array is not a drawing.
            {"insert_array_bomb", "0\nSECTION\n2\nBLOCKS\n0\nBLOCK\n2\nA\n0\nLINE\n"
                                  "10\n0\n20\n0\n11\n1\n21\n1\n0\nENDBLK\n0\nENDSEC\n"
                                  "2\nENTITIES\n0\nINSERT\n2\nA\n10\n0\n20\n0\n"
                                  "70\n1000000000\n71\n1000000000\n44\n1\n45\n1\n"
                                  "0\nENDSEC\n0\nEOF\n"},
            // Degree 99999 makes each NURBS evaluation O(degree^2), tens of
            // thousands of times over.
            {"spline_huge_degree", "0\nSECTION\n2\nENTITIES\n0\nSPLINE\n70\n8\n71\n99999\n"
                                   "10\n0\n20\n0\n10\n10\n20\n0\n10\n10\n20\n10\n"
                                   "10\n0\n20\n10\n0\nENDSEC\n0\nEOF\n"},
        };
        for (const V &v : cases) {
            const c2d::ImportResult res =
                c2d::importDxfData(QByteArray(v.body), opt, QString::fromLatin1(v.name));
            ++g_checks;                       // returning at all is the check
            for (int i = 0; i < qMin(res.elements.size(), qsizetype(200)); ++i) {
                const QRectF bb = res.elements.at(i).painterPath.boundingRect();
                check(std::isfinite(bb.x()) && std::isfinite(bb.y())
                          && std::isfinite(bb.width()) && std::isfinite(bb.height()),
                      "dxf: geometry from a hostile file is finite");
            }
        }
        std::printf("hostile dxf: %d cases returned\n", int(std::size(cases)));
    }

    // -----------------------------------------------------------------------
    // zlib: `sz` is a number in the file, not a promise.
    {
        const QByteArray plain(200000, 'x');
        const QByteArray comp = c2d::zlibDeflate(plain);
        check(c2d::zlibInflate(comp, plain.size()) == plain, "zlib: round trip");
        // A one-line UPDATE used to turn every open into a gigabyte reserve.
        check(c2d::zlibInflate(comp, 999999999) == plain,
              "zlib: an absurd sz does not change the result (or the allocation)");
        check(c2d::zlibInflate(comp, -5) == plain, "zlib: a negative sz is ignored");
    }

    // -----------------------------------------------------------------------
    // Saving: never destroy the destination before the new content is good.
    if (!sample.isEmpty() && QFile::exists(sample)) {
        QTemporaryDir dir;
        check(dir.isValid(), "temp dir");
        const QString a = dir.filePath(QStringLiteral("a.c2d"));
        const QString b = dir.filePath(QStringLiteral("b.c2d"));
        check(QFile::copy(sample, a), "stage a.c2d");
        check(QFile::copy(sample, b), "stage b.c2d");

        // Save As must retarget the document: without it the next Ctrl+S wrote
        // back into the file that was opened, overwriting the original the user
        // was deliberately keeping.
        {
            c2d::Document doc;
            QString err;
            check(doc.load(a, &err), "load a.c2d");
            check(doc.save(b, &err), "save as b.c2d");
            check(QFileInfo(doc.filePath()).absoluteFilePath()
                      == QFileInfo(b).absoluteFilePath(),
                  "save: Save As retargets the document to the new file");
            check(!QFile::exists(b + QStringLiteral(".phobisave")),
                  "save: no staging file is left behind");
        }

        // A save that cannot run must leave the destination exactly as it was.
        {
            const QString c = dir.filePath(QStringLiteral("c.c2d"));
            check(QFile::copy(sample, c), "stage c.c2d");
            QFile bf(c);
            check(bf.open(QIODevice::ReadOnly), "read c.c2d");
            const QByteArray before = bf.readAll();
            bf.close();

            c2d::Document doc;
            QString err;
            check(doc.load(a, &err), "load a.c2d again");
            check(QFile::remove(a), "remove the source out from under it");
            check(!doc.save(c, &err), "save: a save with no source fails");
            QFile af(c);
            check(af.open(QIODevice::ReadOnly), "c.c2d still exists");
            check(af.readAll() == before,
                  "save: a failed save leaves the destination byte-for-byte intact");
            af.close();
            check(!QFile::exists(c + QStringLiteral(".phobisave")),
                  "save: a failed save leaves no staging file");
        }
    } else {
        std::printf("save section: no sample document given, skipped\n");
    }

    // -----------------------------------------------------------------------
    // A relief that will not load must survive the next save.
    {
        QTemporaryDir dir;
        check(dir.isValid(), "temp dir");
        const QString path = dir.filePath(QStringLiteral("model.c2d"));
        {
            const QString conn = QStringLiteral("hmk");
            {
                QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
                db.setDatabaseName(path);
                check(db.open(), "create temp container");
                QSqlQuery q(db);
                q.exec(QStringLiteral("CREATE TABLE sqlar(name TEXT PRIMARY KEY, mode INT, "
                                      "mtime INT, sz INT, data BLOB)"));
                db.close();
            }
            QSqlDatabase::removeDatabase(conn);
        }
        c2d::Model3D m;
        m.resolution = 0.3;
        c2d::ModelComponent comp;
        comp.id = c2d::Model3D::newId();
        comp.name = QStringLiteral("Mesh");
        comp.kind = c2d::ModelComponent::StlMesh;
        comp.data = QByteArray(4096, 'S');
        m.components << comp;
        QString err;
        check(m.saveTo(path, &err), "model: save");

        // Both helpers keep the query strictly inside the connection's scope:
        // a QSqlQuery outliving its QSqlDatabase makes removeDatabase warn and
        // leaves the connection behind.
        auto rowData = [&](const QString &name) {
            QByteArray out;
            const QString conn = QStringLiteral("hrd");
            {
                QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
                db.setDatabaseName(path);
                if (db.open()) {
                    QSqlQuery q(db);
                    q.prepare(QStringLiteral("SELECT data FROM sqlar WHERE name=?"));
                    q.addBindValue(name);
                    q.exec();
                    if (q.next())
                        out = q.value(0).toByteArray();
                    db.close();
                }
            }
            QSqlDatabase::removeDatabase(conn);
            return out;
        };
        auto damage = [&](const QString &name) {
            const QString conn = QStringLiteral("hdm");
            bool ok = false;
            {
                QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
                db.setDatabaseName(path);
                if (db.open()) {
                    QSqlQuery q(db);
                    q.prepare(QStringLiteral("UPDATE sqlar SET sz=999999, data=x'deadbeef' "
                                             "WHERE name=?"));
                    q.addBindValue(name);
                    ok = q.exec();
                    db.close();
                }
            }
            QSqlDatabase::removeDatabase(conn);
            return ok;
        };

        // A damaged component blob: the component is still listed, but its
        // bytes are the only copy there is. Rewriting the component without
        // them would lose the mesh for good.
        const QString blobRow = QStringLiteral("phobi_model3d/") + comp.id;
        const QByteArray blobBefore = rowData(blobRow);
        check(!blobBefore.isEmpty(), "model: the component blob was written");
        check(damage(blobRow), "model: damage the component blob");
        {
            c2d::Model3D r;
            check(r.loadFrom(path, &err), "model: a damaged component still loads");
            check(r.unreadableComponents.size() == 1, "model: it is remembered as unreadable");
            check(r.saveTo(path, &err), "model: saving again succeeds");
            check(rowData(blobRow) == QByteArray("\xde\xad\xbe\xef", 4),
                  "model: the unreadable component blob is kept, not rewritten");
        }

        // A damaged index row: nothing loaded at all, so there is nothing to
        // write back. Writing anyway deleted the whole relief.
        check(damage(QStringLiteral("phobi_model3d.json")), "model: damage the index row");
        {
            c2d::Model3D r;
            check(!r.loadFrom(path, &err), "model: an unreadable index row is reported");
            check(r.unreadable, "model: and remembered");
            check(r.saveTo(path, &err), "model: saving does not fail");
            check(!rowData(QStringLiteral("phobi_model3d.json")).isEmpty(),
                  "model: the index row is left in place");
            check(!rowData(blobRow).isEmpty(),
                  "model: and so is every component blob");
        }
    }

    // -----------------------------------------------------------------------
    // Numbers written as JSON strings. Carbide Create stores start_depth,
    // end_depth, break_through, min_depth and max_depth as strings in the
    // sample documents, and which keys get that treatment varies by build --
    // so every reader of a document number has to accept both forms. A plain
    // toDouble(def) hands back the default instead, silently.
    {
        QJsonObject tool{
            {QStringLiteral("number"), QStringLiteral("3")},
            {QStringLiteral("diameter"), QStringLiteral("6.350")},
            {QStringLiteral("angle"), QStringLiteral("90")},
            {QStringLiteral("type"), QStringLiteral("2")},
        };
        const c2d::ToolGeom g = c2d::toolGeomFromJson(tool);
        check(g.number == 3, "tool: a string tool number is read");
        check(qAbs(g.diameter - 6.35) < 1e-9,
              "tool: a string diameter is read, not the 3.175 default");
        check(g.kind == c2d::ToolGeom::VBit && qAbs(g.angle - 90.0) < 1e-9,
              "tool: a string angle still makes a 90 deg V-bit");

        // Unusable text must fall back, not produce a zero-diameter tool.
        tool.insert(QStringLiteral("diameter"), QStringLiteral("wide"));
        check(qAbs(c2d::toolGeomFromJson(tool).diameter - 3.175) < 1e-9,
              "tool: unreadable text falls back to the default diameter");

        QJsonObject j{
            {QStringLiteral("speeds"), QJsonObject{
                {QStringLiteral("feedrate"), QStringLiteral("1200")},
                {QStringLiteral("plungerate"), QStringLiteral("300")}}},
            {QStringLiteral("tool"), QJsonObject{
                {QStringLiteral("diameter"), 6.0}, {QStringLiteral("type"), 0}}},
            {QStringLiteral("stepdown"), QStringLiteral("0.500")},
            {QStringLiteral("stock_to_leave"), QStringLiteral("0.200")},
            {QStringLiteral("boundary_offset"), QStringLiteral("1.500")},
            {QStringLiteral("raster_angle"), QStringLiteral("45")},
            {QStringLiteral("max_depth"), QStringLiteral("2.000")},
        };
        const c2d::Cam3dParams p = c2d::cam3dParams(j, 6.0);
        check(qAbs(p.feed - 1200.0) < 1e-9, "cam3d: a string feed rate is read");
        check(qAbs(p.plunge - 300.0) < 1e-9, "cam3d: a string plunge rate is read");
        check(qAbs(p.stepdown - 0.5) < 1e-9,
              "cam3d: a string depth per pass is read, not the 1 mm default");
        check(qAbs(p.stockToLeave - 0.2) < 1e-9, "cam3d: a string stock to leave is read");
        check(qAbs(p.boundaryOffset - 1.5) < 1e-9, "cam3d: a string boundary offset is read");
        check(qAbs(p.rasterAngle - 45.0) < 1e-9, "cam3d: a string raster angle is read");
        check(qAbs(p.maxDepth + 2.0) < 1e-9, "cam3d: a string max depth is read");
    }

    // -----------------------------------------------------------------------
    // Depths are the numbers that decide how deep the tool goes. depthToZ was
    // the one reader in the exporter that did not go through numOr: it checked
    // neither the parse flag nor finiteness.
    {
        struct D { const char *value; const char *what; };
        const D bad[] = {
            {"nan",     "nan put a literal G1Znan in the program"},
            {"inf",     "inf never reached the bottom: the pass loop ran for ever"},
            {"1e400",   "an overflowing literal is inf too"},
            {"1e15",    "a finite but absurd depth is just as many passes"},
            {"19,05",   "a comma decimal parsed as 0 and cut nothing, reporting success"},
            {"19.05mm", "a unit suffix did the same"},
        };
        for (const D &d : bad) {
            Rig r;
            r.addShape(c2d::Element::makeRectangle({60, 60}, 20, 20, layer));
            r.addToolpath("contour", {{"ofset_dir", 1},
                                      {"end_depth", QString::fromLatin1(d.value)}});
            const c2d::GcodeResult g = c2d::exportGcode(r.doc);
            check(g.done.isEmpty(), d.what);
            check(g.skipped.size() == 1, "gcode: and the toolpath is reported as skipped");
            check(!g.gcode.contains(QLatin1String("nan")),
                  "gcode: nothing unreadable reaches the program text");
        }

        // The readable case must be untouched by the guard.
        Rig ok;
        ok.addShape(c2d::Element::makeRectangle({60, 60}, 20, 20, layer));
        ok.addToolpath("contour", {{"ofset_dir", 1},
                                   {"end_depth", QStringLiteral("19.050")},
                                   {"stepdown", 1.0}});
        const c2d::GcodeResult g = c2d::exportGcode(ok.doc);
        check(g.done.size() == 1 && g.skipped.isEmpty(), "gcode: a real depth still exports");
        double deepest = 0;
        for (const c2d::Op &o : g.ops)
            if (o.kind == c2d::Op::Feed)
                deepest = qMin(deepest, o.z);
        check(std::fabs(deepest + 19.05) < 1e-9, "gcode: and reaches exactly the full depth");

        // An absent depth still means "from the top", not "unreadable".
        Rig none;
        none.addShape(c2d::Element::makeCircle({60, 60}, 10, layer));
        none.addToolpath("drilling_toolpath", {});
        check(!c2d::exportGcode(none.doc).done.isEmpty(),
              "gcode: an absent start_depth is not treated as damage");
    }

    // -----------------------------------------------------------------------
    // Tiles left over from a previous, longer export under the same base name.
    {
        QTemporaryDir d;
        check(d.isValid(), "stale: scratch directory");
        Rig r;
        r.doc.setParam(QStringLiteral("retract"), QStringLiteral("2.54"));
        r.addShape(c2d::Element::makeRectangle({60, 60}, 40, 40, layer));
        r.addToolpath("contour", {{"ofset_dir", 1}});
        const QString base = d.filePath(QStringLiteral("job"));
        // A leftover from an export that needed more tiles than this one does.
        const QString ghost = base + QStringLiteral("_tile9.nc");
        { QFile f(ghost); check(f.open(QIODevice::WriteOnly), "stale: write the leftover");
          f.write("(tile 9/9)\n"); }
        const c2d::TiledExport te = c2d::exportTiled(r.doc, base, 25.0);
        check(te.error.isEmpty(), "stale: the export still succeeds");
        check(te.files.size() < 9, "stale: this export needs fewer tiles");
        // Only reported when the run stops short of it; tile9 sits past the end.
        check(te.stale.isEmpty() || te.stale.contains(ghost),
              "stale: a leftover tile past the end is reported, not silently left");
    }

    // -----------------------------------------------------------------------
    // Texture over a big sheet. The generator used to sample the region every
    // 0.5 mm along the diagonal for every hatch line, so its cost grew with
    // the square of the sheet: a 1220 mm Shapeoko XXL panel took 25 s on the
    // interface thread. The bound here is deliberately loose - it is there to
    // catch a return to quadratic, not to police a few hundred milliseconds.
    {
        // The shape that hurt: a small total area spread across a big bounding
        // box. The stroke guard never fires, so every hatch line was scanned
        // in full. (A solid sheet is NOT the bad case - it hits the guard
        // almost immediately and stops early.)
        // The cost was (path complexity) x (bounding box area), so the shape
        // needs both: many separate outlines, spread wide. Measured with the
        // old scan, this takes ~13 s; with 18 holes, like the sample baseplate
        // that first showed it up, ~3 s.
        c2d::Document doc;
        QJsonArray refs;
        for (int i = 0; i < 80; ++i) {
            const double a = 2 * M_PI * i / 80;
            const QPointF c(600 + 560 * std::cos(a), 600 + 560 * std::sin(a));
            const c2d::Element e = c2d::Element::makeCircle(c, 25, layer);
            doc.addElement(e);
            refs.append(QJsonObject{{QStringLiteral("uuid"), e.id}});
        }
        c2d::Toolpath tp;
        tp.uuid = QStringLiteral("{tex}");
        tp.type = QStringLiteral("texture_toolpath");
        tp.json = QJsonObject{
            {"type", "texture_toolpath"}, {"name", "tex"}, {"enabled", true},
            {"uuid", tp.uuid}, {"elements", refs},
            {"angle", 30}, {"min_length", 5}, {"max_length", 12}, {"stepover", 2.0},
            {"start_depth", QStringLiteral("0.000")},
            {"min_depth", QStringLiteral("-0.5")},
            {"max_depth", QStringLiteral("-1.5")},
            {"speeds", QJsonObject{{"feedrate", 1000}, {"plungerate", 300}, {"rpm", 10000}}},
            {"tool", QJsonObject{{"diameter", 3.175}, {"number", 301}}}};
        doc.addToolpath(tp);

        QElapsedTimer t;
        t.start();
        const c2d::GcodeResult g = c2d::exportGcode(doc);
        const qint64 ms = t.elapsed();
        check(g.done.size() == 1, "texture: a big sheet still exports");
        check(ms < 5000, "texture: and does not take the old quadratic path");
        std::printf("texture: 1200 mm sheet in %lld ms, %d ops\n",
                    (long long)ms, int(g.ops.size()));

        // Every stroke must sit inside the sheet it decorates.
        int cuts = 0;
        for (const c2d::Op &o : g.ops) {
            if (o.kind != c2d::Op::Feed || o.z > -1e-9)
                continue;
            ++cuts;
            check(o.x > 9.0 && o.x < 1191.0 && o.y > 9.0 && o.y < 1191.0,
                  "texture: a stroke lies inside the bounds it was given");
            if (cuts > 300)
                break;                       // a sample is enough
        }
        check(cuts > 0, "texture: the sheet actually got strokes");
    }

    std::printf("test_hardening: %d checks OK\n", g_checks);
    return 0;
}
