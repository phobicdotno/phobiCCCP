#include "exportprogress.h"

#include "c2ddocument.h"
#include "heightmodel.h"

#include <QApplication>
#include <QEventLoop>
#include <QProgressDialog>
#include <QThread>
#include <QTimer>

#include <atomic>

namespace c2d {
namespace {

// What the worker publishes and the dialog reads. Only atomics cross the
// thread boundary; the stage name is deliberately not shared.
struct Shared {
    std::atomic<bool> cancel{false};
    std::atomic<int> done{0};
    std::atomic<int> total{0};
};

// Drives the dialog while `body` runs on a worker. Returns when the worker
// finishes; the dialog's Cancel only sets the flag, so the export always stops
// at a safe point of its own choosing rather than being killed.
void runBehindDialog(QWidget *parent, const QString &title, Shared &sh,
                     std::function<void()> body)
{
    QProgressDialog dlg(QStringLiteral("Preparing toolpaths…"),
                        QStringLiteral("Cancel"), 0, 0, parent);
    dlg.setWindowTitle(title);
    dlg.setWindowModality(Qt::WindowModal);
    dlg.setMinimumDuration(300);      // a fast export must not flash a dialog
    dlg.setAutoClose(false);
    dlg.setAutoReset(false);
    QObject::connect(&dlg, &QProgressDialog::canceled, &dlg,
                     [&sh] { sh.cancel.store(true); });

    QThread *t = QThread::create(std::move(body));
    QEventLoop loop;
    // The worker can finish before exec() is even entered (a small document
    // exports in microseconds). The connection is queued -- the QEventLoop
    // belongs to this thread and `finished` is emitted on the worker -- so the
    // quit is delivered as an event rather than lost, but loop on the flag
    // anyway so a stray quit from anything else cannot end the wait early.
    bool done = false;
    QObject::connect(t, &QThread::finished, &loop, [&done, &loop] {
        done = true;
        loop.quit();
    });

    QTimer tick;
    QObject::connect(&tick, &QTimer::timeout, &dlg, [&dlg, &sh] {
        const int total = sh.total.load();
        if (total > 0) {
            dlg.setMaximum(total);
            dlg.setValue(qMin(sh.done.load(), total));
        }
    });
    tick.start(100);

    t->start();
    while (!done)
        loop.exec();      // the window stays alive; only Cancel is reachable
    t->wait();
    delete t;
    dlg.close();
}

// The watch handed to the export. Runs on the worker, so it touches nothing
// but the atomics.
ExportWatch watchFor(Shared &sh)
{
    ExportWatch w;
    w.step = [&sh](int done, int total, const QString &) {
        sh.done.store(done);
        sh.total.store(total);
        return !sh.cancel.load();
    };
    return w;
}

} // namespace

GcodeResult exportWithProgress(QWidget *parent, Document &doc, const QString &title)
{
    // Composite the relief here, on the thread that owns it, and take a copy.
    // heightModelFor() rebuilds and caches into the model store when it is
    // dirty, and the model panel writes that same store from its own rebuild,
    // so the worker must never call it. The copy is passed even when it holds
    // no model: that keeps the export from falling back to the provider.
    const HeightModel *live = heightModelFor(doc);
    const HeightModel relief = live ? *live : HeightModel();
    Document snapshot = doc;

    Shared sh;
    const ExportWatch watch = watchFor(sh);
    GcodeResult out;
    runBehindDialog(parent, title, sh, [&] {
        out = exportGcode(snapshot, &watch, &relief);
    });
    if (sh.cancel.load())
        out.cancelled = true;
    return out;
}

TiledExport exportTiledWithProgress(QWidget *parent, Document &doc, const QString &outBase,
                                    double tileHeight, const QString &title)
{
    const HeightModel *live = heightModelFor(doc);
    const HeightModel relief = live ? *live : HeightModel();
    Document snapshot = doc;

    Shared sh;
    const ExportWatch watch = watchFor(sh);
    TiledExport out;
    runBehindDialog(parent, title, sh, [&] {
        out = exportTiled(snapshot, outBase, tileHeight, &watch, &relief);
    });
    if (sh.cancel.load() && out.error.isEmpty())
        out.error = QStringLiteral("cancelled");
    return out;
}

} // namespace c2d
