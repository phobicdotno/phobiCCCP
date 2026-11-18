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
#include "../src/popuplook.h"
#include "../src/setupsheet.h"
#include "../src/toolpathfactory.h"
#include "../src/toolpathpanel.h"
#include "../src/toolstrip.h"

#include <QApplication>
#include <QDialog>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QAbstractButton>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QWindow>

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

// The painted 3D area of a preview: its largest child widget.
static QWidget *isoArea(c2d::IsoPreview &iso)
{
    QWidget *best = nullptr;
    for (QWidget *w : iso.findChildren<QWidget *>(Qt::FindDirectChildrenOnly))
        if (!best || w->width() * w->height() > best->width() * best->height())
            best = w;
    return best;
}

static QImage isoShot(c2d::IsoPreview &iso)
{
    QApplication::processEvents();
    return isoArea(iso)->grab().toImage();
}

// A drag across the 3D area: an orbit, as a finger or the mouse makes it.
static void isoOrbit(c2d::IsoPreview &iso, QPoint from, QPoint to)
{
    QWidget *v = isoArea(iso);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(from), QPointF(v->mapToGlobal(from)), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(v, &press);
    QMouseEvent move(QEvent::MouseMove, QPointF(to), QPointF(v->mapToGlobal(to)), Qt::NoButton, Qt::LeftButton,
                     Qt::NoModifier);
    QApplication::sendEvent(v, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(to), QPointF(v->mapToGlobal(to)), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(v, &release);
}

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

        // The 3D preview keeps the route projected for the view between
        // paints (it projected every point on every playback tick). What it
        // paints is what a fresh preview paints, after playback, a resize,
        // an orbit, a zoom and a new job.
        QVector<c2d::Op> mirrored = g.ops;     // the same job turned over in x
        for (c2d::Op &o : mirrored)
            if (o.kind == c2d::Op::Rapid || o.kind == c2d::Op::Feed || o.kind == c2d::Op::Arc) {
                o.x = 200 - o.x;
                o.ci = -o.ci;
                o.cw = !o.cw;
            }
        c2d::IsoPreview p, q;
        for (c2d::IsoPreview *v : {&p, &q}) {
            v->resize(640, 420);
            v->show();
        }
        p.setJob(g.ops, 200, 200, 19);
        q.setJob(g.ops, 200, 200, 19);
        p.setProgress(0.3);
        q.setProgress(0.3);
        const QImage at3 = isoShot(p);
        check(!at3.isNull() && at3 == isoShot(q), "3D preview: two previews of one job paint alike");
        p.setProgress(0.7);
        const QImage at7 = isoShot(p);
        p.setProgress(0.3);
        check(at7 != at3 && isoShot(p) == at3, "3D preview: played on and back, the same picture");
        p.resize(500, 360);
        const QImage small = isoShot(p);
        p.resize(640, 420);
        check(small.size() != at3.size() && isoShot(p) == at3, "3D preview: resized and back, the same picture");
        p.setJob(mirrored, 200, 200, 19);
        q.setJob(mirrored, 200, 200, 19);
        p.setProgress(0.3);
        q.setProgress(0.3);
        const QImage turned = isoShot(p);
        check(turned != at3 && turned == isoShot(q), "3D preview: a new job is drawn, not the one before");
        isoOrbit(p, {300, 200}, {340, 180});
        isoOrbit(q, {300, 200}, {340, 180});
        const QImage orbited = isoShot(p);
        check(orbited != turned && orbited == isoShot(q), "3D preview: an orbit turns the route with the stock");
        for (c2d::IsoPreview *v : {&p, &q})
            isoArea(*v)->findChild<QToolButton *>(QStringLiteral("zoomIn"))->click();
        const QImage zoomed = isoShot(p);
        check(zoomed != orbited && zoomed == isoShot(q), "3D preview: and a zoom scales it");
        p.resetView();
        q.resetView();
        check(isoShot(p) == isoShot(q) && isoShot(p) == turned, "3D preview: and the view reset puts it back");
    }

    // A route of more than 200 000 moves is drawn a few moves to a line.
    // Such a line never runs from a cut into a rapid: it drew a cut line
    // straight across the stock, from the end of one cut to the start of
    // the next one far away.
    {
        QVector<c2d::Op> ops{c2d::Op::tool(1), c2d::Op::rapid(10, 10, 5), c2d::Op::feedTo(10, 10, -1, 1000)};
        // Moves 1..100000 zig-zag in a 10 mm patch at (10, 10); move 100000
        // starts a line of two, and the move after it is the rapid across.
        const int n = 100000;
        for (int k = 1; k <= n; ++k)
            ops << c2d::Op::feedTo(k % 2 ? 20 : 10, 10 + 10.0 * k / n, -1, 1000);
        ops << c2d::Op::rapid(190, 190, 5) << c2d::Op::feedTo(190, 190, -1, 1000);
        for (int k = 1; k <= n; ++k)
            ops << c2d::Op::feedTo(k % 2 ? 180 : 190, 190 - 10.0 * k / n, -1, 1000);
        ops << c2d::Op::rapid(180, 180, 5);
        c2d::IsoPreview iso;
        iso.resize(640, 420);
        iso.show();
        iso.setJob(ops, 200, 200, 19);
        iso.setProgress(1);
        QImage img = isoShot(iso);
        // The two patches lie at the two ends of the stock's diagonal, left
        // and right in this view; the rapid between them crosses the middle.
        auto cutPixels = [&](int x0, int x1) {
            int k = 0;
            for (int y = 0; y < img.height(); ++y)
                for (int x = x0; x < x1; ++x) {
                    const QColor c = img.pixelColor(x, y);
                    k += c.red() > 150 && c.blue() < 100 && c.red() - c.blue() > 100;
                }
            return k;
        };
        auto drawnPixels = [&](int x0, int x1) {
            int k = 0;
            const QRgb bg = img.pixel(2, img.height() / 2);
            for (int y = img.height() / 4; y < img.height() * 3 / 4; ++y)
                for (int x = x0; x < x1; ++x)
                    k += img.pixel(x, y) != bg;
            return k;
        };
        const int w = img.width();
        check(cutPixels(0, w / 3) > 50 && cutPixels(w * 2 / 3, w) > 50, "long route: both patches are drawn");
        check(drawnPixels(w * 2 / 5, w * 3 / 5) > 0, "long route: the stock and the rapid are in the middle");
        check(cutPixels(w * 2 / 5, w * 3 / 5) == 0, "long route: no cut line runs across to the other patch");
        // Where a cut does run across, the same look sees it.
        iso.setJob({c2d::Op::tool(1), c2d::Op::rapid(10, 10, 5), c2d::Op::feedTo(10, 10, -1, 1000),
                    c2d::Op::feedTo(20, 20, -1, 1000), c2d::Op::feedTo(10, 20, -1, 1000),
                    c2d::Op::feedTo(190, 190, -1, 1000), c2d::Op::feedTo(180, 180, -1, 1000),
                    c2d::Op::feedTo(190, 180, -1, 1000), c2d::Op::rapid(190, 180, 5)},
                   200, 200, 19);
        iso.setProgress(1);
        img = isoShot(iso);
        check(img.width() == w && cutPixels(w * 2 / 5, w * 3 / 5) > 20, "long route: a real cut across is seen there");
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
        // A construction guide in the selection: the female leaves it out,
        // so the male must too.
        c2d::Element guide = c2d::Element::makeCircle({50, 50}, 25, d.defaultLayer());
        guide.raw.insert("construction", true);
        d.addElement(guide);
        c2d::Toolpath t = c2d::makeToolpath(d, "advanced_vcarve_toolpath", {r.id, guide.id});
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
        check(d.toolpathByUuid(male)->json.value("elements").toArray().size() == 2,
              "the male has the border and the outline, not the construction guide");
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

    // Every popup gets the shop look: no title bar, the title as a caption
    // inside, and still its own buttons to close it with. A popup with
    // nothing to press gets a big X.
    {
        c2d::installPopupLook(app);
        QMessageBox box(QMessageBox::Warning, QStringLiteral("Save failed"),
                        QStringLiteral("The file could not be written."),
                        QMessageBox::Save | QMessageBox::Cancel);
        box.show();
        QApplication::processEvents();
        check(box.windowFlags() & Qt::FramelessWindowHint, "popup look: a message box has no title bar");
        auto *cap = box.findChild<QLabel *>(QStringLiteral("popupCaption"));
        check(cap && cap->text() == QLatin1String("SAVE FAILED"), "popup look: the title is the caption");
        check(box.button(QMessageBox::Cancel) && box.button(QMessageBox::Cancel)->isVisible(),
              "popup look: the buttons are still there to close it");
        if (!qEnvironmentVariableIsEmpty("POPUP_SHOT"))
            box.grab().save(QString::fromLocal8Bit(qgetenv("POPUP_SHOT")) + QStringLiteral("-box.png"));
        box.close();

        QInputDialog in;
        in.setWindowTitle(QStringLiteral("New design"));
        in.setLabelText(QStringLiteral("Stock width (mm)"));
        in.setInputMode(QInputDialog::DoubleInput);
        in.show();
        QApplication::processEvents();
        check(in.windowFlags() & Qt::FramelessWindowHint, "popup look: an input box has no title bar");
        if (!qEnvironmentVariableIsEmpty("POPUP_SHOT"))
            in.grab().save(QString::fromLocal8Bit(qgetenv("POPUP_SHOT")) + QStringLiteral("-input.png"));
        in.close();

        QDialog bare;
        bare.setWindowTitle(QStringLiteral("Nothing to press"));
        bare.show();
        QApplication::processEvents();
        auto *x = bare.findChild<QToolButton *>(QStringLiteral("popupClose"));
        check((bare.windowFlags() & Qt::FramelessWindowHint) && x && x->isVisible()
                  && x->geometry().right() <= bare.width() && x->x() > bare.width() / 2,
              "popup look: a popup without a button gets an X in its top right corner");
        if (x)
            x->click();
        QApplication::processEvents();
        check(!bare.isVisible(), "popup look: and the X closes it");

        // The window itself is made without a frame, and a box closes from
        // its buttons the first time it is shown (Qt 6.4 kept it open).
        QMessageBox again(QMessageBox::Question, QStringLiteral("Close?"), QStringLiteral("Really?"),
                          QMessageBox::Yes | QMessageBox::No);
        again.show();
        QApplication::processEvents();
        check(again.windowHandle() && (again.windowHandle()->flags() & Qt::FramelessWindowHint),
              "popup look: the window is made without a title bar");
        again.button(QMessageBox::No)->click();
        QApplication::processEvents();
        check(!again.isVisible(), "popup look: a button closes the box the first time");

        // The bold default button was sized from the plain font, so a long
        // label ("Close without Saving") was cut off at both ends.
        QMessageBox unsaved(QMessageBox::Warning, QStringLiteral("Unsaved changes"),
                            QStringLiteral("a.c2d has unsaved changes.\n\nSave before opening another file?"),
                            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        unsaved.setDefaultButton(QMessageBox::Discard);
        unsaved.button(QMessageBox::Discard)->setText(QStringLiteral("Close without Saving")); // GNOME's label
        unsaved.show();
        QApplication::processEvents();
        bool fits = true;
        for (QAbstractButton *b : unsaved.buttons()) {
            QFont f = b->font();
            f.setBold(true);
            QString t = b->text();
            t.remove(QLatin1Char('&'));
            if (b->width() < QFontMetrics(f).horizontalAdvance(t) + 40)
                fits = false;
        }
        check(fits, "popup look: every button label fits in bold");
        if (!qEnvironmentVariableIsEmpty("POPUP_SHOT"))
            unsaved.grab().save(QString::fromLocal8Bit(qgetenv("POPUP_SHOT")) + QStringLiteral("-unsaved.png"));
        unsaved.close();
    }

    std::printf("test_uifixes: %d checks OK\n", g_checks);
    return 0;
}
