#include "mainwindow.h"
#include "toolpathpanel.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QInputDialog>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSettings>
#include <QStatusBar>
#include <QStringList>

namespace c2d {

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("phobicCC - Carbide Create .c2d (Linux)"));
    resize(1100, 780);
    setDockNestingEnabled(true);

    // All three surfaces are docks (no central widget), so any of them can be
    // dragged to another screen, resized, or maximized there.
    m_canvas = new Canvas(this);
    connect(m_canvas, &Canvas::elementsMoved, this, &MainWindow::onElementsMoved);
    m_canvasDock = makeDock(QStringLiteral("Canvas"), QStringLiteral("dockCanvas"),
                            m_canvas, Qt::RightDockWidgetArea);

    m_info = new QPlainTextEdit(this);
    m_info->setReadOnly(true);
    m_infoDock = makeDock(QStringLiteral("Document"), QStringLiteral("dockDocument"),
                          m_info, Qt::RightDockWidgetArea);

    m_toolpaths = new ToolpathPanel(this);
    connect(m_toolpaths, &ToolpathPanel::aboutToEdit, this, &MainWindow::pushUndo);
    connect(m_toolpaths, &ToolpathPanel::edited, this, [this] {
        setDirty(true);
        refreshInfo();   // toolpath names appear in the sidebar list
    });
    m_tpDock = makeDock(QStringLiteral("Toolpaths"), QStringLiteral("dockToolpaths"),
                        m_toolpaths, Qt::LeftDockWidgetArea);

    resizeDocks({m_tpDock, m_canvasDock, m_infoDock}, {280, 620, 280}, Qt::Horizontal);

    // addAction(text, receiver, method, shortcut) - argument order that is
    // stable across Qt6 minor versions (the (text, shortcut, …) overload is newer).
    auto *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    fileMenu->addAction(QStringLiteral("&Open…"), this, &MainWindow::onOpen,
                        QKeySequence::Open);
    fileMenu->addAction(QStringLiteral("&Save"), this, &MainWindow::onSave,
                        QKeySequence::Save);
    fileMenu->addAction(QStringLiteral("Save &As…"), this, &MainWindow::onSaveAs,
                        QKeySequence::SaveAs);
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("E&xit"), this, &MainWindow::close,
                        QKeySequence::Quit);

    auto *editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
    editMenu->addAction(QStringLiteral("&Undo"), this, &MainWindow::onUndo,
                        QKeySequence::Undo);
    editMenu->addAction(QStringLiteral("&Redo"), this, &MainWindow::onRedo,
                        QKeySequence::Redo);
    editMenu->addSeparator();
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

    auto *viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    viewMenu->addAction(m_canvasDock->toggleViewAction());
    viewMenu->addAction(m_tpDock->toggleViewAction());
    viewMenu->addAction(m_infoDock->toggleViewAction());
    viewMenu->addSeparator();
    viewMenu->addAction(QStringLiteral("Re-&dock All"), this, [this] {
        for (QDockWidget *d : {m_canvasDock, m_tpDock, m_infoDock}) {
            d->setFloating(false);
            d->show();
        }
    });
    auto *fsAction = viewMenu->addAction(QStringLiteral("&Fullscreen"));
    fsAction->setCheckable(true);
    fsAction->setShortcut(QKeySequence::FullScreen);   // F11 on most platforms
    connect(fsAction, &QAction::toggled, this, [this](bool on) {
        if (on)
            showFullScreen();
        else
            showNormal();
    });

    statusBar()->showMessage(QStringLiteral("Open a .c2d file to begin"));

    // Restore last session's window geometry (screen placement included) and
    // dock layout. Qt sanity-checks against currently connected screens.
    QSettings settings;
    restoreGeometry(settings.value(QStringLiteral("geometry")).toByteArray());
    restoreState(settings.value(QStringLiteral("windowState")).toByteArray());
    fsAction->setChecked(isFullScreen());
}

QDockWidget *MainWindow::makeDock(const QString &title, const QString &objectName,
                                  QWidget *widget, Qt::DockWidgetArea area)
{
    auto *dock = new QDockWidget(title, this);
    dock->setObjectName(objectName);   // required for saveState/restoreState
    dock->setWidget(widget);
    addDockWidget(area, dock);

    // A floating dock gets a native window frame with minimize/maximize/close
    // buttons, so it can be maximized or fullscreened on whatever screen it
    // was dragged to. Docking back resets the flags automatically.
    connect(dock, &QDockWidget::topLevelChanged, dock, [dock](bool floating) {
        if (floating) {
            dock->setWindowFlags(Qt::CustomizeWindowHint | Qt::Window |
                                 Qt::WindowMinimizeButtonHint |
                                 Qt::WindowMaximizeButtonHint |
                                 Qt::WindowCloseButtonHint);
            dock->show();
        }
    });
    return dock;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!maybeSave()) {
        event->ignore();
        return;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("geometry"), saveGeometry());
    settings.setValue(QStringLiteral("windowState"), saveState());
    event->accept();
}

bool MainWindow::maybeSave()
{
    if (!m_dirty)
        return true;
    const auto ret = QMessageBox::warning(
        this, QStringLiteral("Unsaved changes"),
        QStringLiteral("The document has unsaved changes.\nSave them?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);
    if (ret == QMessageBox::Cancel)
        return false;
    if (ret == QMessageBox::Save) {
        onSave();
        return !m_dirty;   // save may have failed or been refused
    }
    return true;
}

void MainWindow::onOpen()
{
    if (!maybeSave())
        return;
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
    m_undoStack.clear();
    m_redoStack.clear();
    m_canvas->setDocument(&m_doc);
    m_toolpaths->setDocument(&m_doc);
    refreshInfo();
    setDirty(false);
    statusBar()->showMessage(path);
}

void MainWindow::pushUndo()
{
    m_redoStack.clear();
    m_undoStack.append({m_doc.elements(), m_doc.toolpaths()});
    while (m_undoStack.size() > 50)
        m_undoStack.removeFirst();
}

void MainWindow::applySnapshot(const Snapshot &snap)
{
    m_doc.elements() = snap.elements;
    m_doc.toolpaths() = snap.toolpaths;
    m_canvas->refresh();
    m_toolpaths->reload();
    refreshInfo();
    setDirty(true);
}

void MainWindow::onUndo()
{
    if (m_undoStack.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Nothing to undo"), 3000);
        return;
    }
    m_redoStack.append({m_doc.elements(), m_doc.toolpaths()});
    applySnapshot(m_undoStack.takeLast());
}

void MainWindow::onRedo()
{
    if (m_redoStack.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Nothing to redo"), 3000);
        return;
    }
    m_undoStack.append({m_doc.elements(), m_doc.toolpaths()});
    applySnapshot(m_redoStack.takeLast());
}

void MainWindow::onElementsMoved(const QList<int> &indices, const QList<QPointF> &deltas)
{
    pushUndo();
    for (int i = 0; i < indices.size(); ++i) {
        const int idx = indices.at(i);
        if (idx >= 0 && idx < m_doc.elements().size())
            m_doc.elements()[idx].translate(deltas.at(i).x(), deltas.at(i).y());
    }
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
    pushUndo();
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
    pushUndo();
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
    pushUndo();
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
    pushUndo();
    // Remove back-to-front so earlier indices stay valid; removeElement also
    // strips the uuid from any toolpath that referenced the element.
    for (int i = sel.size() - 1; i >= 0; --i)
        m_doc.removeElement(sel.at(i));
    m_canvas->refresh();
    m_toolpaths->reload();   // reference lists may have changed
    refreshInfo();
    setDirty(true);
}

void MainWindow::setDirty(bool dirty)
{
    m_dirty = dirty;
    QString title = QStringLiteral("phobicCC - Carbide Create .c2d (Linux)");
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
