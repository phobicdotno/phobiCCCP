// The main window's Preview: edits schedule the export on a worker instead of
// running it on the interface thread, a result for an older edit is dropped,
// and a synchronous rebuild (Simulate) serves what is pending. Also: the
// window does not close while an update step runs.
//
// Plain asserts, no framework; exits 0 on success.

#define private public      // the preview's state is private to the window
#include "../src/mainwindow.h"
#include "../src/canvas.h"
#undef private
#include "../src/c2ddocument.h"
#include "../src/element.h"
#include "../src/post_grbl.h"
#include "../src/toolpathfactory.h"

#include <QAction>
#include <QApplication>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTest>
#include <QTimer>

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

    // The window stays open while an update step's process runs.
    {
        w.m_updateStep = std::make_shared<bool>(true);
        check(!w.close() && w.isVisible(), "update step: close refused");
        w.m_updateStep.reset();
    }

    w.m_dirty = false;
    std::printf("test_mainwindow: %d checks OK\n", g_checks);
    return 0;
}
