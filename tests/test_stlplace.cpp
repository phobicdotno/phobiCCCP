// STL import's default fit for a mesh too deep for the board: scaled down
// to fit both ways (it was scaled to 80 % of the width, 24 times up).
#include "../src/c2ddocument.h"
#include "../src/modelpanel.h"
#include "../src/model3d.h"
#include <QApplication>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFile>
#include <QLineEdit>
#include <QTest>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <cstring>
int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QTemporaryDir tmp; QString err;
    const QString blank = tmp.filePath("a.c2d");
    c2d::Document::createBlank(blank, 300, 300, 19, &err);
    c2d::Document d; d.load(blank, &err);
    // ASCII STL: one triangle 10 wide (X), 500 deep (Y), 5 tall (Z)
    const QString stl = tmp.filePath("tall.stl");
    QFile f(stl); f.open(QIODevice::WriteOnly);
    f.write("solid t\nfacet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex 10 0 5\nvertex 0 500 0\nendloop\nendfacet\nendsolid t\n");
    f.close();
    c2d::ModelPanel panel;
    panel.setDocument(&d);
    QTimer t; t.setInterval(30);
    QObject::connect(&t, &QTimer::timeout, [&] {
        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (auto *fd = qobject_cast<QFileDialog *>(w); fd && fd->isVisible()) { if (auto *le = fd->findChild<QLineEdit *>("fileNameEdit")) { le->setText(stl); QTest::keyClick(le, Qt::Key_Return); } return; }
            if (auto *dlg = qobject_cast<QDialog *>(w); dlg && dlg->isVisible() && dlg->windowTitle().startsWith("Import STL")) {
                dlg->accept(); return;
            }
        }
    });
    t.start();
    for (QPushButton *b : panel.findChildren<QPushButton *>())
        if (b->text().startsWith("STL")) b->click();
    t.stop();
    if (panel.model().components.isEmpty()) { std::printf("no component\n"); return 2; }
    const c2d::ModelComponent &c = panel.model().components.last();
    const double s = c.width > 0 ? c.width / 10.0 : 1.0;
    const bool ok = 10 * s <= 240 + 1e-6 && 500 * s <= 240 + 1e-6 && c.y >= 0 && c.y + 500 * s <= 300 + 1e-6;
    std::printf("%s a mesh deeper than the board is fitted inside it (width %g, y %g)\n", ok ? "ok  " : "FAIL", c.width, c.y);
    return ok ? 0 : 1;
}
