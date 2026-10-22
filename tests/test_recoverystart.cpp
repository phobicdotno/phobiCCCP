// phobicccp <file> after a crash. main() calls
// showStartupWindow() (which posts offerRecovery with singleShot(0)) and then
// openFile(argv[1]) before app.exec(). Any message box openFile shows runs a
// nested event loop, offerRecovery fires inside it and swaps the document
// while openFile is half way through.
#define private public
#include "../src/mainwindow.h"
#undef private
#include "../src/c2ddocument.h"
#include "../src/element.h"
#include "../src/recovery.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <cstdio>

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("phobicccp-test"));
    QCoreApplication::setApplicationName(QStringLiteral("test_recoverystart"));
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();

    QTemporaryDir tmp;
    // 1) A crashed session left a recovery copy of orig.c2d with 3 shapes.
    const QString orig = tmp.filePath(QStringLiteral("orig.c2d"));
    c2d::Document::createBlank(orig, 300, 200, 19);
    QThread::msleep(1100);
    {
        c2d::Document d;
        d.load(orig);
        for (int i = 0; i < 3; ++i)
            d.addElement(c2d::Element::makeCircle({50.0 + 40 * i, 50}, 10, d.defaultLayer()));
        c2d::Recovery r;
        const QString copy = r.write(d, orig, false);
        std::printf("recovery copy: %s\n", qPrintable(copy));
    }   // lock released, copy kept: as after a crash

    // 2) The file given on the command line: a DXF with one LINE and one
    // entity the importer skips, so the import ends with a notes box.
    const QString dxf = tmp.filePath(QStringLiteral("drawing.dxf"));
    QFile f(dxf);
    f.open(QIODevice::WriteOnly);
    f.write("0\nSECTION\n2\nENTITIES\n"
            "0\nLINE\n8\n0\n10\n10\n20\n10\n11\n60\n21\n10\n"
            "0\n3DFACE\n8\n0\n10\n0\n20\n0\n30\n0\n"
            "0\nENDSEC\n0\nEOF\n");
    f.close();

    // The user answers: Yes to the recovery, Discard at an unsaved-changes
    // question, OK to anything else.
    QStringList seen;
    QTimer answer;
    answer.setInterval(30);
    QObject::connect(&answer, &QTimer::timeout, [&] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!box)
            return;
        const QString t = box->text();
        seen << t.left(60).replace('\n', ' ');
        if (t.contains(QStringLiteral("recovery copy")))
            box->button(QMessageBox::Yes)->click();
        else if (t.contains(QStringLiteral("unsaved changes")))
            box->button(QMessageBox::Discard)->click();
        else
            box->accept();
    });
    answer.start();

    c2d::MainWindow w;
    w.resize(1200, 800);
    w.show();
    // What main() does: showStartupWindow() posts offerRecovery, then openFile.
    QTimer::singleShot(0, &w, &c2d::MainWindow::offerRecovery);
    w.openFile(dxf);

    for (const QString &s : seen)
        std::printf("box: %s\n", qPrintable(s));
    std::printf("doc: %s untitled=%d elements=%d untitledFrom=%s\n",
                qPrintable(w.m_doc.filePath()), int(w.m_untitled), int(w.m_doc.elements().size()),
                qPrintable(w.m_untitledFrom));
    const bool mixed = w.m_untitledFrom == dxf && w.m_doc.elements().size() == 3;
    std::printf("%s the open finishes before the recovery is offered\n", mixed ? "FAIL" : "ok  ");
    // ...and the recovery is still offered once it has.
    for (int i = 0; i < 40 && w.m_doc.elements().size() != 3; ++i) {
        QCoreApplication::processEvents();
        QThread::msleep(50);
    }
    const bool recovered = w.m_doc.elements().size() == 3 && w.m_untitledFrom != dxf;
    std::printf("%s the recovery is offered afterwards\n", recovered ? "ok  " : "FAIL");
    // A command-line file that fails to open leaves no design, which main()
    // replaces with a blank one (every tool did nothing without it).
    c2d::MainWindow v;
    v.openFile(tmp.filePath(QStringLiteral("moved-away.c2d")));
    const bool none = !v.hasDocument();
    if (none)
        v.newUntitled();
    const bool blank = none && v.hasDocument();
    std::printf("%s a file that fails to open is seen as no design, and a blank one follows\n",
                blank ? "ok  " : "FAIL");
    return mixed || !recovered || !blank ? 1 : 0;
}
