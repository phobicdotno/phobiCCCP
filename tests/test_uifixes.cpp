// Desktop UI regressions: two bits that share a tool number (setup sheet,
// Simulate tool strip and preview) and the inlay male's depth bindings.
//
// Plain asserts, no framework; exits 0 on success.

#include "../src/c2ddocument.h"
#include "../src/cam3d.h"
#include "../src/heightmodel.h"
#include "../src/model3d.h"
#include "../src/canvas.h"
#include "../src/element.h"
#include "../src/gcodeexport.h"
#include "../src/isopreview.h"
#include "../src/parametric.h"
#include "../src/setupsheet.h"
#include "../src/toolpathfactory.h"
#include "../src/toolpathpanel.h"
#include "../src/toolstrip.h"

#include <QApplication>
#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QListWidget>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>

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

static double num(const QJsonValue &v) { return v.isString() ? v.toString().toDouble() : v.toDouble(); }

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QTemporaryDir tmp;
    QString err;
    const QString blank = tmp.filePath("a.c2d");
    check(c2d::Document::createBlank(blank, 200, 200, 19, &err), "createBlank");

    // Two pockets, both T0: a 6.35 then a 3.175 bit. The program changes
    // tools between them, so everything that lists tools lists both.
    {
        c2d::Document d;
        check(d.load(blank, &err), "load");
        const c2d::Element r1 = c2d::Element::makeRectangle({50, 50}, 40, 40, d.defaultLayer());
        const c2d::Element r2 = c2d::Element::makeRectangle({150, 150}, 40, 40, d.defaultLayer());
        d.addElement(r1);
        d.addElement(r2);
        c2d::Toolpath a = c2d::makeToolpath(d, "pocket_toolpath", {r1.id});
        QJsonObject ta = a.json.value("tool").toObject();
        ta["number"] = 0;
        ta["diameter"] = 6.35;
        ta["name"] = "quarter";
        a.json["tool"] = ta;
        a.json["name"] = "A";
        a.json["end_depth"] = "-2.000";
        d.insertToolpath(0, a);
        c2d::Toolpath b = c2d::makeToolpath(d, "pocket_toolpath", {r2.id});
        QJsonObject tb = b.json.value("tool").toObject();
        tb["number"] = 0;
        tb["diameter"] = 3.175;
        tb["name"] = "eighth";
        b.json["tool"] = tb;
        b.json["name"] = "B";
        b.json["end_depth"] = "-2.000";
        d.insertToolpath(1, b);
        c2d::Document e = d;
        const c2d::GcodeResult g = c2d::exportGcode(e);
        check(g.gcode.count(QStringLiteral("M0 ;T0")) == 2, "two tool changes in the program");

        const QString h = c2d::setupSheetHtml(d, QStringLiteral("t"));
        const int s = h.indexOf(QStringLiteral("<h2>Tools</h2>"));
        const QString tools = h.mid(s, h.indexOf(QStringLiteral("</table>"), s) - s);
        check(tools.count(QStringLiteral("<tr><td")) == 2, "setup sheet: one row per bit");
        check(tools.contains(QStringLiteral("6.350")) && tools.contains(QStringLiteral("3.175")),
              "setup sheet: both diameters listed");
        check(tools.indexOf(QStringLiteral("quarter")) < tools.indexOf(QStringLiteral("eighth")),
              "setup sheet: in the order they are loaded");

        c2d::ToolStrip strip;
        c2d::IsoPreview iso;
        QObject::connect(&strip, &c2d::ToolStrip::toolClicked, &iso, &c2d::IsoPreview::seekToTool);
        QObject::connect(&iso, &c2d::IsoPreview::currentToolChanged, &strip, &c2d::ToolStrip::setCurrent);
        strip.setJob(d, g.ops);
        iso.setJob(g.ops, 200, 200, 19);
        auto *list = strip.findChild<QListWidget *>();
        check(list && list->count() == 2, "strip: two entries");
        iso.setProgress(0);
        check(list->currentRow() == 0, "strip: the first bit is current at the start");
        emit list->itemClicked(list->item(1));
        check(iso.progress() > 0.01, "strip: the second entry seeks to the second bit");
        check(iso.currentTool() == 0 && iso.currentBit().contains(QStringLiteral("3.175")),
              "preview: the 3.175 bit is in the spindle");
        check(list->currentRow() == 1, "strip: the second entry is highlighted");
        emit list->itemClicked(list->item(0));
        check(iso.progress() < 1e-9 && list->currentRow() == 0, "strip: back to the first bit");
        iso.setProgress(0.99);
        check(list->currentRow() == 1, "strip: a seek by progress moves the highlight too");
    }

    // The setup sheet describes what Export writes: a 3D finish over a model
    // that is being edited (not in the file) is timed, not "no 3D model".
    {
        c2d::installModelProvider();
        // A small board: the finish covers all of it.
        const QString small = tmp.filePath("s.c2d");
        check(c2d::Document::createBlank(small, 40, 30, 19, &err), "sheet 3D: createBlank");
        c2d::Document d;
        check(d.load(small, &err), "sheet 3D: load");
        const c2d::Element r = c2d::Element::makeRectangle({20, 15}, 20, 12, d.defaultLayer());
        d.addElement(r);
        c2d::ModelStore store;
        store.attach(&d);
        c2d::ModelComponent c;
        c.id = c2d::Model3D::newId();
        c.vectorIds = {r.id};
        c.shape = c2d::ModelComponent::Flat;
        c.height = 3;
        store.model.components << c;
        store.invalidate();
        c2d::Toolpath t;
        t.json = c2d::defaultFinishToolpathJson();
        t.json.insert("stepover", 3.0);   // coarse: the sheet exports it twice
        t.uuid = t.json.value("uuid").toString();
        t.type = t.json.value("type").toString();
        d.insertToolpath(0, t);
        const QString h = c2d::setupSheetHtml(d, QStringLiteral("t"));
        check(!h.contains(QStringLiteral("no 3D model")), "sheet 3D: the live model is used");
        check(h.contains(QStringLiteral("Extents")), "sheet 3D: the finish is in the totals");
    }

    // Inlay male from a v-carve whose end depth is bound to a parameter:
    // the male's start binding follows it less the glue gap, so re-evaluating keeps it.
    {
        c2d::Document d;
        check(d.load(blank, &err), "load");
        d.parametricRef().params.append({QStringLiteral("carve"), QStringLiteral("3"), QString()});
        const c2d::Element r = c2d::Element::makeCircle({50, 50}, 15, d.defaultLayer());
        d.addElement(r);
        c2d::Toolpath t = c2d::makeToolpath(d, "advanced_vcarve_toolpath", {r.id});
        t.json["end_depth"] = "-3.000";
        t.json["phobic_expr"] = QJsonObject{{"end_depth", QJsonObject{{"expr", "carve"}, {"neg", true},
                                                                        {"str", true}, {"min", 0.0},
                                                                        {"max", 100.0}}}};
        d.insertToolpath(0, t);
        c2d::Canvas canvas;
        c2d::ToolpathPanel panel(&canvas);
        canvas.setDocument(&d);
        panel.setDocument(&d);
        check(panel.currentUuid() == t.uuid, "the v-carve is current");
        QPushButton *inlay = nullptr;
        for (QPushButton *b : panel.findChildren<QPushButton *>())
            if (b->text().contains(QStringLiteral("nlay")))
                inlay = b;
        check(inlay != nullptr, "an inlay button");
        QTimer accept;   // the gap / margin questions and the summary: defaults
        accept.setInterval(50);
        QObject::connect(&accept, &QTimer::timeout, [] {
            for (QWidget *w : QApplication::topLevelWidgets())
                if (auto *dlg = qobject_cast<QDialog *>(w); dlg && dlg->isVisible() && dlg->isModal())
                    dlg->accept();
        });
        accept.start();
        inlay->click();
        accept.stop();
        check(d.toolpaths().size() == 2, "a male toolpath was added");
        const QString male = d.toolpaths().at(1).uuid;
        // The mirrored outline follows the circle: Qt's default flattening
        // made it a coarse polygon, the plug ~0.2 mm undersize mid-chord.
        bool sawOutline = false;
        for (const QJsonValue &v : d.toolpathByUuid(male)->json.value("elements").toArray()) {
            const c2d::Element *e = d.elementById(v.toObject().value("uuid").toString());
            if (!e || e->painterPath.boundingRect().width() > 31)
                continue;   // the border
            sawOutline = true;
            const QPointF c = e->painterPath.boundingRect().center();
            double worst = 0;
            for (const QPolygonF &p : e->painterPath.toSubpathPolygons(QTransform::fromScale(100, 100)))
                for (int i = 0; i + 1 < p.size(); ++i)
                    for (int k = 0; k < 4; ++k)
                        worst = std::max(worst, std::fabs(15 - QLineF((p[i] + (p[i + 1] - p[i]) * (k / 4.0)) / 100, c).length()));
            check(worst < 0.02, "the male outline follows the circle within 0.02 mm");
        }
        check(sawOutline, "the male has the mirrored outline");
        // The plug wedges in at the male's start depth: 0.5 short of the
        // pocket leaves 0.5 of glue under it; the male floor stands it 0.5 proud.
        auto depths = [&](double start, double end) {
            const c2d::Toolpath *m = d.toolpathByUuid(male);
            return std::fabs(num(m->json.value("start_depth")) + start) < 1e-9
                && std::fabs(num(m->json.value("end_depth")) + end) < 1e-9;
        };
        check(depths(2.5, 3.0), "male starts at carve - 0.5 gap and ends at carve");
        c2d::param::applyToolpathBindings(d, nullptr);
        check(depths(2.5, 3.0), "re-evaluating the bindings keeps the gap");
        check(std::fabs(num(d.toolpathByUuid(t.uuid)->json.value("end_depth")) + 3.0) < 1e-9,
              "the female stays at carve");
        d.parametricRef().params[0].expr = QStringLiteral("4");
        c2d::param::applyToolpathBindings(d, nullptr);
        check(depths(3.5, 4.0), "a parameter edit moves the male with the gap");
    }

    std::printf("test_uifixes: %d checks OK\n", g_checks);
    return 0;
}
