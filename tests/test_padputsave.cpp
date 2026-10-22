// r13 ui: a pad's put queued when Ctrl+S (commitEdits) runs with a number typed in the toolpath form.
#define private public
#define protected public
#include "../src/mainwindow.h"
#include "../src/canvas.h"
#include "../src/toolpathform.h"
#include "../src/toolpathpanel.h"
#include "../src/propertiespanel.h"
#undef private
#undef protected
#include "../src/c2ddocument.h"
#include "../src/padlink.h"
#include <QApplication>
#include <QDoubleSpinBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <cstdio>
using namespace c2d;
static double feed(const QJsonObject &j) { return j.value("speeds").toObject().value("feedrate").toDouble(); }
int main(int argc, char *argv[])
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir home;
    qputenv("HOME", home.path().toLocal8Bit());
    qunsetenv("XDG_DOCUMENTS_DIR"); qunsetenv("XDG_DATA_HOME"); qunsetenv("XDG_CONFIG_HOME"); qunsetenv("XDG_CACHE_HOME");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("phobicccp-test");
    QCoreApplication::setApplicationName("test_padputsave");
    const QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QFile f(QStringLiteral(PHOBICCCP_SOURCE_DIR "/tests/data/pad-design.json"));
    f.open(QIODevice::ReadOnly);
    const QJsonObject fixture = QJsonDocument::fromJson(f.readAll()).object();
    QDir().mkpath(docs + "/work");
    const QString path = docs + "/work/A.c2d";
    QString err;
    if (!padlink::buildDocument(fixture, path, nullptr, &err)) { std::fprintf(stderr, "build %s\n", qPrintable(err)); return 2; }
    const QByteArray mode = argc < 2 ? QByteArray("save") : QByteArray(argv[1]);
    const bool viaSave = mode != "nosave";
    int rc = 0;
    {
    MainWindow w;
    w.resize(1400, 900);
    w.show();
    w.activateWindow();
    w.openPath(path, false);
    QApplication::processEvents();
    QStringList lost;
    const QString id = w.padShareSaved(path, true, &lost, &err);
    std::printf("pad id %s lost [%s] %s\n", qPrintable(id), qPrintable(lost.join(",")), qPrintable(err));
    if (id.isEmpty()) return 2;
    // The pad's edit: Holes 4 mm -> 7 mm deep.
    QDir().mkpath(docs + "/padreplica");
    padlink::Store pad(docs + "/phobiCCCP/Pad", docs + "/padreplica");
    QJsonObject design = pad.get(id);
    QJsonArray tps = design.value("toolpaths").toArray();
    for (int i = 0; i < tps.size(); ++i) { QJsonObject t = tps.at(i).toObject(); if (t.value("name").toString() == "Holes") { t.insert("depth", 7.0); tps.replace(i, t); } }
    design.insert("toolpaths", tps);
    design = pad.edited(design, design.value("modified").toDouble() + 60000);

    // The user: feed typed into "Lettering" (no Enter) ...
    w.setMode(MainWindow::Mode::Toolpaths, false);
    QApplication::processEvents();
    QTreeWidget *list = w.m_tp->m_list;
    list->setCurrentItem(list->topLevelItem(0));
    QApplication::processEvents();
    auto *s = qobject_cast<QDoubleSpinBox *>(w.m_tp->m_form->m_editors.value("speeds.feedrate"));
    s->setFocus(); QApplication::processEvents(); s->selectAll(); QTest::keyClicks(s, "1111");
    std::printf("typingPending %d dirty %d\n", int(w.typingPending()), int(w.m_dirty));
    QString asked;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, [&] {
        if (auto *mb = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) { asked += mb->text() + " | "; mb->defaultButton()->click(); }
    });
    poll.start(50);
    // ... the pad's put has just come in (queued, as setReceiving queues it) ...
    QTimer::singleShot(0, &w, [&] { w.onPadRequest(1, QJsonObject{{"op", "put"}, {"design", design}}, "10.0.0.9"); });
    if (mode == "close") {
        // ... and the window is closed in the same pass.
        const bool closed = w.close();
        std::printf("close: closed %d, asked: %s\n", int(closed), qPrintable(asked));
    } else if (viaSave) {
        // ... and Ctrl+S is handled in the same pass of the event loop.
        w.onSave();
    } else {
        // Control: the put is handled first, the field still focused.
        QApplication::processEvents();
        w.m_canvas->setFocus();
        QApplication::processEvents();
        w.onSave();
    }
    for (int i = 0; i < 10; ++i) QApplication::processEvents();
    poll.stop();
    Document disk; disk.load(path, &err);
    double holes = 0, lettering = 0;
    for (const Toolpath &t : disk.toolpaths()) {
        if (t.json.value("name").toString() == "Holes") holes = std::fabs(t.json.value("end_depth").toVariant().toDouble());
        if (t.json.value("name").toString() == "Lettering") lettering = feed(t.json);
    }
    const QJsonObject onPads = padlink::Store(docs + "/phobiCCCP/Pad", docs + "/x").get(id);
    double padHoles = 0;
    for (const QJsonValue &v : onPads.value("toolpaths").toArray()) if (v.toObject().value("name").toString() == "Holes") padHoles = v.toObject().value("depth").toDouble();
    std::printf("%s: file Holes depth %g (pad set 7), Lettering feed %g (typed 1111); pads now Holes %g; dirty %d; asked: %s\n",
                viaSave ? "save in same pass" : "control", holes, lettering, padHoles, int(w.m_dirty), qPrintable(asked));
    const bool padKept = padHoles == 7 || holes == 7;
    std::printf("%s\n", padKept ? "ok: the pad's edit survives" : "FAIL: the pad's edit is gone from the file and from the pads");
    // Typed before Save: it is what gets saved (the pad's edit waits on the pads).
    const bool typedKept = !viaSave || lettering == 1111;
    std::printf("%s\n", typedKept ? "ok: the typed feed is saved" : "FAIL: the typed feed was lost");
    rc = padKept && typedKept ? 0 : 1;
    w.m_dirty = false;
    }
    return rc;
}
