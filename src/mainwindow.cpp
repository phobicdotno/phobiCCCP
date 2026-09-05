#include "mainwindow.h"
#include "backgrounddialog.h"
#include "gcodeexport.h"
#include "isopreview.h"
#include "machinepanel.h"
#include "tiling.h"
#include <QInputDialog>
#include "propertiespanel.h"
#include "simpanel.h"
#include "toolpathpanel.h"
#include "vectoractions.h"
#include "importui.h"

#include <QApplication>
#include <QFile>
#include <QFileInfo>
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
#include <QPainter>
#include <QPolygonF>
#include <QSpinBox>
#include <QToolBar>
#include <QUndoStack>
#include <QtMath>

namespace c2d {

// Simple flat tool icons drawn at runtime — no resource files needed.
static QIcon toolIcon(const QString &kind)
{
    QPixmap pm(20, 20);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPen pen(QColor(0xd8, 0xdc, 0xe4), 1.6);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    if (kind == "select") {
        QPolygonF arrow;
        arrow << QPointF(6, 3) << QPointF(6, 15) << QPointF(9.5, 11.5)
              << QPointF(12, 16) << QPointF(13.8, 15) << QPointF(11.4, 10.6)
              << QPointF(15, 10);
        p.setBrush(QColor(0xd8, 0xdc, 0xe4));
        p.drawPolygon(arrow);
    } else if (kind == "circle") {
        p.drawEllipse(QRectF(4, 4, 12, 12));
    } else if (kind == "rect") {
        p.drawRect(QRectF(4, 5, 12, 10));
    } else if (kind == "polygon") {
        QPolygonF hex;
        for (int i = 0; i < 6; ++i) {
            const double a = M_PI / 3 * i - M_PI / 6;
            hex << QPointF(10 + 6.5 * qCos(a), 10 + 6.5 * qSin(a));
        }
        p.drawPolygon(hex);
    } else if (kind == "path") {
        QPainterPath pp(QPointF(3, 16));
        pp.lineTo(8, 6);
        pp.lineTo(12, 12);
        pp.lineTo(17, 4);
        p.drawPath(pp);
        p.setBrush(QColor(0xd8, 0xdc, 0xe4));
        for (const QPointF &v : {QPointF(3, 16), QPointF(8, 6), QPointF(12, 12), QPointF(17, 4)})
            p.drawEllipse(v, 1.6, 1.6);
    } else if (kind == "text") {
        QFont f;
        f.setPixelSize(15);
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRectF(0, 0, 20, 20), Qt::AlignCenter, QStringLiteral("T"));
    } else if (kind == "gcode") {
        QFont f;
        f.setPixelSize(11);
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRectF(0, 0, 20, 20), Qt::AlignCenter, QStringLiteral("G1"));
    } else if (kind == "fit") {
        p.drawRect(QRectF(4, 4, 12, 12));
        p.drawLine(QLineF(8, 10, 12, 10));
        p.drawLine(QLineF(10, 8, 10, 12));
    } else if (kind == "snap") {
        for (int x = 4; x <= 16; x += 6)
            for (int y = 4; y <= 16; y += 6)
                p.drawPoint(QPointF(x, y));
        p.drawEllipse(QPointF(10, 10), 3.2, 3.2);
    }
    return QIcon(pm);
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("phobiCCCP — Carbide Create .c2d (Linux)"));
    resize(1100, 780);

    m_canvas = new Canvas(this);
    setCentralWidget(m_canvas);
    m_canvas->setBackgroundImage(&m_bg);

    m_props = new PropertiesPanel(m_canvas, this);
    auto *propsDock = new QDockWidget(QStringLiteral("Properties"), this);
    propsDock->setWidget(m_props);
    propsDock->setFeatures(QDockWidget::DockWidgetMovable);
    addDockWidget(Qt::RightDockWidgetArea, propsDock);

    m_tp = new ToolpathPanel(m_canvas, this);
    auto *tpDock = new QDockWidget(QStringLiteral("Toolpaths"), this);
    tpDock->setWidget(m_tp);
    tpDock->setFeatures(QDockWidget::DockWidgetMovable);
    addDockWidget(Qt::RightDockWidgetArea, tpDock);

    m_info = new QPlainTextEdit(this);
    m_info->setReadOnly(true);
    m_info->setMaximumWidth(340);
    auto *dock = new QDockWidget(QStringLiteral("Document"), this);
    dock->setWidget(m_info);
    dock->setFeatures(QDockWidget::DockWidgetMovable);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    m_machine = new MachinePanel(this);
    auto *mcDock = new QDockWidget(QStringLiteral("Machine"), this);
    m_mcDock = mcDock;
    mcDock->setWidget(m_machine);
    mcDock->setFeatures(QDockWidget::DockWidgetMovable);
    addDockWidget(Qt::RightDockWidgetArea, mcDock);

    m_iso = new IsoPreview(this);
    m_isoDock = new QDockWidget(QStringLiteral("Preview"), this);
    m_isoDock->setWidget(m_iso);
    m_isoDock->setFeatures(QDockWidget::DockWidgetMovable);
    addDockWidget(Qt::RightDockWidgetArea, m_isoDock);

    m_sim = new SimPanel(this);
    m_simDock = new QDockWidget(QStringLiteral("Simulation"), this);
    m_simDock->setWidget(m_sim);
    m_simDock->setFeatures(QDockWidget::DockWidgetMovable);
    addDockWidget(Qt::RightDockWidgetArea, m_simDock);

    tabifyDockWidget(tpDock, dock);     // Toolpaths / Document / Machine / Preview / Simulation tabs
    tabifyDockWidget(dock, mcDock);
    tabifyDockWidget(mcDock, m_isoDock);
    tabifyDockWidget(m_isoDock, m_simDock);
    tpDock->raise();
    // The 3D preview (and the simulation's program) is only rebuilt while one
    // of those tabs is showing; catch up when raised after edits behind it.
    for (QDockWidget *d : {m_isoDock, m_simDock})
        connect(d, &QDockWidget::visibilityChanged, this, [this](bool v) {
            if (v && m_isoStale)
                refreshIso();
        });

    connect(m_canvas, &Canvas::selectionChangedIds,
            m_props, &PropertiesPanel::setSelection);
    // Real machine work position → orange crosshair in the 3D preview.
    connect(m_machine, &MachinePanel::livePosition,
            m_iso, &IsoPreview::setLivePosition);

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
    fileMenu->addAction(QStringLiteral("Import &SVG…"), this, [this] {
        importVectorFile(this, m_canvas, &m_doc, {}, QStringLiteral("SVG (*.svg)")); });
    fileMenu->addAction(QStringLiteral("Import &DXF…"), this, [this] {
        importVectorFile(this, m_canvas, &m_doc, {}, QStringLiteral("DXF (*.dxf)")); });
    installImportDropHandler(this, m_canvas, &m_doc, [this](const QString &p) { openFile(p); });
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("Export &G-code…"), this,
                        &MainWindow::onExportGcode,
                        QKeySequence(Qt::CTRL | Qt::Key_G));
    fileMenu->addAction(QStringLiteral("Export G-code (&tiled)…"), this,
                        &MainWindow::onExportGcodeTiled);
    fileMenu->addSeparator();
    installImageMenus(fileMenu, this, m_canvas, &m_doc, &m_bg);   // backgrounddialog.cpp
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("E&xit"), qApp, &QApplication::quit,
                        QKeySequence::Quit);

    // Edit menu: undo/redo backed by the canvas undo stack.
    auto *editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
    QAction *undoAct = m_canvas->undoStack()->createUndoAction(this, QStringLiteral("&Undo"));
    undoAct->setShortcut(QKeySequence::Undo);
    QAction *redoAct = m_canvas->undoStack()->createRedoAction(this, QStringLiteral("&Redo"));
    redoAct->setShortcut(QKeySequence::Redo);
    editMenu->addAction(undoAct);
    editMenu->addAction(redoAct);

    // Vector tools: horizontal icon row above the canvas (deliberately not
    // Carbide Create's left-hand column).
    auto *palette = new QToolBar(QStringLiteral("Tools"), this);
    palette->setMovable(false);
    palette->setToolButtonStyle(Qt::ToolButtonIconOnly);
    palette->setIconSize(QSize(24, 24));
    addToolBar(Qt::TopToolBarArea, palette);
    auto *grp = new QActionGroup(this);
    auto addTool = [&](const QString &name, const QString &icon, Canvas::Tool t,
                       Qt::Key key, const QString &tip) {
        QAction *a = palette->addAction(toolIcon(icon), name);
        a->setCheckable(true);
        a->setShortcut(key);
        a->setToolTip(tip);
        grp->addAction(a);
        connect(a, &QAction::triggered, this, [this, t] { m_canvas->setTool(t); });
        return a;
    };
    addTool(QStringLiteral("Select"), QStringLiteral("select"), Canvas::Select, Qt::Key_V,
            QStringLiteral("Select / move / delete  (V)"))->setChecked(true);
    addTool(QStringLiteral("Circle"), QStringLiteral("circle"), Canvas::DrawCircle, Qt::Key_C,
            QStringLiteral("Circle: press at center, drag to radius  (C)"));
    addTool(QStringLiteral("Rect"), QStringLiteral("rect"), Canvas::DrawRect, Qt::Key_R,
            QStringLiteral("Rectangle: drag corner to corner  (R)"));
    addTool(QStringLiteral("Polygon"), QStringLiteral("polygon"), Canvas::DrawPolygon, Qt::Key_P,
            QStringLiteral("Polygon: press at center, drag to radius  (P)"));
    addTool(QStringLiteral("Path"), QStringLiteral("path"), Canvas::DrawPath, Qt::Key_L,
            QStringLiteral("Path: click points; Enter finishes, click near start closes  (L)"));
    addTool(QStringLiteral("Text"), QStringLiteral("text"), Canvas::DrawText, Qt::Key_T,
            QStringLiteral("Text: click to place  (T)"));

    // Top bar: options and view/edit actions.
    auto *tb = addToolBar(QStringLiteral("Options"));
    tb->setMovable(false);
    tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    tb->setIconSize(QSize(20, 20));

    auto *sides = new QSpinBox(tb);
    sides->setRange(3, 64);
    sides->setValue(6);
    sides->setPrefix(QStringLiteral("sides "));
    sides->setToolTip(QStringLiteral("Polygon sides"));
    QAction *sidesAct = tb->addWidget(sides);
    connect(sides, &QSpinBox::valueChanged, this,
            [this](int n) { m_canvas->setPolygonSides(n); });
    QAction *sidesSep = tb->addSeparator();

    // The side count only means something while drawing polygons, so show it
    // (and its separator) only when the Polygon tool is active.
    auto showSides = [sidesAct, sidesSep](Canvas::Tool t) {
        const bool on = (t == Canvas::DrawPolygon);
        sidesAct->setVisible(on);
        sidesSep->setVisible(on);
    };
    showSides(m_canvas->tool());
    connect(m_canvas, &Canvas::toolChanged, this, showSides);
    QAction *snapAct = tb->addAction(toolIcon(QStringLiteral("snap")), QStringLiteral("Snap"));
    snapAct->setCheckable(true);
    snapAct->setShortcut(Qt::Key_G);
    snapAct->setToolTip(QStringLiteral("Snap to grid  (G)"));
    connect(snapAct, &QAction::toggled, this,
            [this](bool on) { m_canvas->setSnapEnabled(on); });

    QAction *fitAct = tb->addAction(toolIcon(QStringLiteral("fit")), QStringLiteral("Fit"));
    fitAct->setShortcut(Qt::Key_F);
    fitAct->setToolTip(QStringLiteral("Zoom to fit board  (F)"));
    connect(fitAct, &QAction::triggered, this, [this] { m_canvas->zoomFit(); });

    m_previewAct = tb->addAction(toolIcon(QStringLiteral("gcode")),
                                 QStringLiteral("Preview"));
    m_previewAct->setCheckable(true);
    m_previewAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_P));
    m_previewAct->setToolTip(QStringLiteral(
        "Show the generated toolpath on the canvas — rapids dashed, cuts "
        "colored shallow→deep  (Ctrl+P)"));
    connect(m_previewAct, &QAction::toggled, this, [this](bool on) {
        if (!on) {
            m_canvas->clearToolpathPreview();
            return;
        }
        refreshPreview();
    });

    tb->addSeparator();
    tb->addAction(undoAct);
    tb->addAction(redoAct);
    new VectorActions(m_canvas, editMenu, this);   // Edit → Vectors + icon bar

    // Status bar: transient hints on the left, zoom + live mm on the right.
    m_zoomLabel = new QLabel(QStringLiteral("100%"), this);
    m_zoomLabel->setMinimumWidth(56);
    m_zoomLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    statusBar()->addPermanentWidget(m_zoomLabel);
    connect(m_canvas, &Canvas::zoomChanged, this, [this](double pct) {
        m_zoomLabel->setText(QStringLiteral("%1%").arg(qRound(pct)));
    });

    m_cursorLabel = new QLabel(QStringLiteral("X —    Y —"), this);
    m_cursorLabel->setMinimumWidth(180);
    m_cursorLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    statusBar()->addPermanentWidget(m_cursorLabel);
    connect(m_canvas, &Canvas::cursorMoved, this, [this](QPointF p) {
        m_cursorLabel->setText(QStringLiteral("X %1    Y %2 mm")
                                   .arg(p.x(), 0, 'f', 2).arg(p.y(), 0, 'f', 2));
    });

    connect(m_canvas, &Canvas::documentChanged, this, [this] {
        m_dirty = true;
        updateTitle();
        refreshInfo();
        m_props->refresh();
        m_tp->refresh();
        if (m_previewAct->isChecked())
            refreshPreview();
        else
            markIsoStale();
        statusBar()->showMessage(QStringLiteral("Edited — Ctrl+S to save"));
    });
    connect(m_canvas, &Canvas::statusHint, this, [this](const QString &m) {
        statusBar()->showMessage(m);
    });

    statusBar()->showMessage(QStringLiteral("Open a .c2d file to begin"));
}

void MainWindow::onExportGcode()
{
    if (m_doc.filePath().isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Export G-code"),
                                 QStringLiteral("Open a .c2d file first."));
        return;
    }
    const GcodeResult r = exportGcode(m_doc);
    if (r.done.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Export G-code"),
            QStringLiteral("No exportable toolpaths.\n\nSupported today: contour "
                           "(no offset) and drilling.\nSkipped:\n  %1")
                .arg(r.skipped.join(QStringLiteral("\n  "))));
        return;
    }
    // A document with tiling switched on whose job is taller than one tile is
    // meant to be cut in pieces: offer the tiled export.
    const double tileH = m_doc.params().value("tile_height", "0").toDouble();
    if (m_doc.params().value("tiling_enabled") == QLatin1String("1") && tileH > 0
        && tileCount(r.ops, tileH) > 1) {
        const auto ans = QMessageBox::question(
            this, QStringLiteral("Export G-code"),
            QStringLiteral("Tiling is enabled in this document and the job needs %1 "
                           "tiles of %2 mm.\n\nExport one program per tile?")
                .arg(tileCount(r.ops, tileH)).arg(tileH),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::Yes);
        if (ans == QMessageBox::Cancel)
            return;
        if (ans == QMessageBox::Yes) {
            onExportGcodeTiled();
            return;
        }
    }
    QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export G-code"),
        QFileInfo(m_doc.filePath()).completeBaseName() + QStringLiteral(".nc"),
        QStringLiteral("G-code (*.nc *.gcode);;All files (*)"));
    if (path.isEmpty())
        return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("Export failed"), f.errorString());
        return;
    }
    f.write(r.gcode.toUtf8());
    f.close();
    QString msg = QStringLiteral("Exported %1 toolpath(s) to %2   [%3]")
                      .arg(r.done.size()).arg(path)
                      .arg(statsSummary(computeStats(r.ops))
                               .replace(QChar('\n'), QStringLiteral("  ·  ")));
    if (!r.skipped.isEmpty())
        msg += QStringLiteral("  (skipped: %1)").arg(r.skipped.join(QStringLiteral(", ")));
    statusBar()->showMessage(msg, 8000);
}

void MainWindow::onExportGcodeTiled()
{
    if (m_doc.filePath().isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Export G-code (tiled)"),
                                 QStringLiteral("Open a .c2d file first."));
        return;
    }
    const double docTile = m_doc.params().value("tile_height", "508.0").toDouble();
    bool ok = false;
    const double tileH = QInputDialog::getDouble(
        this, QStringLiteral("Export G-code (tiled)"),
        QStringLiteral("Tile height (mm): the Y extent one program may use.\n"
                       "Tile k holds the cuts with Y in [k·h, (k+1)·h), shifted down\n"
                       "by k·h — slide the stock forward by h between tiles."),
        docTile > 0 ? docTile : 508.0, 10.0, 10000.0, 1, &ok);
    if (!ok)
        return;
    QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export tiled G-code — base name (_tile1.nc, _tile2.nc, … are added)"),
        QFileInfo(m_doc.filePath()).completeBaseName() + QStringLiteral(".nc"),
        QStringLiteral("G-code (*.nc *.gcode);;All files (*)"));
    if (path.isEmpty())
        return;
    for (const char *suffix : {".nc", ".gcode"})
        if (path.endsWith(QLatin1String(suffix), Qt::CaseInsensitive))
            path.chop(int(qstrlen(suffix)));
    const TiledExport r = exportTiled(m_doc, path, tileH);
    if (!r.error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Export failed"),
            QStringLiteral("%1\nSkipped: %2").arg(r.error, r.skipped.join(QStringLiteral(", "))));
        return;
    }
    QString msg = QStringLiteral("Exported %1 toolpath(s) as %2 tile(s) of %3 mm: %4")
                      .arg(r.done.size()).arg(r.files.size()).arg(tileH)
                      .arg(r.files.join(QStringLiteral(", ")));
    if (!r.skipped.isEmpty())
        msg += QStringLiteral("  (skipped: %1)").arg(r.skipped.join(QStringLiteral(", ")));
    statusBar()->showMessage(msg, 10000);
}

void MainWindow::refreshPreview()
{
    if (m_doc.filePath().isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Open a .c2d file first"));
        return;
    }
    const GcodeResult r = exportGcode(m_doc);
    m_canvas->setToolpathPreview(r.ops);
    m_iso->setJob(r.ops, m_doc.boardWidth(), m_doc.boardHeight(),
                  m_doc.params().value("thickness").toDouble());
    m_sim->setJob(r.ops, toolGeometry(m_doc), m_doc.boardWidth(), m_doc.boardHeight(),
                  m_doc.params().value("thickness").toDouble());
    m_isoStale = false;
    QString msg = QStringLiteral("Preview: %1 toolpath(s)").arg(r.done.size());
    if (!r.skipped.isEmpty())
        msg += QStringLiteral(" — skipped: %1").arg(r.skipped.join(QStringLiteral(", ")));
    statusBar()->showMessage(msg, 6000);
}

void MainWindow::showMachinePanel()
{
    if (m_mcDock)
        m_mcDock->raise();
}

void MainWindow::showToolpathPreview()
{
    m_previewAct->setChecked(true);
    refreshPreview();
    m_isoDock->raise();   // --shot … preview: capture the 3D tab too
}

void MainWindow::refreshIso()
{
    m_isoStale = false;
    if (m_doc.filePath().isEmpty())
        return;
    const GcodeResult r = exportGcode(m_doc);
    m_iso->setJob(r.ops, m_doc.boardWidth(), m_doc.boardHeight(),
                  m_doc.params().value("thickness").toDouble());
    m_sim->setJob(r.ops, toolGeometry(m_doc), m_doc.boardWidth(), m_doc.boardHeight(),
                  m_doc.params().value("thickness").toDouble());
}

void MainWindow::showSimulation()
{
    if (m_isoStale)
        refreshIso();
    m_simDock->raise();
    m_sim->simulateBlocking();   // --shot … simulation: result is in the grab
}

void MainWindow::markIsoStale()
{
    if (m_isoDock->isVisible() || m_simDock->isVisible())
        refreshIso();
    else
        m_isoStale = true;
}

void MainWindow::updateTitle()
{
    QString t = QStringLiteral("phobiCCCP");
    if (!m_doc.filePath().isEmpty())
        t = QFileInfo(m_doc.filePath()).fileName() + (m_dirty ? QStringLiteral(" *") : QString())
            + QStringLiteral(" — phobiCCCP");
    setWindowTitle(t);
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
    if (!m_doc.save(m_doc.filePath(), &err)) {
        QMessageBox::warning(this, QStringLiteral("Save failed"), err);
    } else {
        m_bg.saveTo(m_doc.filePath());
        m_dirty = false;
        updateTitle();
        statusBar()->showMessage(
            QStringLiteral("Saved %1").arg(m_doc.filePath()), 5000);
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
        m_bg.saveTo(path);
        m_dirty = false;
        updateTitle();
        statusBar()->showMessage(QStringLiteral("Saved %1").arg(path), 5000);
    }
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
    m_bg.loadFrom(path);
    m_canvas->setDocument(&m_doc);
    m_props->setDocument(&m_doc);
    m_tp->setDocument(&m_doc);
    m_machine->setDocument(&m_doc);
    m_dirty = false;
    updateTitle();
    refreshInfo();
    if (m_previewAct->isChecked())
        refreshPreview();
    else
        markIsoStale();
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
