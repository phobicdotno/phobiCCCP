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
#include "../src/grblstreamer.h"
#include "../src/post_grbl.h"

#include <QApplication>
#include <QEvent>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>

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
        // Without a BitSetter the bit is wherever the operator left it after
        // the change, maybe just above the stock: up first, then across.
        const int a = int(o.indexOf(QStringLiteral("G0Z2.540")));
        check(a > 0 && o.value(a + 1) == QLatin1String("G0X10.000Y20.000"),
              "first rapid after a tool change: Z up, then XY across");
        const QStringList start = c2d::safeRapidsTransform(
            {QStringLiteral("G90"), QStringLiteral("G0X10.000Y20.000Z2.540")});
        check(start.value(1) == QLatin1String("G0X10.000Y20.000") && start.value(2) == QLatin1String("G0Z2.540"),
              "first rapid of the program (the sender rose to the top): across, then down");
        const int b = int(o.indexOf(QStringLiteral("G0X50.000Y60.000")));
        check(b > 0 && o.value(b + 1) == QLatin1String("G0Z1.000"),
              "a modal G0 that descends: across, then down");
        const int c = int(o.indexOf(QStringLiteral("G0Z10.000")));
        check(c > 0 && o.value(c + 1) == QLatin1String("G0X0Y0"), "a rapid that climbs: up, then across");
        check(o.contains(QStringLiteral("G1Z-1.000F300")) && o.contains(QStringLiteral("X30.000F1500")),
              "cutting moves untouched");
    }

    // 2026-10-26: after the BitSetter measured a bit the machine came down at
    // the Park spot with the spindle starting, instead of crossing to the
    // first cut at the top first. The look-ahead that finds where to cross to
    // stopped at the post's spindle-on line ("M03S..."), which comes after the
    // retract and before the first X/Y. Run it over a real program, after
    // every tool change, the way the sender sees it.
    {
        c2d::Document two;
        addToolpath(two, 0, layer);
        addToolpath(two, 1, layer);
        c2d::Toolpath tp = two.toolpaths().at(1);
        QJsonObject tool = tp.json.value("tool").toObject();
        tool["number"] = 202;
        tool["diameter"] = 6.35;
        tp.json["tool"] = tool;
        two.replaceToolpath(tp);
        const c2d::GcodeResult r = c2d::exportGcode(two);
        const QStringList lines = c2d::safeRapidsTransform(r.gcode.split(QChar('\n'), Qt::SkipEmptyParts));
        int changes = 0;
        for (int i = 0; i < lines.size(); ++i) {
            if (!lines.at(i).startsWith(QLatin1String("M0 ;T")))
                continue;
            ++changes;
            // Where the program goes across first, and the first line after
            // the change that names X and Y.
            QString want;
            for (int k = i + 1; k < lines.size() && want.isEmpty(); ++k) {
                const QRegularExpressionMatch m =
                    QRegularExpression(QStringLiteral("^(?:G0)?X(-?[0-9.]+)Y(-?[0-9.]+)$")).match(lines.at(k));
                if (m.hasMatch())
                    want = QStringLiteral("G90 G0 X%1 Y%2").arg(m.captured(1), m.captured(2));
            }
            const QString got = c2d::GrblStreamer::crossToFirstCut(lines, i + 1);
            check(!want.isEmpty() && got == want,
                  "real program: after every tool change, cross to the first cut at the top "
                  "(past the spindle start)");
        }
        check(changes == 2, "real program: two tool changes, one at the start");
        check(lines.contains(QStringLiteral("M03S10000")), "real program: the spindle starts after the change");
    }

    std::printf("test_exportjob: %d checks OK\n", g_checks);
    return 0;
}
