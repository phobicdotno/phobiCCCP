// A pad edit after a text field was edited back to its own text and Enter
// pressed (the field keeps the focus):
// nothing is unsaved, so the pad's edit rebuilds the open file.
//
// Plain checks, no framework; exits 0 on success.
#define private public
#include "../src/mainwindow.h"
#include "../src/canvas.h"
#include "../src/propertiespanel.h"
#undef private
#include "../src/c2ddocument.h"
#include "../src/padlink.h"
#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QTest>
#include <cstdio>
int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir home;
    qputenv("HOME", home.path().toLocal8Bit());
    qunsetenv("XDG_DOCUMENTS_DIR"); qunsetenv("XDG_DATA_HOME"); qunsetenv("XDG_CONFIG_HOME");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("phobicccp-test");
    QCoreApplication::setApplicationName("test_padenter");
    const QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QFile f(QStringLiteral(PHOBICCCP_SOURCE_DIR "/tests/data/pad-design.json"));
    f.open(QIODevice::ReadOnly);
    const QJsonObject fixture = QJsonDocument::fromJson(f.readAll()).object();
    QDir().mkpath(docs + "/work");
    const QString path = docs + "/work/A.c2d";
    QString err;
    if (!c2d::padlink::buildDocument(fixture, path, nullptr, &err)) { std::fprintf(stderr, "build %s\n", qPrintable(err)); return 2; }
    c2d::MainWindow w;
    w.resize(1200, 800);
    w.show();
    w.openPath(path, false);
    QStringList lost;
    const QString id = w.padShareSaved(path, true, &lost, &err);
    std::fprintf(stderr, "id %s lost [%s] %s\n", qPrintable(id), qPrintable(lost.join(",")), qPrintable(err));
    if (id.isEmpty() || !lost.isEmpty()) return 2;

    w.setMode(c2d::MainWindow::Mode::Design, false);
    QString textId;
    for (const auto &e : w.m_doc.elements()) if (e.geometryType == "text") textId = e.id;
    w.m_canvas->selectElements({textId});
    QApplication::processEvents();
    QLineEdit *le = w.m_props->m_text;
    w.activateWindow();
    le->setFocus();
    QApplication::processEvents();
    std::fprintf(stderr, "text field '%s' focus %d\n", qPrintable(le->text()), int(le->hasFocus()));
    QTest::keyClick(le, Qt::Key_End);
    QTest::keyClicks(le, "X");
    if (!qEnvironmentVariableIsSet("SAVEMODE")) QTest::keyClick(le, Qt::Key_Backspace);
    QTest::keyClick(le, Qt::Key_Return);
    QApplication::processEvents();
    if (qEnvironmentVariableIsSet("SAVEMODE")) { std::fprintf(stderr, "changed: dirty %d\n", int(w.m_dirty)); w.onSave(); QApplication::processEvents(); }
    std::fprintf(stderr, "after Enter: text '%s' dirty %d focus %d modified %d typingPending %d\n", qPrintable(le->text()),
                 int(w.m_dirty), int(le->hasFocus()), int(le->isModified()), int(w.typingPending()));

    QDir().mkpath(docs + "/padreplica");
    c2d::padlink::Store pad(docs + "/phobiCCCP/Pad", docs + "/padreplica");
    QJsonObject design = pad.get(id);
    QJsonArray shapes = design.value("shapes").toArray();
    double oldW = 0;
    for (int i = 0; i < shapes.size(); ++i) {
        QJsonObject s = shapes.at(i).toObject();
        if (s.value("type") == "rect" && !qEnvironmentVariableIsSet("SAVEMODE")) { oldW = s.value("w").toDouble(); s.insert("w", oldW + 10); shapes.replace(i, s); }
    }
    if (qEnvironmentVariableIsSet("SAVEMODE")) { design = pad.get(id); shapes = design.value("shapes").toArray();
      for (int i = 0; i < shapes.size(); ++i) { QJsonObject s = shapes.at(i).toObject();
        if (s.value("type") == "rect") { oldW = s.value("w").toDouble(); s.insert("w", oldW + 10); shapes.replace(i, s); } } }
    design.insert("shapes", shapes);
    design = pad.edited(design, design.value("modified").toDouble() + 60000);
    w.onPadRequest(1, QJsonObject{{"op", "put"}, {"design", design}}, "10.0.0.9");
    QApplication::processEvents();
    c2d::Document onDisk;
    onDisk.load(path, &err);
    double width = 0;
    for (const auto &e : onDisk.elements()) if (e.geometryType == "rectangle") width = e.raw.value("width").toDouble();
    std::fprintf(stderr, "rect width on disk %g (pad had %g, pad now %g)\n", width, oldW, oldW + 10);
    const bool ok = width > oldW + 5;
    std::printf("%s\n", ok ? "OK: open file took the pad edit" : "BUG: pad edit refused though nothing was unsaved");
    le->clearFocus(); w.m_dirty = false;
    return ok ? 0 : 1;
}
