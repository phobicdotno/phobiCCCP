// The threaded export path (src/exportprogress.cpp): the worker must produce
// exactly what a plain synchronous export produces, and must share nothing
// with the thread that started it. Worth re-running under ThreadSanitizer
// after any change to that file; as a plain test it still catches a hang or
// a wrong result.
//
// Needs a QApplication because the helper puts a modal progress dialog up.

#include "../src/c2ddocument.h"
#include "../src/element.h"
#include "../src/exportprogress.h"
#include "../src/gcodeexport.h"
#include "../src/post_grbl.h"

#include <QApplication>
#include <QEvent>
#include <QJsonArray>
#include <QJsonObject>

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

static void addToolpath(c2d::Document &doc, int i, const QJsonObject &layer)
{
    const c2d::Element e =
        c2d::Element::makeRectangle({60.0 + 70 * i, 60}, 40, 40, layer);
    doc.addElement(e);
    c2d::Toolpath tp;
    tp.uuid = QStringLiteral("{tp-%1}").arg(i);
    tp.type = QStringLiteral("contour");
    tp.json = QJsonObject{
        {"type", "contour"}, {"name", QStringLiteral("c%1").arg(i)},
        {"enabled", true}, {"uuid", tp.uuid}, {"ofset_dir", 1},
        {"start_depth", QStringLiteral("0.000")},
        {"end_depth", QStringLiteral("3.000")}, {"stepdown", 0.5},
        {"elements", QJsonArray{QJsonObject{{"uuid", e.id}}}},
        {"speeds", QJsonObject{{"feedrate", 1000}, {"plungerate", 300}, {"rpm", 10000}}},
        {"tool", QJsonObject{{"diameter", 3.175}, {"number", 201}}}};
    doc.addToolpath(tp);
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    const QJsonObject layer;
    c2d::Document doc;
    for (int i = 0; i < 6; ++i)
        addToolpath(doc, i, layer);

    const c2d::GcodeResult sync = c2d::exportGcode(doc);
    check(sync.done.size() == 6 && !sync.cancelled, "sync export produces six toolpaths");

    // Several runs: a race is timing-dependent and one pass proves little.
    for (int run = 0; run < 5; ++run) {
        const c2d::GcodeResult threaded =
            c2d::exportWithProgress(nullptr, doc, QStringLiteral("test"));
        check(!threaded.cancelled, "threaded export is not reported as cancelled");
        check(threaded.done == sync.done, "threaded export machines the same toolpaths");
        check(threaded.gcode == sync.gcode, "threaded export is byte-identical to the sync one");
        check(threaded.ops.size() == sync.ops.size(), "and emits the same operations");
    }

    // The document the caller holds must be untouched by the worker.
    check(doc.toolpaths().size() == 6, "the caller's document still has its toolpaths");
    check(doc.elements().size() == 6, "and its elements");

    // The helper defers its worker's destruction; drain that so the test does
    // not exit holding one.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    // Rapids into a job: across then down, up then across.
    {
        const QStringList in = {QStringLiteral("G90"), QStringLiteral("G21"), QStringLiteral("M0 ;T602"),
                                QStringLiteral("(Pocket)"), QStringLiteral("M03S18000"),
                                QStringLiteral("G0X10.000Y20.000Z2.540"), QStringLiteral("G1Z-1.000F300"),
                                QStringLiteral("X30.000F1500"), QStringLiteral("G0Z2.540"),
                                QStringLiteral("X50.000Y60.000Z1.000"),
                                QStringLiteral("G0X0Y0Z10.000"), QStringLiteral("M05")};
        const QStringList o = c2d::safeRapidsTransform(in);
        const int a = int(o.indexOf(QStringLiteral("G0X10.000Y20.000")));
        check(a > 0 && o.value(a + 1) == QLatin1String("G0Z2.540"),
              "first rapid after a tool change: XY across, then Z down");
        const int b = int(o.indexOf(QStringLiteral("G0X50.000Y60.000")));
        check(b > 0 && o.value(b + 1) == QLatin1String("G0Z1.000"),
              "a modal G0 that descends: across, then down");
        const int c = int(o.indexOf(QStringLiteral("G0Z10.000")));
        check(c > 0 && o.value(c + 1) == QLatin1String("G0X0Y0"), "a rapid that climbs: up, then across");
        check(o.contains(QStringLiteral("G1Z-1.000F300")) && o.contains(QStringLiteral("X30.000F1500")),
              "cutting moves untouched");
    }

    std::printf("test_exportjob: %d checks OK\n", g_checks);
    return 0;
}
