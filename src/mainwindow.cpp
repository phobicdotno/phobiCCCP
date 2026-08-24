#include "mainwindow.h"

#include <QApplication>
#include <QDockWidget>
#include <QFileDialog>
#include <QInputDialog>
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
    connect(m_canvas, &Canvas::elementMoved, this, &MainWindow::onElementMoved);

    m_info = new QPlainTextEdit(this);
    m_info->setReadOnly(true);
    m_info->setMaximumWidth(340);
    auto *dock = new QDockWidget(QStringLiteral("Document"), this);
    dock->setWidget(m_info);
    addDockWidget(Qt::RightDockWidgetArea, dock);

    // addAction(text, receiver, method, shortcut) — argument order that is
    // stable across Qt6 minor versions (the (text, shortcut, …) overload is newer).
    auto *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    fileMenu->addAction(QStringLiteral("&Open…"), this, &MainWindow::onOpen,
                        QKeySequence::Open);
    fileMenu->addAction(QStringLiteral("&Save"), this, &MainWindow::onSave,
                        QKeySequence::Save);
    fileMenu->addAction(QStringLiteral("Save &As…"), this, &MainWindow::onSaveAs,
                        QKeySequence::SaveAs);
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("E&xit"), qApp, &QApplication::quit,
                        QKeySequence::Quit);

    auto *editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
    editMenu->addAction(QStringLiteral("S&cale Selected…"), this,
                        &MainWindow::onScaleSelected,
                        QKeySequence(QStringLiteral("Ctrl+E")));
    editMenu->addAction(QStringLiteral("&Delete Selected"), this,
                        &MainWindow::onDeleteSelected, QKeySequence::Delete);
    editMenu->addSeparator();
    editMenu->addAction(QStringLiteral("Add &Circle…"), this,
                        &MainWindow::onAddCircle);
    editMenu->addAction(QStringLiteral("Add &Rectangle…"), this,
                        &MainWindow::onAddRectangle);

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

void MainWindow::onSave()
{
    if (m_doc.filePath().isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Nothing to save"),
                                 QStringLiteral("Open a .c2d file first."));
        return;
    }
    QString err;
    if (!m_doc.save(m_doc.filePath(), &err)) {
        QMessageBox::warning(this, QStringLiteral("Save failed"), err);
    } else {
        setDirty(false);
        statusBar()->showMessage(QStringLiteral("Saved %1").arg(m_doc.filePath()), 5000);
    }
}

void MainWindow::onSaveAs()
{
    if (m_doc.filePath().isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Nothing to save"),
                                 QStringLiteral("Open a .c2d file first."));
        return;
    }
    QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save Carbide Create file"), {},
        QStringLiteral("Carbide Create (*.c2d)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(QStringLiteral(".c2d"), Qt::CaseInsensitive))
        path += QStringLiteral(".c2d");

    QString err;
    if (!m_doc.save(path, &err)) {
        QMessageBox::warning(this, QStringLiteral("Save failed"), err);
    } else {
        setDirty(false);
        statusBar()->showMessage(QStringLiteral("Saved %1").arg(path), 5000);
    }
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
    setDirty(false);
    statusBar()->showMessage(path);
}

void MainWindow::onElementMoved(int index, QPointF delta)
{
    if (index < 0 || index >= m_doc.elements().size())
        return;
    m_doc.elements()[index].translate(delta.x(), delta.y());
    setDirty(true);
}

bool MainWindow::requireSelection(QList<int> *indices)
{
    *indices = m_canvas->selectedElements();
    if (indices->isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Nothing selected"), 3000);
        return false;
    }
    return true;
}

void MainWindow::onScaleSelected()
{
    QList<int> sel;
    if (!requireSelection(&sel))
        return;
    bool ok = false;
    const double f = QInputDialog::getDouble(
        this, QStringLiteral("Scale selected"),
        QStringLiteral("Scale factor (about each shape's center):"),
        1.0, 0.01, 100.0, 3, &ok);
    if (!ok || qFuzzyCompare(f, 1.0))
        return;
    for (int i : sel)
        m_doc.elements()[i].scaleBy(f);
    m_canvas->refresh();
    refreshInfo();
    setDirty(true);
}

void MainWindow::onAddCircle()
{
    if (m_doc.filePath().isEmpty())
        return;
    bool ok = false;
    const double d = QInputDialog::getDouble(
        this, QStringLiteral("Add circle"), QStringLiteral("Diameter (mm):"),
        10.0, 0.01, 10000.0, 2, &ok);
    if (!ok)
        return;
    const QPointF c(m_doc.boardWidth() / 2.0, m_doc.boardHeight() / 2.0);
    m_doc.addElement(Element::makeCircle(c, d / 2.0));
    m_canvas->refresh();
    refreshInfo();
    setDirty(true);
}

void MainWindow::onAddRectangle()
{
    if (m_doc.filePath().isEmpty())
        return;
    bool ok = false;
    const double w = QInputDialog::getDouble(
        this, QStringLiteral("Add rectangle"), QStringLiteral("Width (mm):"),
        20.0, 0.01, 10000.0, 2, &ok);
    if (!ok)
        return;
    const double h = QInputDialog::getDouble(
        this, QStringLiteral("Add rectangle"), QStringLiteral("Height (mm):"),
        10.0, 0.01, 10000.0, 2, &ok);
    if (!ok)
        return;
    const QPointF c(m_doc.boardWidth() / 2.0, m_doc.boardHeight() / 2.0);
    m_doc.addElement(Element::makeRectangle(c, w, h));
    m_canvas->refresh();
    refreshInfo();
    setDirty(true);
}

void MainWindow::onDeleteSelected()
{
    QList<int> sel;
    if (!requireSelection(&sel))
        return;
    // Remove back-to-front so earlier indices stay valid.
    for (int i = sel.size() - 1; i >= 0; --i)
        m_doc.removeElement(sel.at(i));
    m_canvas->refresh();
    refreshInfo();
    setDirty(true);
}

void MainWindow::setDirty(bool dirty)
{
    m_dirty = dirty;
    QString title = QStringLiteral("phobicCC — Carbide Create .c2d (Linux)");
    if (!m_doc.filePath().isEmpty())
        title += QStringLiteral("  [%1]%2").arg(m_doc.filePath(),
                                                dirty ? QStringLiteral(" *") : QString());
    setWindowTitle(title);
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
