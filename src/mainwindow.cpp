#include "mainwindow.h"

#include <QApplication>
#include <QDockWidget>
#include <QFileDialog>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QStatusBar>
#include <QStringList>
#include <QDebug>
#include <QSqlDatabase>
#include <QActionGroup>
#include <QLabel>
#include <QSpinBox>
#include <QToolBar>

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

    // Vector tools.
    auto *tb = addToolBar(QStringLiteral("Tools"));
    tb->setMovable(false);
    auto *grp = new QActionGroup(this);
    auto addTool = [&](const QString &name, Canvas::Tool t, Qt::Key key) {
        QAction *a = tb->addAction(name);
        a->setCheckable(true);
        a->setShortcut(key);
        grp->addAction(a);
        connect(a, &QAction::triggered, this, [this, t] { m_canvas->setTool(t); });
        return a;
    };
    addTool(QStringLiteral("Select (V)"),    Canvas::Select,      Qt::Key_V)->setChecked(true);
    addTool(QStringLiteral("Circle (C)"),    Canvas::DrawCircle,  Qt::Key_C);
    addTool(QStringLiteral("Rectangle (R)"), Canvas::DrawRect,    Qt::Key_R);
    addTool(QStringLiteral("Polygon (P)"),   Canvas::DrawPolygon, Qt::Key_P);

    tb->addSeparator();
    tb->addWidget(new QLabel(QStringLiteral(" Sides: ")));
    auto *sides = new QSpinBox(tb);
    sides->setRange(3, 64);
    sides->setValue(6);
    connect(sides, &QSpinBox::valueChanged, this,
            [this](int n) { m_canvas->setPolygonSides(n); });
    tb->addWidget(sides);

    connect(m_canvas, &Canvas::documentChanged, this, [this] {
        refreshInfo();
        statusBar()->showMessage(QStringLiteral("Edited — Ctrl+S to save"));
    });
    connect(m_canvas, &Canvas::statusHint, this, [this](const QString &m) {
        statusBar()->showMessage(m);
    });

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
        onSaveAs();
        return;
    }
    QString err;
    if (!m_doc.save(m_doc.filePath(), &err))
        QMessageBox::warning(this, QStringLiteral("Save failed"), err);
    else
        statusBar()->showMessage(
            QStringLiteral("Saved %1").arg(m_doc.filePath()), 5000);
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
    if (!m_doc.save(path, &err))
        QMessageBox::warning(this, QStringLiteral("Save failed"), err);
    else
        statusBar()->showMessage(QStringLiteral("Saved %1").arg(path), 5000);
}

void MainWindow::openFile(const QString &path)
{
    qDebug() << "[diag] SQL drivers:" << QSqlDatabase::drivers();
    qDebug() << "[diag] opening:" << path;
    QString err;
    if (!m_doc.load(path, &err)) {
        qDebug() << "[diag] LOAD FAILED:" << err;
        QMessageBox::warning(this, QStringLiteral("Open failed"), err);
        return;
    }
    qDebug() << "[diag] loaded elements:" << m_doc.elements().size()
             << "toolpaths:" << m_doc.toolpaths().size()
             << "board:" << m_doc.boardWidth() << "x" << m_doc.boardHeight()
             << "params:" << m_doc.params().size();
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
