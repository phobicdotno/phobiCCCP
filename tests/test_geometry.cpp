// Headless checks for the geometry-editing layer: Element translate/scale/
// factories, and (when a sample .c2d path is passed as argv[1]) a full
// load -> edit -> save -> reload round trip through Document.
//
// Plain asserts, no test framework; exits 0 on success.

#include "../src/c2ddocument.h"
#include "../src/cam.h"
#include "../src/element.h"
#include "../src/grblsender.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>
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

static bool approx(double a, double b, double eps = 1e-6)
{
    return std::fabs(a - b) < eps;
}

static c2d::Element samplePath()
{
    // A minimal absolute-coordinate path: triangle (10,10) (30,10) (20,25).
    QJsonObject o;
    o.insert("behavior", 0);
    o.insert("geometryType", QStringLiteral("path"));
    o.insert("id", QStringLiteral("{test-path}"));
    o.insert("position", QJsonArray{0, 0});
    o.insert("points", QJsonArray{QJsonArray{10, 10}, QJsonArray{30, 10},
                                  QJsonArray{20, 25}, QJsonArray{0, 0}});
    o.insert("cp1", QJsonArray{QJsonArray{10, 10}, QJsonArray{10, 10},
                               QJsonArray{30, 10}, QJsonArray{20, 25}});
    o.insert("cp2", QJsonArray{QJsonArray{10, 10}, QJsonArray{30, 10},
                               QJsonArray{20, 25}, QJsonArray{10, 10}});
    o.insert("point_type", QJsonArray{0, 1, 1, 4});
    return c2d::Element::fromJson(o);
}

// A minimal toolpath of the given type with sane feeds and a 6.35 mm tool,
// depths 0 -> 2 mm in one pass. extra overrides/adds keys.
static c2d::Toolpath makeTp(const char *type, const QJsonObject &extra = {})
{
    QJsonObject j;
    j.insert("type", QLatin1String(type));
    j.insert("name", QStringLiteral("test"));
    j.insert("enabled", true);
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
    for (const QString &k : extra.keys())
        j.insert(k, extra.value(k));
    c2d::Toolpath tp;
    tp.type = QLatin1String(type);
    tp.json = j;
    return tp;
}

// Bounding box over the XY of all cutting (Feed) moves. Tracked by hand:
// QRectF::united() treats zero-size rectangles as null and skips them.
static QRectF feedBounds(const QVector<c2d::Op> &ops, double *minZ = nullptr)
{
    double minX = 1e30, maxX = -1e30, minY = 1e30, maxY = -1e30;
    bool any = false;
    if (minZ)
        *minZ = 0;
    for (const c2d::Op &op : ops) {
        if (op.kind != c2d::Op::Feed)
            continue;
        any = true;
        minX = std::min(minX, op.x);
        maxX = std::max(maxX, op.x);
        minY = std::min(minY, op.y);
        maxY = std::max(maxY, op.y);
        if (minZ && op.z < *minZ)
            *minZ = op.z;
    }
    return any ? QRectF(QPointF(minX, minY), QPointF(maxX, maxY)) : QRectF();
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // --- makeCircle -------------------------------------------------------
    {
        c2d::Element e = c2d::Element::makeCircle(QPointF(50, 40), 6);
        const QRectF bb = e.painterPath.boundingRect();
        check(approx(bb.center().x(), 50, 1e-3) && approx(bb.center().y(), 40, 1e-3),
              "circle centered");
        check(approx(bb.width(), 12, 1e-2) && approx(bb.height(), 12, 1e-2),
              "circle diameter");
        check(e.raw.value("radius").toDouble() == 6, "circle radius field");
        check(e.raw.value("id").toString().startsWith(QLatin1Char('{')),
              "circle id braces");
        check(e.raw.value("layer").toObject().value("name").toString()
                  == QLatin1String("DEFAULT"), "circle layer block");
    }

    // --- makeRectangle ----------------------------------------------------
    {
        c2d::Element e = c2d::Element::makeRectangle(QPointF(30, 20), 90, 84);
        const QRectF bb = e.painterPath.boundingRect();
        check(approx(bb.width(), 90) && approx(bb.height(), 84), "rect size");
        check(approx(bb.center().x(), 30) && approx(bb.center().y(), 20), "rect centered");
        check(e.raw.value("width").toDouble() == 90 &&
              e.raw.value("height").toDouble() == 84, "rect w/h fields");
    }

    // --- translate: closed shape moves center/position only ---------------
    {
        c2d::Element e = c2d::Element::makeCircle(QPointF(10, 10), 5);
        const QJsonArray before = e.raw.value("points").toArray();
        e.translate(7, -3);
        const QJsonArray center = e.raw.value("center").toArray();
        check(approx(center.at(0).toDouble(), 17) && approx(center.at(1).toDouble(), 7),
              "translate circle center");
        check(e.raw.value("points").toArray() == before,
              "translate circle keeps relative points");
        const QRectF bb = e.painterPath.boundingRect();
        check(approx(bb.center().x(), 17, 1e-3) && approx(bb.center().y(), 7, 1e-3),
              "translate circle path rebuilt");
    }

    // --- translate: path moves absolute points, position stays [0,0] ------
    {
        c2d::Element e = samplePath();
        e.translate(5, 5);
        const QJsonArray pos = e.raw.value("position").toArray();
        check(pos.at(0).toDouble() == 0 && pos.at(1).toDouble() == 0,
              "translate path keeps position 0,0");
        const QJsonArray pts = e.raw.value("points").toArray();
        check(approx(pts.at(0).toArray().at(0).toDouble(), 15) &&
              approx(pts.at(0).toArray().at(1).toDouble(), 15),
              "translate path shifts points");
        const QRectF bb = e.painterPath.boundingRect();
        check(approx(bb.left(), 15) && approx(bb.bottom(), 30), "translate path bbox");
    }

    // --- scaleBy: rectangle scales fields and model about its center ------
    {
        c2d::Element e = c2d::Element::makeRectangle(QPointF(30, 20), 40, 20);
        e.scaleBy(2.0);
        check(approx(e.raw.value("width").toDouble(), 80) &&
              approx(e.raw.value("height").toDouble(), 40), "scale rect w/h fields");
        const QRectF bb = e.painterPath.boundingRect();
        check(approx(bb.width(), 80) && approx(bb.height(), 40), "scale rect bbox");
        check(approx(bb.center().x(), 30) && approx(bb.center().y(), 20),
              "scale rect keeps center");
    }

    // --- scaleBy: path scales about its bounding-box center ---------------
    {
        c2d::Element e = samplePath();
        const QPointF c0 = e.painterPath.boundingRect().center();
        e.scaleBy(3.0);
        const QRectF bb = e.painterPath.boundingRect();
        check(approx(bb.width(), 60) && approx(bb.height(), 45), "scale path bbox");
        check(approx(bb.center().x(), c0.x()) && approx(bb.center().y(), c0.y()),
              "scale path keeps center");
    }

    // --- CAM: contour outside offsets by the tool radius -------------------
    {
        const c2d::Element rect = c2d::Element::makeRectangle(QPointF(30, 20), 40, 20);
        const c2d::Toolpath tp = makeTp("contour", {{"ofset_dir", 1}});
        QStringList notes;
        const QVector<c2d::Op> ops =
            c2d::Cam::toolpathOps(tp, {rect.painterPath}, &notes);
        check(!ops.isEmpty() && notes.isEmpty(), "contour produces ops");
        double minZ = 0;
        const QRectF bb = feedBounds(ops, &minZ);
        // 40x20 rect at (30,20) offset outward by r=3.175
        check(approx(bb.left(), 10 - 3.175, 0.05) &&
              approx(bb.right(), 50 + 3.175, 0.05) &&
              approx(bb.top(), 10 - 3.175, 0.05) &&
              approx(bb.bottom(), 30 + 3.175, 0.05),
              "contour outside bbox");
        check(approx(minZ, -2.0), "contour cuts to end depth");
    }

    // --- CAM: contour inside shrinks by the tool radius --------------------
    {
        const c2d::Element rect = c2d::Element::makeRectangle(QPointF(30, 20), 40, 20);
        const c2d::Toolpath tp = makeTp("contour", {{"ofset_dir", -1}});
        const QVector<c2d::Op> ops =
            c2d::Cam::toolpathOps(tp, {rect.painterPath}, nullptr);
        const QRectF bb = feedBounds(ops);
        check(approx(bb.left(), 10 + 3.175, 0.05) &&
              approx(bb.right(), 50 - 3.175, 0.05),
              "contour inside bbox");
    }

    // --- CAM: pocket stays inside the shrunk region, multiple rings --------
    {
        const c2d::Element rect = c2d::Element::makeRectangle(QPointF(30, 20), 40, 20);
        const c2d::Toolpath tp = makeTp("pocket_toolpath", {{"stepover", 3.0}});
        const QVector<c2d::Op> ops =
            c2d::Cam::toolpathOps(tp, {rect.painterPath}, nullptr);
        check(!ops.isEmpty(), "pocket produces ops");
        const QRectF bb = feedBounds(ops);
        check(bb.left() >= 10 + 3.175 - 0.05 && bb.right() <= 50 - 3.175 + 0.05 &&
              bb.top() >= 10 + 3.175 - 0.05 && bb.bottom() <= 30 - 3.175 + 0.05,
              "pocket stays inside region");
        // More than one ring: some cut is strictly inside the boundary pass.
        bool inner = false;
        for (const c2d::Op &op : ops)
            if (op.kind == c2d::Op::Feed && op.z < 0 &&
                op.x > 10 + 3.175 + 1.0 && op.x < 50 - 3.175 - 1.0 &&
                op.y > 10 + 3.175 + 1.0 && op.y < 30 - 3.175 - 1.0)
                inner = true;
        check(inner, "pocket has inner rings");
    }

    // --- CAM: drilling pecks at circle centers ----------------------------
    {
        const c2d::Element hole = c2d::Element::makeCircle(QPointF(15, 15), 2);
        const c2d::Toolpath tp = makeTp("drilling_toolpath",
                                        {{"end_depth", QStringLiteral("3.000")},
                                         {"stepdown", 1.0}});
        const QVector<c2d::Op> ops =
            c2d::Cam::toolpathOps(tp, {hole.painterPath}, nullptr);
        int plunges = 0;
        double deepest = 0;
        for (const c2d::Op &op : ops)
            if (op.kind == c2d::Op::Feed) {
                ++plunges;
                check(approx(op.x, 15, 1e-3) && approx(op.y, 15, 1e-3),
                      "drill at circle center");
                if (op.z < deepest)
                    deepest = op.z;
            }
        check(plunges == 3, "drill pecks per stepdown");
        check(approx(deepest, -3.0), "drill reaches end depth");
    }

    // --- CAM: unsupported type is skipped with a note ----------------------
    {
        const c2d::Element rect = c2d::Element::makeRectangle(QPointF(30, 20), 40, 20);
        const c2d::Toolpath tp = makeTp("texture_toolpath");
        QStringList notes;
        const QVector<c2d::Op> ops =
            c2d::Cam::toolpathOps(tp, {rect.painterPath}, &notes);
        check(ops.isEmpty() && notes.size() == 1, "unsupported type skipped");
    }

    // --- GrblSender: one line in flight, ok/error sequencing ---------------
    {
        QList<QByteArray> wrote;
        c2d::GrblSender sender([&wrote](const QByteArray &b) { wrote.append(b); });
        sender.start({QStringLiteral("G90"), QStringLiteral("G21"),
                      QStringLiteral("M02")});
        check(wrote.size() == 1 && wrote.at(0) == "G90\n", "sender first line");
        sender.onData("ok\r\n");
        check(wrote.size() == 2 && wrote.at(1) == "G21\n", "sender advances on ok");
        sender.onData("error:5\r\n");
        check(wrote.size() == 3 && sender.errors() == 1,
              "sender advances on error");
        sender.onData("ok\r\n");
        check(sender.finished() && sender.acked() == 3, "sender finishes");
    }

    // --- Document round trip (needs a sample .c2d as argv[1]) -------------
    if (argc > 1) {
        const QString src = QString::fromLocal8Bit(argv[1]);
        c2d::Document doc;
        QString err;
        check(doc.load(src, &err), "load sample c2d");
        check(!doc.elements().isEmpty(), "sample has elements");
        const int n = doc.elements().size();

        const QRectF bb0 = doc.elements()[0].painterPath.boundingRect();
        doc.elements()[0].translate(5, 5);
        doc.addElement(c2d::Element::makeCircle(
            QPointF(doc.boardWidth() / 2, doc.boardHeight() / 2), 4));

        // Toolpath-parameter edit: a nested number and a depth string (which
        // must stay a string - its sign convention is build-dependent).
        const int nTp = doc.toolpaths().size();
        check(nTp > 0, "sample has toolpaths");
        {
            c2d::Toolpath &tp = doc.toolpaths()[0];
            QJsonObject speeds = tp.json.value("speeds").toObject();
            speeds.insert("feedrate", 1234);
            tp.json.insert("speeds", speeds);
            tp.json.insert("end_depth", QStringLiteral("9.999"));
        }

        const QString dst = QDir::temp().filePath(QStringLiteral("phobiccc_test.c2d"));
        QFile::remove(dst);
        check(doc.save(dst, &err), "save edited c2d");

        c2d::Document doc2;
        check(doc2.load(dst, &err), "reload edited c2d");
        check(doc2.elements().size() == n + 1, "element count after add");

        // The saved order matches m_elements, so element 0 is the moved one.
        const QRectF bb1 = doc2.elements()[0].painterPath.boundingRect();
        check(approx(bb1.center().x(), bb0.center().x() + 5, 1e-3) &&
              approx(bb1.center().y(), bb0.center().y() + 5, 1e-3),
              "translation survives round trip");

        check(doc2.toolpaths().size() == nTp, "toolpath count preserved");
        {
            const QJsonObject &tj = doc2.toolpaths()[0].json;
            check(tj.value("speeds").toObject().value("feedrate").toDouble() == 1234,
                  "toolpath feedrate survives round trip");
            const QJsonValue depth = tj.value("end_depth");
            check(depth.isString() && depth.toString() == QLatin1String("9.999"),
                  "toolpath depth stays a string");
        }
        QFile::remove(dst);

        // CAM end to end over the real sample: supported types produce ops,
        // unsupported ones produce notes, and the post emits a program.
        {
            c2d::Document camDoc;
            check(camDoc.load(src, &err), "load for cam test");
            const c2d::Cam::Result cam = c2d::Cam::generate(camDoc);
            check(!cam.ops.isEmpty(), "sample cam produces ops");
            check(!cam.notes.isEmpty(), "sample cam notes unsupported types");
            const QString gcode = c2d::GrblPost(true).generate(cam.ops);
            check(gcode.startsWith(QLatin1String("G90")), "gcode starts with G90");
            check(gcode.contains(QLatin1String("G1")) &&
                  gcode.trimmed().endsWith(QLatin1String("M02")),
                  "gcode has cuts and ends with M02");
        }

        // removeElement must strip the element's uuid from toolpath references.
        {
            c2d::Document doc3;
            check(doc3.load(src, &err), "load for removal test");
            int refTp = -1;
            QString refUuid;
            for (int t = 0; t < doc3.toolpaths().size() && refTp < 0; ++t) {
                const QJsonArray refs =
                    doc3.toolpaths()[t].json.value("elements").toArray();
                if (!refs.isEmpty()) {
                    refTp = t;
                    refUuid = refs.at(0).toObject().value("uuid").toString();
                }
            }
            check(refTp >= 0, "sample has a referenced element");
            int elemIdx = -1;
            for (int i = 0; i < doc3.elements().size(); ++i)
                if (doc3.elements()[i].id == refUuid) { elemIdx = i; break; }
            check(elemIdx >= 0, "referenced element resolves");

            const int ne = doc3.elements().size();
            doc3.removeElement(elemIdx);
            check(doc3.elements().size() == ne - 1, "element removed");
            bool dangling = false;
            for (const c2d::Toolpath &t : doc3.toolpaths())
                for (const QJsonValue &r : t.json.value("elements").toArray())
                    if (r.toObject().value("uuid").toString() == refUuid)
                        dangling = true;
            check(!dangling, "dangling toolpath reference stripped");
        }
    } else {
        std::fprintf(stderr, "note: no .c2d passed, Document round trip skipped\n");
    }

    std::printf("OK: %d checks passed\n", g_checks);
    return 0;
}
