// A pad's edit ("put") of the open, linked design while a number is being
// typed: that number is an unsaved change, so the file is left alone (the
// pad's edit stays on the pads), nothing asks to save over it, and the
// typing is not cut short.
//
// Plain checks, no framework; exits 0 on success.
#define private public
#include "../src/mainwindow.h"
#include "../src/canvas.h"
#include "../src/toolpathform.h"
#include "../src/toolpathpanel.h"
#include "../src/propertiespanel.h"
#undef private
#include "../src/c2ddocument.h"
#include "../src/element.h"
#include "../src/padlink.h"
#include "../src/toolpathfactory.h"

#include <QApplication>
#include <QDoubleSpinBox>
#include <QJsonArray>
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

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // A home of its own: the pad store and Documents are real folders under it.
    QTemporaryDir home;
    qputenv("HOME", home.path().toLocal8Bit());
    qunsetenv("XDG_DOCUMENTS_DIR");
    qunsetenv("XDG_DATA_HOME");
    qunsetenv("XDG_CONFIG_HOME");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("phobicccp-test"));
    QCoreApplication::setApplicationName(QStringLiteral("test_padput"));
    const QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (!docs.startsWith(home.path())) {
        std::fprintf(stderr, "Documents is not under the test's home: %s\n", qPrintable(docs));
        return 2;
    }

    c2d::MainWindow w;
    w.resize(1200, 800);
    w.show();
    w.newUntitled(300, 200, 19);
    c2d::Document &d = w.m_doc;
    const c2d::Element r = c2d::Element::makeRectangle({80, 80}, 40, 30, d.defaultLayer());
    d.addElement(r);
    QDir().mkpath(docs + "/work");
    const QString path = docs + "/work/A.c2d";
    QFile::remove(path);
    QString err;
    if (!d.save(path, &err)) { std::fprintf(stderr, "save: %s\n", qPrintable(err)); return 2; }
    w.m_untitled = false;
    w.m_dirty = false;
    w.openPath(path, false);
    QStringList lost;
    const QString id = w.padShareSaved(path, true, &lost, &err);
    std::fprintf(stderr, "pad id %s lost [%s] err %s\n", qPrintable(id), qPrintable(lost.join(", ")), qPrintable(err));
    if (id.isEmpty()) return 2;

    // The pad's edit: the rectangle made wider (pad replica id of its own).
    QDir().mkpath(docs + "/padreplica");
    c2d::padlink::Store pad(docs + "/phobiCCCP/Pad", docs + "/padreplica");
    QJsonObject design = pad.get(id);
    QJsonArray shapes = design.value("shapes").toArray();
    QJsonObject s0 = shapes.at(0).toObject();
    s0.insert("w", 120.0);
    shapes.replace(0, s0);
    design.insert("shapes", shapes);
    design = pad.edited(design, design.value("modified").toDouble() + 60000);

    // A height typed into Properties for the rectangle, not yet committed.
    w.setMode(c2d::MainWindow::Mode::Design, false);
    w.m_canvas->selectElements({w.m_doc.elements().at(0).id});
    QApplication::processEvents();
    QDoubleSpinBox *spin = w.m_props->m_height;
    w.activateWindow();
    spin->setFocus();
    QApplication::processEvents();
    QTest::keyClick(spin, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClicks(spin, "5");
    std::fprintf(stderr, "before put: dirty %d\n", int(w.m_dirty));

    // Whatever asks: take its default button (Save), as Enter would.
    QString asked;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, [&] {
        if (auto *mb = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            asked += mb->text() + " | ";
            mb->defaultButton()->click();
        }
    });
    poll.start(50);
    w.onPadRequest(1, QJsonObject{{"op", "put"}, {"design", design}}, "10.0.0.9");
    poll.stop();
    // The typing goes on undisturbed: the pad's edit did not take the focus.
    QTest::keyClicks(spin, "5");
    const bool typingKept = spin->hasFocus() && spin->text().startsWith(QStringLiteral("55"));
    std::fprintf(stderr, "field after the put: '%s' focus %d\n", qPrintable(spin->text()), int(spin->hasFocus()));
    std::fprintf(stderr, "asked: %s\n", qPrintable(asked));

    c2d::Document onDisk;
    onDisk.load(path, &err);
    double width = 0, depth = 0;
    for (const c2d::Element &e : onDisk.elements()) {
        width = e.painterPath.boundingRect().width();
        depth = e.painterPath.boundingRect().height();
    }
    const QJsonObject kept = c2d::padlink::Store(docs + "/phobiCCCP/Pad", docs + "/x").get(id);
    double keptW = 0;
    for (const QJsonValue &v : kept.value("shapes").toArray()) keptW = v.toObject().value("w").toDouble();
    std::fprintf(stderr, "file: rect width %g, height %g; pads now have width %g; window doc %s dirty %d\n",
                 width, depth, keptW, qPrintable(w.m_doc.filePath()), int(w.m_dirty));
    const bool ok = asked.isEmpty() && keptW > 100 && width < 50 && typingKept;
    std::printf("%s\n", ok ? "test_padput: OK, the pad edit is kept on the pads and the file left alone"
                            : "FAIL: test_padput");
    // Done typing before the window goes (as closing it would commit first).
    spin->clearFocus();
    QApplication::processEvents();
    w.m_dirty = false;
    return ok ? 0 : 1;
}
