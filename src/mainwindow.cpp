#include "mainwindow.h"

#include <QApplication>
#include <QDockWidget>
#include <QFileDialog>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QStatusBar>
#include <QStringList>

namespace c2d {

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("phobicCC — Carbide Create .c2d (Linux)"));
    resize(1100, 780);

    m_canvas = new Canvas(this);
    setCentralWidget(m_canvas);

    m_info = new QPlainTextEdit(this);
    m_info->setReadOnly(true);
    m_info->setMaximumWidth(340);
    auto *dock = new QDockWidget(QStringLiteral("Document"), this);
    dock->setWidget(m_info);
    addDockWidget(Qt::RightDockWidgetArea, dock);

    auto *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    fileMenu->addAction(QStringLiteral("&Open…"), QKeySequence::Open,
                        this, &MainWindow::onOpen);
    fileMenu->addAction(QStringLiteral("E&xit"), QKeySequence::Quit,
                        qApp, &QApplication::quit);

    statusBar()->showMessage(QStringLiteral("Open a .c2d file to begin"));
}

void MainWindow::onOpen()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open Carbide Create file"), {},
        QStringLiteral("Carbide Create (*.c2d);;All files (*)"));
    if (!path.isEmpty())
        openFile(path);
}

void MainWindow::openFile(const QString &path)
{
    QString err;
    if (!m_doc.load(path, &err)) {
        QMessageBox::warning(this, QStringLiteral("Open failed"), err);
        return;
    }
    m_canvas->setDocument(&m_doc);
    refreshInfo();
    statusBar()->showMessage(path);
}

void MainWindow::refreshInfo()
{
    QStringList lines;
    lines << QStringLiteral("== params ==");
    const auto p = m_doc.params();
    QStringList keys = p.keys();
    keys.sort();
    for (const QString &k : keys)
        lines << QStringLiteral("%1 = %2").arg(k, p.value(k));

    // element type histogram
    QHash<QString, int> elemHist;
    for (const Element &e : m_doc.elements())
        elemHist[e.geometryType]++;
    lines << QString() << QStringLiteral("== elements (%1) ==").arg(m_doc.elements().size());
    for (auto it = elemHist.constBegin(); it != elemHist.constEnd(); ++it)
        lines << QStringLiteral("%1: %2").arg(it.key()).arg(it.value());

    // toolpath list
    lines << QString() << QStringLiteral("== toolpaths (%1) ==").arg(m_doc.toolpaths().size());
    for (const Toolpath &t : m_doc.toolpaths())
        lines << QStringLiteral("%1  [%2]").arg(t.json.value("name").toString(), t.type);

    m_info->setPlainText(lines.join(QChar('\n')));
}

} // namespace c2d
