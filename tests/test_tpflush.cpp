// Typed values in the panels survive the next click: properties panel typed width + click canvas; toolpath form typed + click other card;
// recovery roundtrip of a titled design.
#define private public
#define protected public
#include "../src/mainwindow.h"
#include "../src/canvas.h"
#include "../src/propertiespanel.h"
#include "../src/toolpathpanel.h"
#include "../src/toolpathform.h"
#include "../src/recovery.h"
#undef private
#undef protected
#include "../src/c2ddocument.h"
#include "../src/padlink.h"
#include <QApplication>
#include <QDoubleSpinBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QPushButton>
#include <QJsonArray>
#include <QToolButton>
#include <QDir>
#include <QFile>
#include <QTimer>
#include <QTest>
#include <cstdio>
using namespace c2d;
static int fails = 0;
static void check(bool ok, const char *what) { std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++fails; }
int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir home;
    qputenv("HOME", home.path().toLocal8Bit());
    qunsetenv("XDG_DOCUMENTS_DIR"); qunsetenv("XDG_DATA_HOME"); qunsetenv("XDG_CONFIG_HOME"); qunsetenv("XDG_CACHE_HOME");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("phobicccp-test");
    QCoreApplication::setApplicationName("test_tpflush");
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
    QApplication::processEvents();
    w.setMode(MainWindow::Mode::Design, false);
    QString rectId, textId;
    for (const auto &e : w.m_doc.elements()) { if (e.geometryType == "rectangle") rectId = e.id; if (e.geometryType == "text") textId = e.id; }
    // 1: typed width, then click on the text element on the canvas
    w.m_canvas->selectElements({rectId});
    QApplication::processEvents();
    QDoubleSpinBox *wsb = w.m_props->m_width;
    wsb->setFocus();
    QApplication::processEvents();
    wsb->selectAll();
    QTest::keyClicks(wsb, "77");
    const Element *te = w.m_doc.elementById(textId);
    QPoint vp = w.m_canvas->mapFromScene(te->painterPath.boundingRect().center());
    QTest::mouseClick(w.m_canvas->viewport(), Qt::LeftButton, {}, vp);
    QApplication::processEvents();
    double rw = w.m_doc.elementById(rectId)->raw.value("width").toDouble();
    std::printf("rect width after click elsewhere: %g; selection now %s\n", rw, qPrintable(w.m_canvas->selectedElementIds().join(",")));
    check(std::fabs(rw - 77) < 1e-6, "typed width committed when clicking on another shape");

    // 2: toolpath form typed number then click another toolpath card
    w.setMode(MainWindow::Mode::Toolpaths, false);
    QApplication::processEvents();
    QTreeWidget *list = w.m_tp->m_list;
    list->setCurrentItem(list->topLevelItem(0));
    QApplication::processEvents();
    const QString tp0 = w.m_doc.toolpaths().at(0).uuid;
    QDoubleSpinBox *sp = nullptr; QString key;
    for (auto it = w.m_tp->m_form->m_editors.begin(); it != w.m_tp->m_form->m_editors.end(); ++it)
        if (auto *s = qobject_cast<QDoubleSpinBox *>(it.value())) if (s->isVisible() && s->isEnabled() && it.key().contains("feedrate")) { sp = s; key = it.key(); break; }
    if (!sp) for (auto it = w.m_tp->m_form->m_editors.begin(); it != w.m_tp->m_form->m_editors.end(); ++it)
        if (auto *s = qobject_cast<QDoubleSpinBox *>(it.value())) if (s->isVisible() && s->isEnabled()) { sp = s; key = it.key(); break; }
    std::printf("form field %s visible %d\n", qPrintable(key), sp ? sp->isVisible() : -1);
    if (sp) {
        sp->setFocus(); QApplication::processEvents();
        sp->selectAll();
        QTest::keyClicks(sp, "1234");
        QRect r1 = list->visualItemRect(list->topLevelItem(1));
        QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, r1.center());
        for (int i = 0; i < 5; ++i) QApplication::processEvents();
        std::printf("after click: tp0 %s = %s; current tp %s\n", qPrintable(key),
                    qPrintable(QJsonDocument(QJsonObject{{"v", w.m_doc.toolpaths().at(0).json.value("speeds").toObject().value("feedrate")}}).toJson(QJsonDocument::Compact)),
                    qPrintable(w.m_tp->m_uuid));
    }

    // 3: typed feedrate in toolpath 0, then the Duplicate button
    if (sp) {
        list->setCurrentItem(list->topLevelItem(0));
        for (int i = 0; i < 5; ++i) QApplication::processEvents();
        QDoubleSpinBox *s3 = qobject_cast<QDoubleSpinBox *>(w.m_tp->m_form->m_editors.value(key));
        s3->setFocus(); QApplication::processEvents();
        s3->selectAll();
        QTest::keyClicks(s3, "2222");
        std::printf("dup button focus policy %d\n", int(w.m_tp->m_dupBtn->focusPolicy()));
        QTest::mouseClick(w.m_tp->m_dupBtn, Qt::LeftButton);
        for (int i = 0; i < 5; ++i) QApplication::processEvents();
        std::printf("focus now %s\n", QApplication::focusWidget() ? QApplication::focusWidget()->metaObject()->className() : "none");
        w.m_canvas->setFocus();
        for (int i = 0; i < 5; ++i) QApplication::processEvents();
        for (const auto &t : w.m_doc.toolpaths())
            std::printf("  tp %s feedrate %g\n", qPrintable(t.json.value("name").toString()), t.json.value("speeds").toObject().value("feedrate").toDouble());
        int both = 0;
        for (const auto &t : w.m_doc.toolpaths()) both += t.json.value("speeds").toObject().value("feedrate").toDouble() == 2222;
        check(both == 2, "typed 2222 is in the toolpath and its duplicate");
    }

    // 4: shapes selected on the canvas, typed feedrate, then "Assign selected vectors"
    if (sp) {
        list->setCurrentItem(list->topLevelItem(2));
        for (int i = 0; i < 5; ++i) QApplication::processEvents();
        const QString uuid = w.m_tp->m_uuid;
        w.m_canvas->selectElements({rectId});
        for (int i = 0; i < 5; ++i) QApplication::processEvents();
        std::printf("canvas selection %s; toolpath %s elements before %s\n", qPrintable(w.m_canvas->selectedElementIds().join(",")),
                    qPrintable(w.m_doc.toolpathByUuid(uuid)->json.value("name").toString()),
                    QJsonDocument(w.m_doc.toolpathByUuid(uuid)->json.value("elements").toArray()).toJson(QJsonDocument::Compact).constData());
        QDoubleSpinBox *s4 = qobject_cast<QDoubleSpinBox *>(w.m_tp->m_form->m_editors.value(key));
        s4->setFocus(); QApplication::processEvents();
        s4->selectAll();
        QTest::keyClicks(s4, "3333");
        QPushButton *assign = nullptr;
        for (auto *b : w.m_tp->findChildren<QPushButton *>()) if (b->text() == "Assign selected vectors") assign = b;
        QTest::mouseClick(assign, Qt::LeftButton);
        for (int i = 0; i < 5; ++i) QApplication::processEvents();
        const QJsonObject j = w.m_doc.toolpathByUuid(uuid)->json;
        const QJsonArray els = j.value("elements").toArray();
        std::printf("after Assign: feedrate %g elements %s\n", j.value("speeds").toObject().value("feedrate").toDouble(),
                    QJsonDocument(els).toJson(QJsonDocument::Compact).constData());
        check(els.size() == 1 && els.at(0).toObject().value("uuid").toString() == rectId, "Assign selected vectors kept");
        check(j.value("speeds").toObject().value("feedrate").toDouble() == 3333, "typed feedrate kept");
    }

    // 5: typed feedrate in toolpath 3, then its own on/off box in the list
    if (sp) {
        list->setCurrentItem(list->topLevelItem(3));
        for (int i = 0; i < 5; ++i) QApplication::processEvents();
        const QString uuid = w.m_tp->m_uuid;
        QDoubleSpinBox *s5 = qobject_cast<QDoubleSpinBox *>(w.m_tp->m_form->m_editors.value(key));
        s5->setFocus(); QApplication::processEvents();
        s5->selectAll();
        QTest::keyClicks(s5, "4444");
        QRect r = list->visualItemRect(list->topLevelItem(3)).adjusted(2, 3, -2, -3);
        const int sz = qMin(26, r.height() - 16);
        QPoint box(r.left() + 10 + sz / 2, r.center().y());
        QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, box);
        for (int i = 0; i < 5; ++i) QApplication::processEvents();
        const QJsonObject j = w.m_doc.toolpathByUuid(uuid)->json;
        std::printf("after box click: %s enabled %d feedrate %g; field shows %s\n", qPrintable(j.value("name").toString()),
                    int(j.value("enabled").toBool(true)), j.value("speeds").toObject().value("feedrate").toDouble(), qPrintable(s5->text()));
        check(!j.value("enabled").toBool(true), "box click switched it off");
        check(j.value("speeds").toObject().value("feedrate").toDouble() == 4444, "typed feedrate kept after box click");
    }
    w.m_dirty = false;
    }
    return fails ? 1 : 0;
}
