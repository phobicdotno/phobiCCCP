// The main window's Preview: edits schedule the export on a worker instead of
// running it on the interface thread, a result for an older edit is dropped,
// and a synchronous rebuild (Simulate) serves what is pending. Also: the
// window does not close while an update step runs. Simulate with Preview off
// rebuilds after an edit, never inside it, and no key is bound twice.
//
// Plain asserts, no framework; exits 0 on success.

#define private public      // the preview's state is private to the window
#include "../src/jobrotate.h"
#include "../src/backgroundimage.h"
#include "../src/model3d.h"
#include "../src/mainwindow.h"
#include "../src/modelpanel.h"
#include "../src/canvas.h"
#include "../src/simpanel.h"
#include "../src/toolpathform.h"
#include "../src/toolpathpanel.h"
#undef private
#include "../src/c2ddocument.h"
#include "../src/element.h"
#include "../src/post_grbl.h"
#include "../src/toolpathfactory.h"

#include <QAction>
#include <QApplication>
#include <QGraphicsItem>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QImage>
#include <QLineF>
#include <QJsonObject>
#include <QMap>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QSettings>
#include <QTest>
#include <QStatusBar>
#include <QTimer>
#include <QTreeWidget>
#include <QToolBar>
#include <QPainter>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QToolButton>
#include <QUndoStack>

#include <cstdio>
#include <cstdlib>
#include <memory>

static int g_checks = 0;

static void check(bool cond, const char *what)
{
    ++g_checks;
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

// The toolpath names the overlay's program carries, in order.
static QStringList names(const QVector<c2d::Op> &ops)
{
    QStringList out;
    for (const c2d::Op &o : ops)
        if (o.kind == c2d::Op::Comment)
            out << o.text;
    return out;
}

static bool settled(const c2d::MainWindow &w)
{
    return !w.m_previewTimer->isActive() && !w.m_previewRun && w.m_previewServed == w.m_previewGen;
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("phobicccp-test"));
    QCoreApplication::setApplicationName(QStringLiteral("test_mainwindow"));

    c2d::MainWindow w;
    w.resize(1200, 800);
    w.show();
    check(w.newUntitled(300, 200, 19), "untitled design");
    c2d::Document &d = w.m_doc;
    const c2d::Element r = c2d::Element::makeRectangle({80, 80}, 40, 30, d.defaultLayer());
    const c2d::Element c = c2d::Element::makeCircle({200, 100}, 15, d.defaultLayer());
    d.addElement(r);
    d.addElement(c);
    w.m_canvas->setDocument(&d);
    c2d::Toolpath p = c2d::makeToolpath(d, "pocket_toolpath", {r.id});
    p.json.insert("name", "Pocket");
    d.insertToolpath(0, p);
    c2d::Toolpath k = c2d::makeToolpath(d, "contour", {c.id});
    k.json.insert("name", "Ring");
    d.insertToolpath(1, k);
    // Settings > Tab height is what a new contour starts with; a contour
    // starts outside (holes in it still come out inside).
    check(k.json.value("tab_height").toDouble() == 3.0 && k.json.value("ofset_dir").toInt() == 1,
          "a new contour: 3 mm tabs, outside");
    {
        QSettings().setValue("defaults/tabHeight", 5.5);
        const c2d::Toolpath t5 = c2d::makeToolpath(d, "contour", {c.id});
        QSettings().remove("defaults/tabHeight");
        check(t5.json.value("tab_height").toDouble() == 5.5, "the tab height setting is a new contour's tab height");
    }

    w.setMode(c2d::MainWindow::Mode::Toolpaths, true);   // turns Preview on
    check(w.m_previewAct->isChecked(), "Toolpaths mode turns Preview on");
    check(!w.m_canvas->hasToolpathPreview(), "the preview is not built on the spot");
    check(QTest::qWaitFor([&] { return settled(w); }, 30000), "the preview lands");
    check(names(w.m_canvas->m_previewOps) == QStringList({"Pocket", "Ring"}), "both toolpaths shown");
    check(!w.m_isoStale, "and the 3D preview has the job");

    // An edit returns at once: no export, no dialog, the old overlay stays
    // until the new one is ready.
    {
        QJsonObject j = d.toolpathByUuid(k.uuid)->json;
        j.insert("name", "Circle");
        w.m_canvas->editToolpath(k.uuid, j);
        check(QApplication::activeModalWidget() == nullptr, "edit: no progress dialog");
        check(w.m_previewTimer->isActive() && w.m_isoStale, "edit: the export is scheduled");
        check(names(w.m_canvas->m_previewOps) == QStringList({"Pocket", "Ring"}), "edit: old overlay kept meanwhile");
        check(QTest::qWaitFor([&] { return settled(w); }, 30000), "edit: the preview catches up");
        check(names(w.m_canvas->m_previewOps) == QStringList({"Pocket", "Circle"}), "edit: the overlay shows it");
    }

    // A newer edit while an export runs: only the newest lands.
    {
        QJsonObject j = d.toolpathByUuid(k.uuid)->json;
        j.insert("name", "First");
        w.m_canvas->editToolpath(k.uuid, j);
        check(QTest::qWaitFor([&] { return bool(w.m_previewRun) || settled(w); }, 30000), "stale: a run starts");
        j.insert("name", "Second");
        w.m_canvas->editToolpath(k.uuid, j);
        check(QTest::qWaitFor([&] { return settled(w); }, 30000), "stale: settles");
        check(names(w.m_canvas->m_previewOps) == QStringList({"Pocket", "Second"}), "stale: the newest edit is shown");
        QTest::qWait(100);
        check(names(w.m_canvas->m_previewOps) == QStringList({"Pocket", "Second"}), "stale: and stays");
    }

    // A synchronous rebuild (entering Simulate) serves what is pending.
    {
        QJsonObject j = d.toolpathByUuid(k.uuid)->json;
        j.insert("name", "Sim");
        w.m_canvas->editToolpath(k.uuid, j);
        check(w.m_isoStale, "simulate: pending edit marks the 3D preview stale");
        w.refreshIso(false);
        check(settled(w) && !w.m_isoStale, "simulate: the rebuild serves the pending edit");
        check(names(w.m_canvas->m_previewOps) == QStringList({"Pocket", "Sim"}), "simulate: overlay follows");
    }

    // Preview off: an export started before must not land as the job.
    {
        QJsonObject j = d.toolpathByUuid(k.uuid)->json;
        j.insert("name", "Off");
        w.m_canvas->editToolpath(k.uuid, j);
        w.m_previewAct->setChecked(false);
        j.insert("enabled", false);
        w.m_canvas->editToolpath(k.uuid, j);   // with Preview off: marks stale
        check(settled(w) && w.m_isoStale, "preview off: pending export dropped, 3D stale");
        QTest::qWait(400);
        check(w.m_isoStale && !w.m_canvas->hasToolpathPreview(), "preview off: nothing lands later");
    }

    // Simulate with Preview off: an undo step announces the change from
    // inside QUndoCommand::undo(). The 3D rebuild runs an event loop behind
    // its progress dialog; run there, a second Ctrl+Z (here: a zero timer)
    // undid the same command again, because QUndoStack moves its index only
    // after undo() returns.
    {
        check(!w.m_previewAct->isChecked(), "simulate undo: Preview is off");
        w.setMode(c2d::MainWindow::Mode::Simulate, false);
        QUndoStack *undo = w.m_canvas->undoStack();
        const double x0 = d.elementById(c.id)->painterPath.boundingRect().center().x();
        w.m_canvas->moveElementBy(c.id, 10, 0);
        w.m_canvas->moveElementBy(c.id, 10, 0);
        check(QTest::qWaitFor([&] { return !w.m_isoStale && !w.m_isoQueued; }, 30000),
              "simulate undo: the moves are rebuilt");
        const int idx = undo->index();
        bool second = false;
        QTimer::singleShot(0, &w, [&] { second = true; undo->undo(); });
        undo->undo();
        check(!second, "simulate undo: no event loop runs inside undo()");
        check(undo->index() == idx - 1, "simulate undo: one step back");
        check(QTest::qWaitFor([&] { return second; }, 5000), "simulate undo: the second press runs");
        check(undo->index() == idx - 2, "simulate undo: each press undoes its own step");
        const double x = d.elementById(c.id)->painterPath.boundingRect().center().x();
        check(qAbs(x - x0) < 1e-6, "simulate undo: both moves undone once each");
        check(QTest::qWaitFor([&] { return !w.m_isoStale && !w.m_isoQueued; }, 30000),
              "simulate undo: the program is rebuilt afterwards");
    }

    // An edit while the 3D rebuild's event loop runs: that program is for
    // neither state, so the rebuild runs again rather than nesting a second
    // one whose newer result the outer call then overwrote.
    {
        QJsonObject j = d.toolpathByUuid(p.uuid)->json;
        j.insert("name", "During");
        QTimer::singleShot(0, &w, [&] { w.m_canvas->editToolpath(p.uuid, j); });
        w.refreshIso(true);
        check(!w.m_isoBusy && !w.m_isoStale, "rebuild re-entry: settled, not stale");
        check(names(w.m_sim->m_ops).contains("During"), "rebuild re-entry: Simulate has the edit");
        check(QTest::qWaitFor([&] { return !w.m_isoQueued; }, 5000), "rebuild re-entry: queue drained");
        check(names(w.m_sim->m_ops).contains("During") && !w.m_isoStale,
              "rebuild re-entry: and keeps it");
    }

    // Every shortcut is bound once: a key on two actions in one window is
    // ambiguous and Qt fires neither (G was Constrain and Snap).
    {
        w.setMode(c2d::MainWindow::Mode::Design, false);
        QMap<QString, QStringList> byKey;
        for (QAction *a : w.findChildren<QAction *>())
            for (const QKeySequence &key : a->shortcuts())
                if (!key.isEmpty())
                    byKey[key.toString()] << a->text();
        for (auto it = byKey.cbegin(); it != byKey.cend(); ++it)
            if (it.value().size() > 1)
                std::fprintf(stderr, "duplicate shortcut %s: %s\n", qPrintable(it.key()),
                             qPrintable(it.value().join(QStringLiteral(", "))));
        bool unique = true;
        for (const QStringList &acts : byKey)
            unique = unique && acts.size() == 1;
        check(unique, "shortcuts: no key on two actions");
        QAction *snap = nullptr;
        for (QAction *a : w.findChildren<QAction *>())
            if (a->text() == QLatin1String("Snap"))
                snap = a;
        check(snap, "shortcuts: Snap action");
        w.activateWindow();
        w.m_canvas->setFocus();
        QApplication::processEvents();
        const bool snapOn = snap->isChecked();
        QTest::keyClick(w.m_canvas, Qt::Key_G);
        check(w.m_canvas->m_tool == c2d::Canvas::Constrain && snap->isChecked() == snapOn,
              "shortcuts: G picks Constrain");
        QTest::keyClick(w.m_canvas, Qt::Key_G, Qt::ShiftModifier);
        check(snap->isChecked() != snapOn, "shortcuts: Shift+G toggles Snap");
        check(snap->toolTip().contains(snap->shortcut().toString(QKeySequence::NativeText)),
              "shortcuts: Snap's tooltip names its key");
        snap->setChecked(snapOn);
    }

    // A number typed in a toolpath field but not yet committed (no Enter, no
    // focus change) reaches the document before save, export or close read it.
    {
        w.setMode(c2d::MainWindow::Mode::Toolpaths, false);
        w.m_tp->m_list->setCurrentItem(w.m_tp->m_list->topLevelItem(0));
        QApplication::processEvents();
        auto *spin = qobject_cast<QDoubleSpinBox *>(w.m_tp->m_form->m_editors.value("end_depth"));
        check(spin, "typed field: end depth editor");
        w.activateWindow();
        spin->setFocus();
        QApplication::processEvents();
        QTest::keyClick(spin, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClicks(spin, "12");
        w.commitEdits();
        const double depth = qAbs(d.toolpathByUuid(p.uuid)->json.value("end_depth").toVariant().toDouble());
        std::fprintf(stderr, "typed field: end depth %g\n", depth);
        check(qAbs(depth - 12) < 1e-6, "typed field: committed before the file action");
        check(spin->hasFocus() || !w.isActiveWindow(), "typed field: focus given back");
    }

    // The Area tool on a big shape with a small notch overlapping its corner
    // picks the shape minus the notch (added as a closed path), not the whole
    // shape: the notch is far more than a sliver along the outline.
    {
        w.setMode(c2d::MainWindow::Mode::Design, false);
        const c2d::Element t = c2d::Element::makeRectangle({1000, 1000}, 600, 400, d.defaultLayer());
        const QRectF tb = t.painterPath.boundingRect();
        c2d::Element n = c2d::Element::makeRectangle({0, 0}, 20, 20, d.defaultLayer());
        const QPointF shift = tb.bottomRight() - n.painterPath.boundingRect().center();
        n = c2d::Element::makeRectangle(QPointF(0, 0) + shift, 20, 20, d.defaultLayer());
        d.addElement(t);
        d.addElement(n);
        w.m_canvas->setDocument(&d);
        QApplication::processEvents();
        const int before = int(d.elements().size());
        QGraphicsItem *ti = w.m_canvas->itemFor(t.id);
        check(ti, "area notch: the table is drawn");
        w.m_canvas->selectAreaAt(ti->sceneBoundingRect().center(), false);
        check(int(d.elements().size()) == before + 1, "area notch: the face is added, not the whole shape taken");
    }
    // Smaller notches too (a few mm on an edge or corner), and a plain shape
    // is still the whole shape.
    {
        struct Case { const char *name; QPointF at; bool circle; double size; bool notched; };
        const Case cases[] = {
            {"8 mm square on an edge", {300, 200}, false, 8, true},
            {"12 mm square on a corner", {600, 400}, false, 12, true},
            {"10 mm circle on an edge", {300, 200}, true, 10, true},
            {"no notch", {0, 0}, false, 0, false},
        };
        double y = 3000;
        for (const Case &c : cases) {
            const c2d::Element t = c2d::Element::makeRectangle({1300, y + 200}, 600, 400, d.defaultLayer());
            d.addElement(t);
            if (c.notched) {
                // c.at is from the table's lower-left; an edge notch sits on y = 0.
                const QPointF ctr(1000 + c.at.x(), y + (c.at.y() == 200 ? 0 : c.at.y()));
                d.addElement(c.circle ? c2d::Element::makeCircle(ctr, c.size / 2, d.defaultLayer())
                                      : c2d::Element::makeRectangle(ctr, c.size, c.size, d.defaultLayer()));
            }
            w.m_canvas->setDocument(&d);
            QApplication::processEvents();
            const int before = int(d.elements().size());
            QGraphicsItem *ti = w.m_canvas->itemFor(t.id);
            check(ti, "area notch: the table is drawn");
            w.m_canvas->selectAreaAt(ti->sceneBoundingRect().center(), false);
            std::fprintf(stderr, "area notch %s: %d -> %d\n", c.name, before, int(d.elements().size()));
            check(int(d.elements().size()) == before + (c.notched ? 1 : 0), c.name);
            y += 1000;
        }
    }

    // A Send-to dialog sends the design it was opened for: a different one
    // opened under it meanwhile is refused, not sent under the old name.
    {
        QString err;
        check(!w.sendCopy(w.m_doc.filePath(), &err).isEmpty(), "send copy: the same design is copied");
        check(w.sendCopy(QStringLiteral("/elsewhere/Alpha.c2d"), &err).isEmpty() && !err.isEmpty(),
              "send copy: another design is refused");
        // The same file through a symlinked folder is the same design.
        const QFileInfo fi(w.m_doc.filePath());
        QTemporaryDir links;
        if (!fi.filePath().isEmpty() && QFile::link(fi.absolutePath(), links.filePath("via"))) {
            const QString viaLink = links.filePath("via/") + fi.fileName();
            check(!w.sendCopy(viaLink, &err).isEmpty(), "send copy: the same file by another path");
        }
    }

    // The window stays open while an update step's process runs.
    {
        w.m_updateStep = std::make_shared<bool>(true);
        check(!w.close() && w.isVisible(), "update step: close refused");
        w.m_updateStep.reset();
    }

    // A preview run that ends with Preview off leaves no "Updating
    // preview…" behind: nothing else would replace it.
    {
        const bool was = w.m_previewAct->isChecked();
        w.m_previewAct->setChecked(false);
        ++w.m_previewGen;
        w.startPreviewRun();
        QTest::qWaitFor([&] { return !w.m_previewRun; }, 30000);
        QApplication::processEvents();
        check(w.statusBar()->currentMessage() != QStringLiteral("Updating preview…"),
              "preview: the status line is cleared when a run ends with Preview off");
        w.m_previewAct->setChecked(was);
    }

    // Export G-code is offered beside the file an untitled design came
    // from, named after it - not after the cache's scratch copy.
    {
        const bool wasUntitled = w.m_untitled;
        const QString wasFrom = w.m_untitledFrom;
        w.m_untitled = true;
        w.m_untitledFrom = QStringLiteral("/tmp/parts/part.dxf");
        check(w.gcodeSuggestion() == QLatin1String("/tmp/parts/part.nc"), "export: named after the imported file");
        // Export DXF never offers the DXF the design was imported from: one
        // Yes to "replace?" wrote over the original drawing.
        check(w.dxfSuggestion() == QLatin1String("/tmp/parts/part-export.dxf"),
              "export DXF: not the imported file itself");
        {
            QTemporaryDir dxfDir;
            const QString svg = dxfDir.filePath("logo.svg"), dxf = dxfDir.filePath("logo.dxf");
            w.m_untitledFrom = svg;
            check(w.dxfSuggestion() == dxf, "export DXF: beside an imported SVG, its name");
            QFile f(dxf);
            check(f.open(QIODevice::WriteOnly) && f.write("0\nEOF\n") > 0, "export DXF: a DXF already there");
            f.close();
            check(w.dxfSuggestion() == dxfDir.filePath("logo-export.dxf"), "export DXF: not over a file already there");
            QFile f2(dxfDir.filePath("logo-export.dxf"));
            check(f2.open(QIODevice::WriteOnly) && f2.write("0\nEOF\n") > 0, "export DXF: an earlier export there");
            f2.close();
            check(w.dxfSuggestion() == dxfDir.filePath("logo-export-2.dxf"), "export DXF: not over an earlier export");
        }
        w.m_untitled = wasUntitled;
        w.m_untitledFrom = wasFrom;
    }

    // On the shop screen (1280 px) every Options button stays in sight in
    // both modes: the bar takes a row of its own rather than dropping Fit,
    // Rotate 90°, Preview and Undo off its end.
    {
        const QSize was = w.size();
        const c2d::MainWindow::Mode mode = w.mode();
        w.resize(1280, 800);
        for (c2d::MainWindow::Mode m : {c2d::MainWindow::Mode::Toolpaths, c2d::MainWindow::Mode::Design}) {
            w.setMode(m, false);
            QApplication::processEvents();
            QTest::qWait(50);
            bool all = true;
            for (QAction *a : w.m_optionsBar->actions())
                if (a->isVisible() && !a->isSeparator()) {
                    QWidget *b = w.m_optionsBar->widgetForAction(a);
                    all = all && b && b->isVisible();
                }
            check(all, "1280 px: every Options button is shown");
        }
        w.resize(was);
        w.setMode(mode, false);
    }

    // Narrower still (a pad): a bar that cannot show all its buttons shows a
    // » big enough to see and press, and pressing it shows the rest.
    {
        const QSize was = w.size();
        const c2d::MainWindow::Mode mode = w.mode();
        w.resize(900, 700);
        w.setMode(c2d::MainWindow::Mode::Toolpaths, false);
        QApplication::processEvents();
        QTest::qWait(50);
        QToolBar *kinds = w.m_tpBar;
        QToolButton *more = kinds->findChild<QToolButton *>(QStringLiteral("qt_toolbar_ext_button"));
        int hidden = 0;
        for (QAction *a : kinds->actions())
            if (a->isVisible() && !a->isSeparator() && kinds->widgetForAction(a)
                && !kinds->widgetForAction(a)->isVisible())
                ++hidden;
        check(hidden > 0, "900 px: the toolpath kinds do not all fit");
        check(more && more->isVisible() && more->width() >= 30, "900 px: their » shows, big enough to press");
        // » opens a menu of exactly the kinds out of sight.
        QStringList inMenu, outOfSight;
        for (QAction *a : kinds->actions())
            if (a->isVisible() && !a->isSeparator() && kinds->widgetForAction(a)
                && !kinds->widgetForAction(a)->isVisible())
                outOfSight << a->text();
        QTimer::singleShot(100, [&] {
            if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
                for (QAction *a : menu->actions())
                    if (!a->isSeparator())
                        inMenu << a->text();
                if (qEnvironmentVariableIsSet("PHOBIC_TEST_GRAB")) {
                    QPixmap shot = w.grab();
                    QPainter p(&shot);
                    p.drawPixmap(w.mapFromGlobal(menu->mapToGlobal(QPoint(0, 0))), menu->grab());
                    p.end();
                    shot.save(qEnvironmentVariable("PHOBIC_TEST_GRAB"));
                }
                menu->close();
            }
        });
        more->click();
        check(!inMenu.isEmpty() && inMenu == outOfSight, "900 px: pressing » lists every kind out of sight");
        w.resize(was);
        w.setMode(mode, false);
    }

    // Rotate 90° (options bar and Edit menu): the design turns, the stock
    // does not, and one Ctrl+Z puts it back.
    {
        QAction *rot = w.findChild<QAction *>(QStringLiteral("rotateJob"));
        check(rot != nullptr, "rotate: the action is on the window");
        const QVector<c2d::Element> before = d.elements();
        const QVector<c2d::Toolpath> tpsBefore = d.toolpaths();
        const double sw = d.boardWidth(), sh = d.boardHeight();
        const int undoAt = w.m_canvas->undoStack()->index();
        rot->trigger();
        const c2d::Element *rr = d.elementById(r.id);
        check(rr && rr->raw.value("width").toDouble() == 30 && rr->raw.value("height").toDouble() == 40,
              "rotate: the rectangle lies the other way");
        // The design here does not lie on the stock, so the stock only turns
        // if that makes it lie the same way as the design (it may stay).
        const QRectF nb = c2d::jobrotate::designBounds(d);
        check((nb.width() > nb.height()) == (d.boardWidth() > d.boardHeight())
                  || d.boardWidth() == d.boardHeight(),
              "rotate: the stock lies the same way as the turned design");
        check(d.toolpaths().size() == tpsBefore.size() && d.toolpaths().at(0).json.value("elements") == tpsBefore.at(0).json.value("elements"),
              "rotate: the toolpaths still machine the same vectors");
        check(w.m_canvas->undoStack()->index() == undoAt + 1, "rotate: one undo step");
        check(w.m_dirty, "rotate: the design is marked edited");
        w.m_canvas->undoStack()->undo();
        bool same = d.elements().size() == before.size();
        for (int i = 0; same && i < before.size(); ++i)
            same = d.elements().at(i).raw == before.at(i).raw;
        check(same, "rotate: Ctrl+Z puts every vector back");
        check(d.boardWidth() == sw && d.boardHeight() == sh, "rotate: and the stock");
        w.m_canvas->undoStack()->redo();
        check(d.elementById(r.id)->raw.value("width").toDouble() == 30, "rotate: and Ctrl+Y turns it again");
        w.m_canvas->undoStack()->undo();
    }

    // Rotate 90° turns the 3D model and the background as they are when it
    // is done or undone, not snapshots of them: a texture added or the
    // background moved after the turn was lost again by Ctrl+Z.
    {
        QUndoStack *undo = w.m_canvas->undoStack();
        QAction *rot = w.findChild<QAction *>(QStringLiteral("rotateJob"));
        c2d::Model3D m;
        c2d::ModelComponent tex;
        tex.kind = c2d::ModelComponent::Texture;
        tex.id = c2d::Model3D::newId();
        tex.x = 12.5;
        tex.y = 20.25;
        m.components = {tex};
        w.m_model->setModel(m);
        w.m_bg.image = QImage(40, 20, QImage::Format_RGB32);
        w.m_bg.image.fill(Qt::gray);
        w.m_bg.x = 30;
        w.m_bg.y = 40;
        w.m_bg.rotationDeg = 0;
        w.m_bg.userChanged = false;
        const QPointF centre0 = w.m_bg.rectMm().center();
        auto compAt = [&](int i) { return w.m_model->model().components.at(i); };
        rot->trigger();
        check(compAt(0).turns == 1 && w.m_bg.rotationDeg == -90, "rotate: the model and the background turn");
        undo->undo();
        check(compAt(0).x == 12.5 && compAt(0).y == 20.25 && compAt(0).turns == 0,
              "rotate undo: an untouched component is exactly where it was");
        check(w.m_bg.x == 30 && w.m_bg.y == 40 && w.m_bg.rotationDeg == 0 && !w.m_bg.userChanged,
              "rotate undo: an untouched background is exactly as it was");
        // Resized after the turn but not moved: not "untouched", so it turns
        // back as it is instead of taking its old corner with its new size.
        undo->redo();
        w.m_bg.scale = 2;
        undo->undo();
        check(qAbs(w.m_bg.rotationDeg) < 1e-9 && !(w.m_bg.x == 30 && w.m_bg.y == 40),
              "rotate undo: a background resized after the turn turns back, not to its old corner");
        w.m_bg.x = 30;
        w.m_bg.y = 40;
        w.m_bg.scale = 1;
        w.m_bg.userChanged = false;
        undo->redo();
        // Edits after the turn: one more texture, the background moved 5 mm.
        c2d::Model3D m2 = w.m_model->model();
        c2d::ModelComponent tex2 = tex;
        tex2.id = c2d::Model3D::newId();
        tex2.x = 50;
        tex2.y = 60;
        tex2.turns = 1;
        m2.components.append(tex2);
        w.m_model->setModel(m2);
        w.m_bg.x += 5;
        const c2d::BackgroundImage moved = w.m_bg;
        undo->undo();
        check(w.m_model->model().components.size() == 2, "rotate undo: a component added after the turn stays");
        check(compAt(1).turns == 0, "rotate undo: and turns back with the design");
        check(compAt(0).x == 12.5 && compAt(0).y == 20.25 && compAt(0).turns == 0,
              "rotate undo: the untouched one is still exact");
        check(qAbs(w.m_bg.rotationDeg) < 1e-9
                  && qAbs(QLineF(w.m_bg.rectMm().center(), centre0).length() - 5) < 1e-6,
              "rotate undo: the moved background turns back, keeping its move");
        undo->redo();
        check(w.m_bg.x == moved.x && w.m_bg.y == moved.y && w.m_bg.rotationDeg == moved.rotationDeg,
              "rotate redo: the moved background is exactly where it was moved to");
        check(compAt(1).x == 50 && compAt(1).y == 60 && compAt(1).turns == 1,
              "rotate redo: and so is the added component");
        undo->undo();
        w.m_bg = c2d::BackgroundImage();
        w.m_model->setModel(c2d::Model3D());
    }

    // Toolpaths mode picks vectors: a drawing tool left on from Design (its
    // bar is hidden there) trimmed or drew at a tap instead. Back to Select.
    {
        const c2d::MainWindow::Mode mode = w.mode();
        w.setMode(c2d::MainWindow::Mode::Design, false);
        QAction *trim = w.m_palette->findChild<QAction *>(QStringLiteral("trim"));
        if (!trim)
            for (QAction *a : w.m_palette->actions())
                if (a->objectName() == QLatin1String("trim"))
                    trim = a;
        check(trim != nullptr, "tools: the Trim action");
        trim->trigger();
        check(w.m_canvas->tool() == c2d::Canvas::Trim, "tools: Trim is on in Design");
        w.setMode(c2d::MainWindow::Mode::Toolpaths, false);
        check(w.m_canvas->tool() == c2d::Canvas::Select && w.m_selectAct->isChecked(),
              "tools: Toolpaths mode is back on Select, its button ticked");
        w.m_canvas->setTool(c2d::Canvas::Pan);
        w.setMode(c2d::MainWindow::Mode::Design, false);
        w.setMode(c2d::MainWindow::Mode::Toolpaths, false);
        check(w.m_canvas->tool() == c2d::Canvas::Pan, "tools: the hand (on the Toolpaths bar too) stays");
        w.m_selectAct->trigger();
        w.setMode(mode, false);
    }

    // A toolpath row let go of (a tap on the board) stays let go of: every
    // edit rebuilt the list and selected the row again, halo and all.
    {
        w.m_canvas->m_scene->clearSelection();
        QTreeWidget *list = w.m_tp->m_list;
        check(list->topLevelItemCount() >= 2, "toolpath list: two rows");
        list->setCurrentItem(list->topLevelItem(1));
        const QString uuid = list->currentItem()->data(0, Qt::UserRole).toString();
        check(!list->selectedItems().isEmpty() && !w.m_canvas->m_highlightIds.isEmpty(),
              "toolpath list: a picked row is selected and its shapes outlined");
        emit w.m_canvas->documentChanged();
        check(!list->selectedItems().isEmpty() && list->currentItem() == list->topLevelItem(1),
              "toolpath list: an edit keeps a selected row selected");
        emit w.m_canvas->emptyPressed();
        check(list->selectedItems().isEmpty() && w.m_canvas->m_highlightIds.isEmpty(),
              "toolpath list: a tap on the board lets go of it");
        emit w.m_canvas->documentChanged();
        check(list->selectedItems().isEmpty() && w.m_canvas->m_highlightIds.isEmpty(),
              "toolpath list: an edit does not select it again");
        check(list->currentItem() == list->topLevelItem(1)
                  && list->currentItem()->data(0, Qt::UserRole).toString() == uuid,
              "toolpath list: it stays the current row");
        check(QTest::qWaitFor([&] { return settled(w); }, 30000), "toolpath list: the preview settles");
    }

    // The edited mark follows undo: a design undone (or redone) back to how
    // it was saved is not edited any more - Save, an edit and Ctrl+Z left the
    // * in the title and closing asked about changes that were gone. It may
    // only ever go when what is in memory is what is in the file.
    {
        QTemporaryDir dir;
        QString err;
        const QString path = dir.filePath("mark.c2d"), other = dir.filePath("other.c2d");
        check(c2d::Document::createBlank(path, 300, 200, 19, &err), "mark: a blank file");
        QString rid, cid, tid;
        {
            c2d::Document seed;
            check(seed.load(path, &err), "mark: the blank loads");
            const c2d::Element sr = c2d::Element::makeRectangle({80, 80}, 40, 30, seed.defaultLayer());
            const c2d::Element sc = c2d::Element::makeCircle({200, 100}, 15, seed.defaultLayer());
            seed.addElement(sr);
            seed.addElement(sc);
            c2d::Toolpath st = c2d::makeToolpath(seed, "contour", {sc.id});
            st.json.insert("name", "Ring");
            seed.insertToolpath(0, st);
            check(seed.save(path, &err), "mark: the design is written");
            rid = sr.id;
            cid = sc.id;
            tid = st.uuid;
        }
        check(QFile::copy(path, other), "mark: a second design");
        w.m_dirty = false;                    // the design of the checks above is let go
        w.openPath(path, false);
        QApplication::processEvents();
        QUndoStack *undo = w.m_canvas->undoStack();
        check(QFileInfo(d.filePath()) == QFileInfo(path) && d.elementById(rid) && d.elementById(cid)
                  && d.toolpathByUuid(tid),
              "mark: the design is open");
        auto starred = [&] { return w.windowTitle().contains(QStringLiteral(" *")); };
        auto xOf = [&](const QString &id) { return d.elementById(id)->painterPath.boundingRect().center().x(); };
        // What a fresh load of the file holds against what is in the window:
        // the one thing a design shown as saved must never differ in.
        auto inFile = [&] {
            c2d::Document f;
            QString e;
            if (!f.load(d.filePath(), &e) || f.elements().size() != d.elements().size()
                || f.toolpaths().size() != d.toolpaths().size())
                return false;
            for (int i = 0; i < d.elements().size(); ++i)
                if (f.elements().at(i).raw != d.elements().at(i).raw)
                    return false;
            for (int i = 0; i < d.toolpaths().size(); ++i)
                if (f.toolpaths().at(i).json != d.toolpaths().at(i).json)
                    return false;
            return f.boardWidth() == d.boardWidth() && f.boardHeight() == d.boardHeight();
        };
        // One user action at a time: the event loop runs between them.
        auto step = [] { QApplication::processEvents(); };
        auto clean = [&] { return !w.m_dirty && !starred() && inFile(); };
        auto edited = [&] { return w.m_dirty && starred(); };
        const double x0 = xOf(rid);
        check(clean() && undo->count() == 0, "mark: a design just opened is not edited");

        // An edit and Ctrl+Z, Ctrl+Y and Ctrl+Z again.
        w.m_canvas->moveElementBy(rid, 10, 0);
        step();
        check(edited() && !inFile(), "mark: an edit marks the design");
        undo->undo();
        step();
        check(clean() && xOf(rid) == x0, "mark: undone back to the design as opened, it is not edited");
        check(w.confirmDiscard(QStringLiteral("unused")), "mark: and nothing is asked before it is let go");
        undo->redo();
        step();
        check(edited(), "mark: redo is the edit again");
        undo->undo();
        step();
        check(clean(), "mark: and undo takes it back once more");

        // Edit, Save, edit: the saved point is the first edit now.
        w.m_canvas->moveElementBy(rid, 10, 0);
        step();
        w.onSave();
        step();
        check(clean() && xOf(rid) == x0 + 10, "mark: saved");
        w.m_canvas->moveElementBy(rid, 10, 0);
        step();
        check(edited(), "mark: an edit after the save");
        undo->undo();
        step();
        check(clean() && xOf(rid) == x0 + 10, "mark: one undo is back at the save");
        undo->undo();
        step();
        check(edited() && !inFile() && xOf(rid) == x0, "mark: a second undo goes behind the save: edited");
        undo->redo();
        step();
        check(clean(), "mark: redo up to the save is clean again");
        undo->redo();
        step();
        check(edited(), "mark: and past it edited");
        undo->undo();
        step();
        check(clean(), "mark: back at the save");

        // Undo behind the save, then a new edit: the saved state is gone from
        // the stack, and no undo or redo is the file any more.
        undo->undo();
        step();
        w.m_canvas->moveElementBy(cid, 0, 5);
        step();
        check(edited() && !undo->canRedo(), "mark: a new edit behind the save");
        undo->undo();
        step();
        check(edited() && !inFile(), "mark: its undo is not the saved design");
        undo->redo();
        step();
        check(edited() && !inFile(), "mark: nor its redo");
        w.onSave();
        step();
        check(clean(), "mark: until it is saved");

        // Every kind of undo step leads back: a toolpath edit, a step of
        // several commands (construction), Rotate 90°.
        {
            QJsonObject j = d.toolpathByUuid(tid)->json;
            j.insert("name", "Renamed");
            w.m_canvas->editToolpath(tid, j);
            step();
            check(edited() && !inFile(), "mark: a toolpath edit marks");
            undo->undo();
            step();
            check(clean(), "mark: and its undo is the saved design");
            const int at = undo->index();
            w.m_canvas->toggleConstruction({rid, cid});
            step();
            check(edited() && undo->index() == at + 1, "mark: two vectors made construction, one step");
            undo->undo();
            step();
            check(clean(), "mark: undone, the saved design");
            w.findChild<QAction *>(QStringLiteral("rotateJob"))->trigger();
            step();
            check(edited() && !inFile(), "mark: Rotate 90° marks");
            undo->undo();
            step();
            check(clean(), "mark: undone, the saved design");
            // Saved turned: now the turned design is the clean one.
            undo->redo();
            step();
            w.onSave();
            step();
            check(clean(), "mark: the turned design saved");
            undo->undo();
            step();
            check(edited() && !inFile(), "mark: turned back is an edit of it");
            undo->redo();
            step();
            check(clean(), "mark: turned again is the file");
        }

        // A change no undo step made (here announced the way the Background
        // menu announces its own) is not taken back by Ctrl+Z: the mark
        // stays, whatever is undone and redone, until the next save.
        emit w.m_canvas->documentChanged();
        step();
        check(edited(), "mark: a change past the undo stack marks");
        w.m_canvas->moveElementBy(rid, 10, 0);
        step();
        undo->undo();
        step();
        check(edited(), "mark: an undo after it does not unmark");
        undo->undo();
        step();
        undo->redo();
        step();
        check(edited(), "mark: nor does coming back to where the save was");
        w.onSave();
        step();
        check(clean(), "mark: the save does");
        w.m_canvas->moveElementBy(rid, 10, 0);
        step();
        undo->undo();
        step();
        check(clean(), "mark: and from then on undo leads back again");

        // The background is saved with the design but moved past the undo
        // stack. Moved and then, before the event loop is back, an edit and
        // its undo: the stack is where it was saved, the background is not.
        w.m_bg.x += 5;
        emit w.m_canvas->documentChanged();
        w.m_canvas->moveElementBy(rid, 10, 0);
        undo->undo();
        step();
        check(edited(), "mark: a moved background keeps the mark through an undo");
        w.m_bg.x -= 5;
        w.onSave();
        step();
        check(clean(), "mark: saved again");

        // The 3D model is no undo step either.
        emit w.m_model->modelChanged();
        step();
        check(edited(), "mark: a model change marks");
        w.m_canvas->moveElementBy(rid, 10, 0);
        step();
        undo->undo();
        step();
        check(edited(), "mark: and an undo after it does not unmark");
        w.onSave();
        step();
        check(clean(), "mark: saved again");

        // An undo stack emptied under an edited design says nothing about
        // the file, though an empty stack counts as clean.
        w.m_canvas->moveElementBy(rid, 10, 0);
        step();
        w.m_canvas->setDocument(&d);
        step();
        check(edited() && !inFile() && undo->count() == 0, "mark: an emptied undo stack leaves the mark");
        w.m_canvas->moveElementBy(cid, 0, 5);
        step();
        undo->undo();
        step();
        check(edited() && !inFile(), "mark: and undoing back to its start is not the file");
        w.onSave();
        step();
        check(clean(), "mark: saved again");

        // A save in the middle of a step of several commands: the stack takes
        // no clean mark there, so the one it has stands for an older file.
        {
            const double xs = xOf(rid);
            undo->beginMacro(QStringLiteral("two moves"));
            w.m_canvas->moveElementBy(rid, 10, 0);
            w.onSave();
            w.m_canvas->moveElementBy(rid, 10, 0);
            undo->endMacro();
            step();
            check(edited(), "mark: a step saved half way is an edit");
            undo->undo();
            step();
            check(xOf(rid) == xs && edited() && !inFile(),
                  "mark: undone, it is not the file, which holds the first half");
            w.onSave();
            step();
            check(clean(), "mark: saved again");
        }

        // Another design opened over unsaved edits (Discard): the new one
        // starts clean, with a clean mark of its own.
        {
            w.m_canvas->moveElementBy(rid, 10, 0);
            step();
            check(edited(), "mark: edited before the open");
            int asked = 0;
            QTimer answer;
            answer.setInterval(20);
            QObject::connect(&answer, &QTimer::timeout, [&] {
                if (auto *mb = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                    ++asked;
                    mb->button(QMessageBox::Discard)->click();
                }
            });
            answer.start();
            w.openPath(other, false);
            answer.stop();
            step();
            check(asked == 1 && QFileInfo(d.filePath()) == QFileInfo(other), "mark: the other design is open");
            check(clean() && undo->count() == 0, "mark: and is not edited");
            w.m_canvas->moveElementBy(rid, 10, 0);
            step();
            check(edited(), "mark: an edit of it marks");
            undo->undo();
            step();
            check(clean() && xOf(rid) == x0, "mark: and its undo is the design as opened");
        }

        // Rotate 90° on a design with no stock size writes none: it added an
        // empty width and height to the design on every turn and undo.
        {
            d.m_params.remove(QStringLiteral("width"));
            d.m_params.remove(QStringLiteral("height"));
            auto noStock = [&] {
                return !d.params().contains(QStringLiteral("width")) && !d.params().contains(QStringLiteral("height"));
            };
            const int at = undo->index();
            w.findChild<QAction *>(QStringLiteral("rotateJob"))->trigger();
            check(undo->index() == at + 1 && noStock(), "rotate: no stock size, none written");
            undo->undo();
            check(noStock(), "rotate: nor by its undo");
            undo->redo();
            check(noStock(), "rotate: nor by its redo");
            undo->undo();
            step();
        }

        // An untitled design: the first step undone is the blank again.
        w.m_dirty = false;
        check(w.newUntitled(300, 200, 19), "mark: an untitled design");
        step();
        check(!w.m_dirty && !starred(), "mark: starts not edited");
        const c2d::Element ue = c2d::Element::makeCircle({50, 50}, 10, d.defaultLayer());
        w.m_canvas->insertGenerated({ue}, c2d::makeToolpath(d, "contour", {ue.id}));
        step();
        check(w.m_dirty && starred() && d.elements().size() == 1, "mark: its first step marks");
        undo->undo();
        step();
        check(!w.m_dirty && !starred() && d.elements().isEmpty() && inFile(),
              "mark: undone, it is the blank design again");
    }

    w.m_dirty = false;
    std::printf("test_mainwindow: %d checks OK\n", g_checks);
    return 0;
}
