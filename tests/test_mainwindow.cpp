// The main window's Preview: edits schedule the export on a worker instead of
// running it on the interface thread, a result for an older edit is dropped,
// and a synchronous rebuild (Simulate) serves what is pending. Also: the
// window does not close while an update step runs. Simulate with Preview off
// rebuilds after an edit, never inside it, and no key is bound twice.
//
// Plain asserts, no framework; exits 0 on success.

#define private public      // the preview's state is private to the window
#include "../src/mainwindow.h"
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
        check(d.boardWidth() == sw && d.boardHeight() == sh, "rotate: the stock stays as it is");
        check(d.toolpaths().size() == tpsBefore.size() && d.toolpaths().at(0).json.value("elements") == tpsBefore.at(0).json.value("elements"),
              "rotate: the toolpaths still machine the same vectors");
        check(w.m_canvas->undoStack()->index() == undoAt + 1, "rotate: one undo step");
        check(w.m_dirty, "rotate: the design is marked edited");
        w.m_canvas->undoStack()->undo();
        bool same = d.elements().size() == before.size();
        for (int i = 0; same && i < before.size(); ++i)
            same = d.elements().at(i).raw == before.at(i).raw;
        check(same, "rotate: Ctrl+Z puts every vector back");
        w.m_canvas->undoStack()->redo();
        check(d.elementById(r.id)->raw.value("width").toDouble() == 30, "rotate: and Ctrl+Y turns it again");
        w.m_canvas->undoStack()->undo();
    }

    w.m_dirty = false;
    std::printf("test_mainwindow: %d checks OK\n", g_checks);
    return 0;
}
