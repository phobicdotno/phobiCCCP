// r12 ui reviewer: typed value in toolpath form then toolbar actions (QToolButton, no focus).
#define private public
#define protected public
#include "../src/mainwindow.h"
#include "../src/canvas.h"
#include "../src/propertiespanel.h"
#include "../src/toolpathpanel.h"
#include "../src/toolpathform.h"
#undef private
#undef protected
#include "../src/c2ddocument.h"
#include "../src/padlink.h"
#include <QApplication>
#include <QDoubleSpinBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QToolBar>
#include <QToolButton>
#include <QDir>
#include <QFile>
#include <QTest>
#include <QTimer>
#include <cstdio>
using namespace c2d;
static int fails = 0;
static void check(bool ok, const char *what) { std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++fails; }
static void pump() { for (int i = 0; i < 6; ++i) QApplication::processEvents(); }
static double feed(const Toolpath &t) { return t.json.value("speeds").toObject().value("feedrate").toDouble(); }
static void dump(MainWindow &w) { for (const auto &t : w.m_doc.toolpaths()) std::printf("   %-28s feed %g\n", qPrintable(t.json.value("name").toString()), feed(t)); }
static QDoubleSpinBox *field(MainWindow &w, const QString &key) { return qobject_cast<QDoubleSpinBox *>(w.m_tp->m_form->m_editors.value(key)); }
static void type(QDoubleSpinBox *s, const char *txt) { s->setFocus(); QApplication::processEvents(); s->selectAll(); QTest::keyClicks(s, txt); }
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
    QCoreApplication::setApplicationName("test_typedclick");
    const QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QFile f(QStringLiteral(PHOBICCCP_SOURCE_DIR "/tests/data/pad-design.json"));
    f.open(QIODevice::ReadOnly);
    const QJsonObject fixture = QJsonDocument::fromJson(f.readAll()).object();
    QDir().mkpath(docs + "/work");
    const QString path = docs + "/work/A.c2d";
    QString err;
    if (!padlink::buildDocument(fixture, path, nullptr, &err)) { std::fprintf(stderr, "build %s\n", qPrintable(err)); return 2; }
    {
    MainWindow w;
    w.resize(1400, 900);
    w.show();
    w.activateWindow();
    w.openPath(path, false);
    pump();
    w.setMode(MainWindow::Mode::Toolpaths, false);
    pump();
    QTreeWidget *list = w.m_tp->m_list;
    const QString key = "speeds.feedrate";
    list->setCurrentItem(list->topLevelItem(0)); pump();
    std::printf("form has %s: %d\n", qPrintable(key), field(w, key) != nullptr);
    dump(w);

    // A: typed in tp0, then a toolbar "new toolpath" button -- driven inside app.exec()
    {
        const QString uuid0 = w.m_tp->m_uuid;
        QAction *act = nullptr;
        for (QAction *a : w.m_tpBar->actions()) if (!act && a->text() == "Pocket") act = a;
        QWidget *btn = w.m_tpBar->widgetForAction(act);
        QTimer::singleShot(0, [&] { type(field(w, key), "1111"); });
        QTimer::singleShot(100, [&] { std::printf("A: clicking %s\n", qPrintable(act->text())); QTest::mouseClick(btn, Qt::LeftButton); });
        QTimer::singleShot(300, [&] { std::printf("A: focus %s\n", QApplication::focusWidget() ? QApplication::focusWidget()->metaObject()->className() : "none"); w.m_canvas->setFocus(); });
        QTimer::singleShot(500, [&] { std::printf("quit\n"); for (QWidget *tw : QApplication::topLevelWidgets()) if (tw->isVisible()) std::printf(" top %s %s\n", tw->metaObject()->className(), qPrintable(tw->windowTitle())); QCoreApplication::exit(0); });
        app.exec();
        dump(w);
        std::printf("count %d\n", int(w.m_doc.toolpaths().size()));
        check(feed(*w.m_doc.toolpathByUuid(uuid0)) == 1111, "A: typed 1111 kept in tp0 after New toolpath button");
        check(feed(w.m_doc.toolpaths().last()) != 1111, "A: new toolpath did not get tp0's typed value");
    }
    // B: typed in a toolpath, then toolbar Delete (removes it); neighbour must not get the value
    {
        list->setCurrentItem(list->topLevelItem(1)); pump();
        const double before = feed(w.m_doc.toolpaths().at(2));
        const QString nb = w.m_doc.toolpaths().at(2).uuid;
        type(field(w, key), "2222");
        QTest::mouseClick(w.m_tp->m_delBtn, Qt::LeftButton);
        pump();
        w.m_canvas->setFocus(); pump();
        dump(w);
        check(feed(*w.m_doc.toolpathByUuid(nb)) == before, "B: neighbour unchanged after deleting a toolpath with a typed value");
    }
    // C: typed then Down button
    {
        list->setCurrentItem(list->topLevelItem(0)); pump();
        const QString u = w.m_tp->m_uuid;
        type(field(w, key), "3333");
        QTest::mouseClick(w.m_tp->m_downBtn, Qt::LeftButton);
        pump();
        w.m_canvas->setFocus(); pump();
        dump(w);
        check(feed(*w.m_doc.toolpathByUuid(u)) == 3333, "C: typed kept after Move down");
        check(w.m_doc.toolpathIndex(u) == 1, "C: moved down");
    }
    // D: an edit to another toolpath, then typed value in the current one and the toolbar Undo button
    {
        const Toolpath other = w.m_doc.toolpaths().last();
        QJsonObject oj = other.json; QJsonObject sp = oj.value("speeds").toObject(); sp.insert("feedrate", 999.0); oj.insert("speeds", sp);
        w.m_canvas->editToolpath(other.uuid, oj); pump();
        list->setCurrentItem(list->topLevelItem(0)); pump();
        const QString u = w.m_tp->m_uuid;
        QToolButton *undoBtn = nullptr;
        for (QToolBar *tb : w.findChildren<QToolBar *>()) for (QAction *a : tb->actions()) if (a->text().contains("Undo") && tb->widgetForAction(a) && tb->isVisible()) undoBtn = qobject_cast<QToolButton *>(tb->widgetForAction(a));
        std::printf("D: undo button %p\n", (void*)undoBtn);
        QTimer::singleShot(0, [&] { type(field(w, key), "7777"); });
        QTimer::singleShot(100, [&] { if (undoBtn) QTest::mouseClick(undoBtn, Qt::LeftButton); });
        QTimer::singleShot(300, [&] { std::printf("D: focus %s text %s\n", QApplication::focusWidget() ? QApplication::focusWidget()->metaObject()->className() : "none", field(w,key)?qPrintable(field(w,key)->text()):"-"); w.m_canvas->setFocus(); });
        QTimer::singleShot(500, [&] { QCoreApplication::exit(0); });
        app.exec();
        dump(w);
        check(feed(*w.m_doc.toolpathByUuid(u)) == 7777, "D: typed kept after toolbar Undo of another toolpath's edit");
        check(feed(*w.m_doc.toolpathByUuid(other.uuid)) == 5000, "D: other's edit undone");
    }
    // E: Design mode, typed width in Properties, then a Vectors toolbar button
    {
        w.setMode(MainWindow::Mode::Design, false); pump();
        QString rectId; for (const auto &e : w.m_doc.elements()) if (e.geometryType == "rectangle") rectId = e.id;
        w.m_canvas->selectElements({rectId}); pump();
        QToolButton *btn = nullptr; QString which;
        for (QToolBar *tb : w.findChildren<QToolBar *>()) if (tb->isVisible()) for (QAction *a : tb->actions()) { if (!btn && a->text().contains("Mirror") && tb->widgetForAction(a)) { btn = qobject_cast<QToolButton *>(tb->widgetForAction(a)); which = a->text(); } }
        std::printf("E: button %s %p width before %g\n", qPrintable(which), (void*)btn, w.m_doc.elementById(rectId)->raw.value("width").toDouble());
        QTimer::singleShot(0, [&] { auto *s = w.m_props->m_width; s->setFocus(); QApplication::processEvents(); s->selectAll(); QTest::keyClicks(s, "77"); });
        QTimer::singleShot(100, [&] { if (btn) QTest::mouseClick(btn, Qt::LeftButton); });
        QTimer::singleShot(300, [&] { std::printf("E: focus %s width field %s\n", QApplication::focusWidget() ? QApplication::focusWidget()->metaObject()->className() : "none", qPrintable(w.m_props->m_width->text())); w.m_canvas->setFocus(); });
        QTimer::singleShot(500, [&] { QCoreApplication::exit(0); });
        app.exec();
        std::printf("E: width after %g\n", w.m_doc.elementById(rectId)->raw.value("width").toDouble());
        check(std::fabs(w.m_doc.elementById(rectId)->raw.value("width").toDouble() - 77) < 1e-6, "E: typed width kept after a Vectors toolbar button");
    }
    w.m_dirty = false;
    }
    return fails ? 1 : 0;
}
