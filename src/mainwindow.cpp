#include "mainwindow.h"
#include "aboutdialog.h"

#include "exportprogress.h"

#include <utility>
#include "backgrounddialog.h"
#include "gcodeexport.h"
#include "isopreview.h"
#include "machinepanel.h"
#include "modelpanel.h"
#include "tiling.h"
#include <QInputDialog>
#include "propertiespanel.h"
#include "simpanel.h"
#include "toolpathpanel.h"
#include "vectoractions.h"
#include "importui.h"
#include "setupsheet.h"
#include <QDesktopServices>
#include <QUrl>

#include <QApplication>
#include <QFile>
#include <QDir>
#include <QCloseEvent>
#include <QFileInfo>
#include <QDockWidget>
#include <QFileDialog>
#include <QMenu>
#include <QMenuBar>
#include <QToolButton>
#include <QTabBar>
#include <functional>
#include <QSettings>
#include <QTimer>
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
#include <QDoubleSpinBox>
#include <QToolBar>
#include <QUndoStack>
#include <QtMath>

#include <algorithm>

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
    } else if (kind == "ellipse") {
        p.drawEllipse(QRectF(2.5, 5.5, 15, 9));
    } else if (kind == "slot") {
        p.drawRoundedRect(QRectF(2.5, 6, 15, 8), 4, 4);
        QPen thin(QColor(0x9f, 0xc8, 0xf2), 1.0);
        p.setPen(thin);
        p.drawLine(QLineF(6.5, 10, 13.5, 10));
    } else if (kind == "arc") {
        p.drawArc(QRectF(3, 5, 14, 14), 20 * 16, 140 * 16);
        p.setBrush(QColor(0xd8, 0xdc, 0xe4));
        for (const QPointF &v : {QPointF(3.6, 9.6), QPointF(10, 5), QPointF(16.4, 9.6)})
            p.drawEllipse(v, 1.5, 1.5);
    } else if (kind == "measure") {
        p.drawLine(QLineF(3, 15, 17, 5));
        p.drawLine(QLineF(1.5, 12.8, 4.5, 17.2));
        p.drawLine(QLineF(15.5, 2.8, 18.5, 7.2));
    } else if (kind == "trim") {
        p.drawLine(QLineF(10, 2, 10, 18));
        p.drawLine(QLineF(2, 10, 10, 10));
        QPen cut(QColor(0xe0, 0x50, 0x50), 1.6, Qt::DotLine);
        p.setPen(cut);
        p.drawLine(QLineF(10, 10, 18, 10));
    } else if (kind == "extend") {
        p.drawLine(QLineF(17, 2, 17, 18));
        p.drawLine(QLineF(2, 10, 9, 10));
        QPen ext(QColor(0x9f, 0xc8, 0xf2), 1.6, Qt::DashLine);
        p.setPen(ext);
        p.drawLine(QLineF(9, 10, 17, 10));
    } else if (kind == "break") {
        p.drawLine(QLineF(10, 2, 10, 18));
        p.drawLine(QLineF(2, 10, 8.5, 10));
        p.drawLine(QLineF(11.5, 10, 18, 10));
        p.setBrush(QColor(0xd8, 0xdc, 0xe4));
        p.drawEllipse(QPointF(10, 10), 1.6, 1.6);
    } else if (kind == "polygon") {
        QPolygonF hex;
        for (int i = 0; i < 6; ++i) {
            const double a = M_PI / 3 * i - M_PI / 6;
            hex << QPointF(10 + 6.5 * qCos(a), 10 + 6.5 * qSin(a));
        }
        p.drawPolygon(hex);
    } else if (kind == "point") {
        p.drawLine(QLineF(5, 5, 15, 15));
        p.drawLine(QLineF(5, 15, 15, 5));
    } else if (kind == "spline") {
        QPainterPath pp(QPointF(2.5, 14));
        pp.cubicTo(5, 4, 9, 4, 10, 10);
        pp.cubicTo(11, 16, 15, 16, 17.5, 6);
        p.drawPath(pp);
        p.setBrush(QColor(0xd8, 0xdc, 0xe4));
        for (const QPointF &v : {QPointF(2.5, 14), QPointF(10, 10), QPointF(17.5, 6)})
            p.drawEllipse(v, 1.5, 1.5);
    } else if (kind == "conic") {
        p.save();
        p.setPen(QPen(QColor(0x8a, 0x90, 0x9c), 1, Qt::DashLine));
        p.drawLine(QLineF(3, 16, 10, 4));
        p.drawLine(QLineF(10, 4, 17, 16));
        p.restore();
        QPainterPath pp(QPointF(3, 16));
        pp.cubicTo(6, 9, 14, 9, 17, 16);
        p.drawPath(pp);
    } else if (kind == "path") {
        QPainterPath pp(QPointF(3, 16));
        pp.lineTo(8, 6);
        pp.lineTo(12, 12);
        pp.lineTo(17, 4);
        p.drawPath(pp);
        p.setBrush(QColor(0xd8, 0xdc, 0xe4));
        for (const QPointF &v : {QPointF(3, 16), QPointF(8, 6), QPointF(12, 12), QPointF(17, 4)})
            p.drawEllipse(v, 1.6, 1.6);
    } else if (kind == "nodes") {
        // Node editor: a curve with an anchor square and a tangent handle.
        QPainterPath pp(QPointF(3, 16));
        pp.cubicTo(QPointF(6, 4), QPointF(14, 4), QPointF(17, 16));
        p.drawPath(pp);
        QPen thin(QColor(0x9f, 0xc8, 0xf2), 1.0);
        p.setPen(thin);
        p.drawLine(QLineF(5, 10, 15, 10));
        p.setBrush(QColor(0x9f, 0xc8, 0xf2));
        p.drawEllipse(QPointF(5, 10), 1.7, 1.7);
        p.drawEllipse(QPointF(15, 10), 1.7, 1.7);
        p.setPen(pen);
        p.setBrush(QColor(0xd8, 0xdc, 0xe4));
        p.drawRect(QRectF(8, 8, 4, 4));
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
    } else if (kind == "properties") {      // three sliders
        for (int i = 0; i < 3; ++i) {
            const double y = 5 + i * 5, k = (i == 1) ? 13 : (i == 0 ? 7 : 10);
            p.drawLine(QLineF(3, y, 17, y));
            p.setBrush(QColor(0xd8, 0xdc, 0xe4));
            p.drawEllipse(QPointF(k, y), 1.8, 1.8);
            p.setBrush(Qt::NoBrush);
        }
    } else if (kind == "toolpaths") {       // pocket zig-zag inside a boundary
        p.drawRect(QRectF(3, 3, 14, 14));
        QPainterPath zz(QPointF(6, 6));
        zz.lineTo(14, 6); zz.lineTo(14, 10); zz.lineTo(6, 10); zz.lineTo(6, 14); zz.lineTo(14, 14);
        p.setPen(QPen(QColor(0xf0, 0xa0, 0x30), 1.6));
        p.drawPath(zz);
    } else if (kind == "document") {        // page with a folded corner
        QPainterPath pg(QPointF(5, 2.5));
        pg.lineTo(12, 2.5); pg.lineTo(16, 6.5); pg.lineTo(16, 17.5); pg.lineTo(5, 17.5);
        pg.closeSubpath();
        p.drawPath(pg);
        p.drawLine(QLineF(12, 2.5, 12, 6.5));
        p.drawLine(QLineF(12, 6.5, 16, 6.5));
        p.drawLine(QLineF(7.5, 10, 13.5, 10));
        p.drawLine(QLineF(7.5, 13.5, 13.5, 13.5));
    } else if (kind == "machine") {         // gantry, spindle and bit over the bed
        p.drawLine(QLineF(2, 17, 18, 17));
        p.drawLine(QLineF(4, 17, 4, 4));
        p.drawLine(QLineF(16, 17, 16, 4));
        p.drawLine(QLineF(4, 4, 16, 4));
        p.drawRect(QRectF(8, 5, 4, 6));
        p.drawLine(QLineF(10, 11, 10, 14.5));
    } else if (kind == "preview") {         // isometric cube
        const QPointF t(10, 2.5), l(3.5, 6.2), r(16.5, 6.2), c(10, 10),
                      bl(3.5, 13.8), br(16.5, 13.8), b(10, 17.5);
        p.drawPolygon(QPolygonF({t, r, br, b, bl, l}));
        p.drawLine(QLineF(l, c)); p.drawLine(QLineF(r, c)); p.drawLine(QLineF(c, b));
    } else if (kind == "simulation") {      // play button
        p.drawEllipse(QRectF(2.5, 2.5, 15, 15));
        p.setBrush(QColor(0xd8, 0xdc, 0xe4));
        p.drawPolygon(QPolygonF({QPointF(8, 6.5), QPointF(14, 10), QPointF(8, 13.5)}));
    } else if (kind == "model") {           // relief rising off the stock
        QPainterPath m(QPointF(2, 15));
        m.cubicTo(QPointF(6, 15), QPointF(7, 5), QPointF(10, 5));
        m.cubicTo(QPointF(13, 5), QPointF(14, 15), QPointF(18, 15));
        p.drawPath(m);
        p.drawLine(QLineF(2, 17.5, 18, 17.5));
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

    m_model = new ModelPanel(this);
    m_modelDock = new QDockWidget(QStringLiteral("Model"), this);
    m_modelDock->setWidget(m_model);
    m_modelDock->setFeatures(QDockWidget::DockWidgetMovable);
    addDockWidget(Qt::RightDockWidgetArea, m_modelDock);

    tabifyDockWidget(tpDock, dock);     // Toolpaths / Document / Machine / Preview / Simulation / Model tabs
    tabifyDockWidget(dock, mcDock);
    tabifyDockWidget(mcDock, m_isoDock);
    tabifyDockWidget(m_isoDock, m_simDock);
    tabifyDockWidget(m_simDock, m_modelDock);
    tpDock->raise();

    // Panel tabs along the top, each with an icon and a bold label; the
    // separators beside and between the docks stay draggable.
    setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);
    setDockOptions(dockOptions() | QMainWindow::AnimatedDocks);
    const QList<QPair<QDockWidget *, const char *>> dockIcons = {
        {propsDock, "properties"}, {tpDock, "toolpaths"}, {dock, "document"},
        {mcDock, "machine"}, {m_isoDock, "preview"}, {m_simDock, "simulation"},
        {m_modelDock, "model"}};
    for (const auto &di : dockIcons)
        di.first->setWindowIcon(toolIcon(QString::fromLatin1(di.second)));
    // QMainWindow draws tabified docks on its own private tab bars and leaves
    // their icons blank, and it rebuilds those bars whenever docks move: put
    // each dock's icon on its tab now and again after every rearrangement.
    auto tabIcons = [this] {
        for (QTabBar *bar : findChildren<QTabBar *>(QString(), Qt::FindDirectChildrenOnly))
            for (int i = 0; i < bar->count(); ++i)
                for (QDockWidget *d : findChildren<QDockWidget *>())
                    if (d->windowTitle() == bar->tabText(i))
                        bar->setTabIcon(i, d->windowIcon());
    };
    QTimer::singleShot(0, this, tabIcons);
    for (const auto &di : dockIcons) {
        connect(di.first, &QDockWidget::dockLocationChanged, this,
                [tabIcons, this] { QTimer::singleShot(0, this, tabIcons); });
        connect(di.first, &QDockWidget::topLevelChanged, this,
                [tabIcons, this] { QTimer::singleShot(0, this, tabIcons); });
    }
    setStyleSheet(styleSheet() + QStringLiteral(
        "QMainWindow > QTabBar::tab { font-weight: bold; padding: 5px 10px; }"
        "QMainWindow > QTabBar { qproperty-iconSize: 18px 18px; }"
        "QMainWindow::separator { width: 6px; height: 6px; }"
        "QMainWindow::separator:hover { background: #4a90d9; }"));
    // The 3D preview (and the simulation's program) is only rebuilt while one
    // of those tabs is showing; catch up when raised after edits behind it.
    for (QDockWidget *d : {m_isoDock, m_simDock})
        connect(d, &QDockWidget::visibilityChanged, this, [this](bool v) {
            if (v && m_isoStale)
                refreshIso();
        });

    connect(m_canvas, &Canvas::selectionChangedIds,
            m_props, &PropertiesPanel::setSelection);
    connect(m_canvas, &Canvas::selectionChangedIds,
            m_model, &ModelPanel::setSelection);
    // Component edits dirty the document (they are saved into the .c2d) and
    // change what the 3D toolpaths cut.
    connect(m_model, &ModelPanel::modelChanged, this, [this] {
        m_dirty = true;
        updateTitle();
    });
    connect(m_model, &ModelPanel::modelRebuilt, this, [this] {
        if (m_previewAct->isChecked())
            refreshPreview();
        else
            markIsoStale();
    });
    // Real machine work position → orange crosshair in the 3D preview.
    connect(m_machine, &MachinePanel::livePosition,
            m_iso, &IsoPreview::setLivePosition);

    // Shortcuts are set on the returned QAction rather than passed to
    // addAction: the (text, receiver, method, shortcut) overload is deprecated
    // from Qt 6.4 and the (text, shortcut, receiver, method) replacement only
    // exists from 6.3, so neither is both warning-free and version-neutral.
    auto *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    fileMenu->addAction(QStringLiteral("&New…"), this, &MainWindow::onNew)
            ->setShortcut(QKeySequence::New);
    fileMenu->addAction(QStringLiteral("&Open…"), this, &MainWindow::onOpen)
            ->setShortcut(QKeySequence::Open);
    m_recentMenu = fileMenu->addMenu(QStringLiteral("Open &Recent"));
    m_recentMenu->setToolTipsVisible(true);   // full path, since names repeat
    rebuildRecentMenu();
    fileMenu->addAction(QStringLiteral("&Save"), this, &MainWindow::onSave)
            ->setShortcut(QKeySequence::Save);
    fileMenu->addAction(QStringLiteral("Save &As…"), this, &MainWindow::onSaveAs)
            ->setShortcut(QKeySequence::SaveAs);
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("Import &SVG…"), this, [this] {
        importVectorFile(this, m_canvas, &m_doc, {}, QStringLiteral("SVG (*.svg)")); });
    fileMenu->addAction(QStringLiteral("Import &DXF…"), this, [this] {
        importVectorFile(this, m_canvas, &m_doc, {}, QStringLiteral("DXF (*.dxf)")); });
    installImportDropHandler(this, m_canvas, &m_doc, [this](const QString &p) { openFile(p); });
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("Export &G-code…"), this,
                        &MainWindow::onExportGcode)
            ->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_G));
    fileMenu->addAction(QStringLiteral("Export G-code (&tiled)…"), this,
                        &MainWindow::onExportGcodeTiled);
    fileMenu->addAction(QStringLiteral("Setup S&heet…"), this, [this] {
        // Fusion's setup sheet: a printable page of the job for the machine.
        if (m_doc.toolpaths().isEmpty()) {
            QMessageBox::information(this, QStringLiteral("Setup sheet"),
                                     QStringLiteral("There are no toolpaths to describe yet."));
            return;
        }
        const QFileInfo fi(m_doc.filePath());
        const QString title = fi.fileName().isEmpty() ? QStringLiteral("Untitled") : fi.completeBaseName();
        const QString dest = QFileDialog::getSaveFileName(
            this, QStringLiteral("Save setup sheet"),
            (fi.fileName().isEmpty() ? QDir::homePath() : fi.absolutePath())
                + QLatin1Char('/') + title + QStringLiteral(" setup sheet.html"),
            QStringLiteral("HTML (*.html)"));
        if (dest.isEmpty())
            return;
        QFile f(dest);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QMessageBox::warning(this, QStringLiteral("Setup sheet"),
                                 QStringLiteral("Could not write %1").arg(dest));
            return;
        }
        f.write(setupSheetHtml(m_doc, title).toUtf8());
        f.close();
        statusBar()->showMessage(QStringLiteral("Setup sheet saved to %1").arg(dest), 6000);
        QDesktopServices::openUrl(QUrl::fromLocalFile(dest));
    });
    fileMenu->addSeparator();
    installImageMenus(fileMenu, this, m_canvas, &m_doc, &m_bg);   // backgrounddialog.cpp
    fileMenu->addSeparator();
    // close(), not quit(): quit() skips closeEvent and with it the
    // unsaved-changes prompt.
    fileMenu->addAction(QStringLiteral("E&xit"), this, &QWidget::close)
            ->setShortcut(QKeySequence::Quit);

    // Edit menu: undo/redo backed by the canvas undo stack.
    // Help goes last on the bar; other menus are still being added below and
    // by the install* helpers, so append it once construction has finished.
    QTimer::singleShot(0, this, [this] {
        auto *helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
        helpMenu->addAction(QStringLiteral("&About phobiCCCP"), this, [this] {
            AboutDialog dlg(this);
            dlg.exec();
        });
    });
    auto *editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
    QAction *undoAct = m_canvas->undoStack()->createUndoAction(this, QStringLiteral("&Undo"));
    undoAct->setShortcut(QKeySequence::Undo);
    QAction *redoAct = m_canvas->undoStack()->createRedoAction(this, QStringLiteral("&Redo"));
    redoAct->setShortcut(QKeySequence::Redo);
    editMenu->addAction(undoAct);
    editMenu->addAction(redoAct);
    editMenu->addSeparator();
    QAction *constrAct = editMenu->addAction(QStringLiteral("&Construction"), this, [this] {
        m_canvas->toggleConstruction(m_canvas->selectedElementIds());
    });
    constrAct->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_X));
    constrAct->setToolTip(QStringLiteral(
        "Make the selected vectors construction geometry (dashed, never machined), "
        "or normal again  (Shift+X)"));

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
    QAction *circleAct = addTool(QStringLiteral("Circle"), QStringLiteral("circle"),
            Canvas::DrawCircle, Qt::Key_C,
            QStringLiteral("Circle (C) — arrow for Center / 2-Point / 3-Point / 2-Tangent / 3-Tangent"));
    QAction *rectAct = addTool(QStringLiteral("Rect"), QStringLiteral("rect"),
            Canvas::DrawRect, Qt::Key_R,
            QStringLiteral("Rectangle (R) — arrow for 2-Point / 3-Point / Center"));
    // Fusion's sketch variants: a drop-down on each button picks the mode and
    // switches to the tool; the button itself reuses the last mode picked.
    auto addModes = [&](QAction *toolAct, const QList<QPair<QString, int>> &modes,
                        const std::function<void(int)> &apply) {
        auto *menu = new QMenu(this);
        auto *modeGrp = new QActionGroup(menu);
        for (const auto &m : modes) {
            QAction *ma = menu->addAction(m.first);
            ma->setCheckable(true);
            modeGrp->addAction(ma);
            const int mode = m.second;
            connect(ma, &QAction::triggered, this, [toolAct, apply, mode] {
                apply(mode);
                if (!toolAct->isChecked())
                    toolAct->trigger();
            });
        }
        menu->actions().first()->setChecked(true);
        if (auto *btn = qobject_cast<QToolButton *>(palette->widgetForAction(toolAct))) {
            btn->setMenu(menu);
            btn->setPopupMode(QToolButton::MenuButtonPopup);
        }
    };
    addModes(circleAct,
             {{QStringLiteral("Center Diameter Circle"), Canvas::CircleCenterDiameter},
              {QStringLiteral("2-Point Circle"), Canvas::Circle2Point},
              {QStringLiteral("3-Point Circle"), Canvas::Circle3Point},
              {QStringLiteral("2-Tangent Circle"), Canvas::Circle2Tangent},
              {QStringLiteral("3-Tangent Circle"), Canvas::Circle3Tangent}},
             [this](int m) { m_canvas->setCircleMode(Canvas::CircleMode(m)); });
    addModes(rectAct,
             {{QStringLiteral("2-Point Rectangle"), Canvas::Rect2Point},
              {QStringLiteral("3-Point Rectangle"), Canvas::Rect3Point},
              {QStringLiteral("Center Rectangle"), Canvas::RectCenter}},
             [this](int m) { m_canvas->setRectMode(Canvas::RectMode(m)); });
    QAction *polyAct = addTool(QStringLiteral("Polygon"), QStringLiteral("polygon"),
            Canvas::DrawPolygon, Qt::Key_P,
            QStringLiteral("Polygon (P) — arrow for Inscribed / Circumscribed / Edge"));
    addModes(polyAct,
             {{QStringLiteral("Inscribed Polygon"), Canvas::PolyInscribed},
              {QStringLiteral("Circumscribed Polygon"), Canvas::PolyCircumscribed},
              {QStringLiteral("Edge Polygon"), Canvas::PolyEdge}},
             [this](int m) { m_canvas->setPolygonMode(Canvas::PolygonMode(m)); });
    addTool(QStringLiteral("Ellipse"), QStringLiteral("ellipse"), Canvas::DrawEllipse, Qt::Key_E,
            QStringLiteral("Ellipse: drag across its bounding box, Ctrl from the center  (E)"));
    QAction *slotAct = addTool(QStringLiteral("Slot"), QStringLiteral("slot"), Canvas::DrawSlot,
            Qt::Key_S,
            QStringLiteral("Slot (S) — arrow for Center to Center / Overall / Center Point / 3-Point Arc"));
    addModes(slotAct,
             {{QStringLiteral("Center to Center Slot"), Canvas::SlotCenterToCenter},
              {QStringLiteral("Overall Slot"), Canvas::SlotOverall},
              {QStringLiteral("Center Point Slot"), Canvas::SlotCenterPoint},
              {QStringLiteral("3-Point Arc Slot"), Canvas::SlotArc3Point}},
             [this](int m) { m_canvas->setSlotMode(Canvas::SlotMode(m)); });
    QAction *arcAct = addTool(QStringLiteral("Arc"), QStringLiteral("arc"), Canvas::DrawArc,
            Qt::Key_A, QStringLiteral("Arc (A) — arrow for 3-Point / Center Point / Tangent"));
    addModes(arcAct,
             {{QStringLiteral("3-Point Arc"), Canvas::Arc3Point},
              {QStringLiteral("Center Point Arc"), Canvas::ArcCenter},
              {QStringLiteral("Tangent Arc"), Canvas::ArcTangent}},
             [this](int m) { m_canvas->setArcMode(Canvas::ArcMode(m)); });
    addTool(QStringLiteral("Spline"), QStringLiteral("spline"), Canvas::DrawSpline, Qt::Key_K,
            QStringLiteral("Fit-point spline: click the points it passes through; Enter finishes, click near start closes  (K)"));
    addTool(QStringLiteral("Conic"), QStringLiteral("conic"), Canvas::DrawConic, Qt::Key_Q,
            QStringLiteral("Conic curve: click the start, the end, then the apex its tangents meet at; rho in the options bar  (Q)"));
    addTool(QStringLiteral("Point"), QStringLiteral("point"), Canvas::DrawPoint, Qt::Key_O,
            QStringLiteral("Point: click to place a sketch point that drilling toolpaths drill at  (O)"));
    addTool(QStringLiteral("Path"), QStringLiteral("path"), Canvas::DrawPath, Qt::Key_L,
            QStringLiteral("Path: click = corner, click-drag = curve; Enter finishes, click near start closes  (L)"));
    addTool(QStringLiteral("Trim"), QStringLiteral("trim"), Canvas::Trim, Qt::Key_X,
            QStringLiteral("Trim: click the piece of a curve to cut away, up to where other curves cross it  (X)"));
    addTool(QStringLiteral("Extend"), QStringLiteral("extend"), Canvas::Extend, Qt::Key_J,
            QStringLiteral("Extend: click near an open curve's end to run it on to the next curve  (J)"));
    addTool(QStringLiteral("Break"), QStringLiteral("break"), Canvas::Break, Qt::Key_B,
            QStringLiteral("Break: click a curve to split it where other curves cross it  (B)"));
    addTool(QStringLiteral("Measure"), QStringLiteral("measure"), Canvas::Measure, Qt::Key_M,
            QStringLiteral("Measure: drag between two points for the distance, ΔX, ΔY and angle  (M)"));
    addTool(QStringLiteral("Nodes"), QStringLiteral("nodes"), Canvas::NodeEdit, Qt::Key_N,
            QStringLiteral("Edit nodes: drag anchors/handles, double-click to insert, Del to remove, right-click for Corner/Smooth/Symmetric  (N)"));
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
    auto *rho = new QDoubleSpinBox(tb);
    rho->setRange(0.05, 0.95);
    rho->setSingleStep(0.05);
    rho->setDecimals(2);
    rho->setValue(0.5);
    rho->setPrefix(QStringLiteral("rho "));
    rho->setToolTip(QStringLiteral("Conic fullness: 0.5 parabola, lower an ellipse arc, higher a hyperbola"));
    QAction *rhoAct = tb->addWidget(rho);
    connect(rho, &QDoubleSpinBox::valueChanged, this,
            [this](double v) { m_canvas->setConicRho(v); });
    QAction *sidesSep = tb->addSeparator();

    // The side count only means something while drawing polygons and rho
    // while drawing conics, so each shows (with the separator) only then.
    auto showSides = [sidesAct, rhoAct, sidesSep](Canvas::Tool t) {
        sidesAct->setVisible(t == Canvas::DrawPolygon);
        rhoAct->setVisible(t == Canvas::DrawConic);
        sidesSep->setVisible(t == Canvas::DrawPolygon || t == Canvas::DrawConic);
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
        // When the 3D model has to be recomposited, wait for the worker:
        // refreshing now would build it synchronously here and again there.
        if (!m_model->documentChanged()) {
            if (m_previewAct->isChecked())
                refreshPreview();
            else
                markIsoStale();
        }
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
    const GcodeResult r = exportWithProgress(this, m_doc, QStringLiteral("Export G-code"));
    if (r.cancelled) {
        statusBar()->showMessage(QStringLiteral("Export cancelled"), 5000);
        return;                       // a partial program must never be written
    }
    if (r.done.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Export G-code"),
            QStringLiteral("No exportable toolpaths.\n\nSupported: contour, pocket, "
                           "cutout, drill, keyhole, texture, v-carve, engrave.\n"
                           "Skipped:\n  %1")
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
    // Unchecked and unflushed, a full disk or a stick pulled mid-write left a
    // program ending in the middle of a block - no retract, no spindle stop -
    // and the status bar said it had been exported.
    const QByteArray bytes = r.gcode.toUtf8();
    if (f.write(bytes) != bytes.size() || !f.flush()) {
        QMessageBox::warning(this, QStringLiteral("Export failed"), f.errorString());
        f.close();
        return;
    }
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
    const TiledExport r = exportTiledWithProgress(this, m_doc, path, tileH,
                                                  QStringLiteral("Export G-code (tiled)"));
    if (r.error == QLatin1String("cancelled")) {
        statusBar()->showMessage(QStringLiteral("Export cancelled"), 5000);
        return;
    }
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
    // A previous export under this base name may have needed more tiles. Those
    // files are still there, still look like part of this job, and still say
    // "(tile 4/5)" inside. The save dialog only asked about <base>.nc, which is
    // never written, so the user was never warned about any of them.
    if (!r.stale.isEmpty())
        QMessageBox::warning(
            this, QStringLiteral("Older tiles are still in that folder"),
            QStringLiteral("This export made %1 tile(s). These files are left over "
                           "from an earlier, longer export and are NOT part of it:\n\n%2\n\n"
                           "Delete them before running the job.")
                .arg(r.files.size())
                .arg(r.stale.join(QStringLiteral("\n"))));
}

void MainWindow::refreshPreview(bool interactive)
{
    if (m_doc.filePath().isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Open a .c2d file first"));
        return;
    }
    const GcodeResult r = interactive
        ? exportWithProgress(this, m_doc, QStringLiteral("Updating preview"))
        : exportGcode(m_doc);
    if (r.cancelled) {
        // Keep the preview that is already on screen rather than replacing it
        // with half a program.
        statusBar()->showMessage(QStringLiteral("Preview update cancelled"), 5000);
        return;
    }
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
    refreshPreview(false);         // --shot screenshots straight after
    m_isoDock->raise();   // --shot … preview: capture the 3D tab too
}

void MainWindow::refreshIso(bool interactive)
{
    m_isoStale = false;
    if (m_doc.filePath().isEmpty())
        return;
    const GcodeResult r = interactive
        ? exportWithProgress(this, m_doc, QStringLiteral("Updating 3D preview"))
        : exportGcode(m_doc);
    if (r.cancelled) {
        statusBar()->showMessage(QStringLiteral("Preview update cancelled"), 5000);
        return;
    }
    m_iso->setJob(r.ops, m_doc.boardWidth(), m_doc.boardHeight(),
                  m_doc.params().value("thickness").toDouble());
    m_sim->setJob(r.ops, toolGeometry(m_doc), m_doc.boardWidth(), m_doc.boardHeight(),
                  m_doc.params().value("thickness").toDouble());
}

void MainWindow::showSimulation()
{
    if (m_isoStale)
        refreshIso(false);         // --shot: no dialog, the grab follows
    m_simDock->raise();
    m_sim->simulateBlocking();   // --shot … simulation: result is in the grab
}

void MainWindow::showModel()
{
    m_modelDock->raise();
    m_model->rebuildBlocking();   // --shot … model: the relief is in the grab
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

void MainWindow::onNew()
{
    if (!confirmDiscard(QStringLiteral("Save before starting a new file?")))
        return;
    bool ok = false;
    const double w = QInputDialog::getDouble(this, QStringLiteral("New design"),
        QStringLiteral("Stock width (mm):"), 300, 1, 5000, 2, &ok);
    if (!ok) return;
    const double h = QInputDialog::getDouble(this, QStringLiteral("New design"),
        QStringLiteral("Stock height (mm):"), 300, 1, 5000, 2, &ok);
    if (!ok) return;
    const double t = QInputDialog::getDouble(this, QStringLiteral("New design"),
        QStringLiteral("Stock thickness (mm):"), 19, 0.1, 500, 2, &ok);
    if (!ok) return;
    QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save new design as"), QStringLiteral("untitled.c2d"),
        QStringLiteral("Carbide Create (*.c2d)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(QStringLiteral(".c2d"), Qt::CaseInsensitive))
        path += QStringLiteral(".c2d");
    QString err;
    if (!c2d::Document::createBlank(path, w, h, t, &err)) {
        QMessageBox::warning(this, QStringLiteral("New failed"), err);
        return;
    }
    m_dirty = false;   // nothing to discard: the question was already asked
    openFile(path);
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
        saveExtras(m_doc.filePath());
    }
}

// The relief and the background live in the same container but are written
// after Document::save has already renamed the finished file into place, so
// they are a second, unprotected pass. Both return bool and both were being
// discarded: a failure there meant the user was told "Saved", the dirty flag
// was cleared, and the 3D model they had just sculpted was silently not in
// the file. Report it, and leave the document dirty so the next Ctrl+S retries.
void MainWindow::saveExtras(const QString &path)
{
    QString bgErr, mdErr;
    const bool bgOk = m_bg.saveTo(path, &bgErr);
    const bool mdOk = m_model->saveTo(path, &mdErr);
    if (!bgOk || !mdOk) {
        QMessageBox::warning(
            this, QStringLiteral("Saved incompletely"),
            QStringLiteral("%1 was written, but %2 could not be:\n%3")
                .arg(path,
                     !mdOk ? QStringLiteral("the 3D model")
                           : QStringLiteral("the background image"),
                     !mdOk ? mdErr : bgErr));
        updateTitle();
        return;                       // still dirty: the next save retries
    }
    m_dirty = false;
    updateTitle();
    statusBar()->showMessage(QStringLiteral("Saved %1").arg(path), 5000);
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
        saveExtras(path);
    }
}

// m_dirty was tracked and shown in the title bar but never acted on: opening
// another document, picking one from Open Recent, dropping a file on the
// window or closing it all discarded an afternoon of edits without a word.
bool MainWindow::confirmDiscard(const QString &what)
{
    if (!m_dirty)
        return true;
    const QString name = m_doc.filePath().isEmpty()
                             ? QStringLiteral("This document")
                             : QFileInfo(m_doc.filePath()).fileName();
    const auto r = QMessageBox::warning(
        this, QStringLiteral("Unsaved changes"),
        QStringLiteral("%1 has unsaved changes.\n\n%2").arg(name, what),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);
    if (r == QMessageBox::Cancel)
        return false;
    if (r == QMessageBox::Save) {
        onSave();
        return !m_dirty;              // a failed save must not lose the edits
    }
    return true;
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    if (confirmDiscard(QStringLiteral("Save before closing?")))
        e->accept();
    else
        e->ignore();
}

void MainWindow::openFile(const QString &path)
{
    if (!confirmDiscard(QStringLiteral("Save before opening another file?")))
        return;
    qDebug() << "[diag] SQL drivers:" << QSqlDatabase::drivers();
    qDebug() << "[diag] opening:" << path;
    QString err;
    // load() clears the document before it validates anything, so loading
    // straight into m_doc and returning on failure left the panels, the canvas
    // and the undo stack pointing at a document that no longer had any
    // contents - and m_doc.filePath() already pointing at the file that failed.
    // The next drag rebuilt an empty drawing; the next Ctrl+S wrote it out.
    // Load into a temporary and commit only once it worked.
    Document next;
    if (!next.load(path, &err)) {
        qDebug() << "[diag] LOAD FAILED:" << err;
        QMessageBox::warning(this, QStringLiteral("Open failed"), err);
        return;
    }
    m_doc = std::move(next);
    qDebug() << "[diag] loaded elements:" << m_doc.elements().size()
             << "toolpaths:" << m_doc.toolpaths().size()
             << "board:" << m_doc.boardWidth() << "x" << m_doc.boardHeight()
             << "params:" << m_doc.params().size();
    m_bg.loadFrom(path);
    m_canvas->setDocument(&m_doc);
    m_props->setDocument(&m_doc);
    m_tp->setDocument(&m_doc);
    m_machine->setDocument(&m_doc);
    m_model->setDocument(&m_doc);
    m_dirty = false;
    rememberRecent(path);
    updateTitle();
    refreshInfo();
    if (m_previewAct->isChecked())
        refreshPreview();
    else
        markIsoStale();
    statusBar()->showMessage(path);
}

// ---- recent files ---------------------------------------------------------

static const int kMaxRecent = 5;

void MainWindow::rememberRecent(const QString &path)
{
    const QString abs = QFileInfo(path).absoluteFilePath();
    QSettings st;
    QStringList recent = st.value(QStringLiteral("recentFiles")).toStringList();
    recent.removeAll(abs);
    recent.prepend(abs);
    while (recent.size() > kMaxRecent)
        recent.removeLast();
    st.setValue(QStringLiteral("recentFiles"), recent);
    rebuildRecentMenu();
}

void MainWindow::rebuildRecentMenu()
{
    if (!m_recentMenu)
        return;
    m_recentMenu->clear();

    QSettings st;
    QStringList recent = st.value(QStringLiteral("recentFiles")).toStringList();
    // Files that have been moved or deleted are dropped rather than offered.
    const qsizetype had = recent.size();
    recent.erase(std::remove_if(recent.begin(), recent.end(),
                                [](const QString &p) { return !QFile::exists(p); }),
                 recent.end());
    if (recent.size() != had)
        st.setValue(QStringLiteral("recentFiles"), recent);

    if (recent.isEmpty()) {
        QAction *none = m_recentMenu->addAction(QStringLiteral("(nothing yet)"));
        none->setEnabled(false);
        return;
    }
    int n = 0;
    for (const QString &p : std::as_const(recent)) {
        // &1..&5 so the list is reachable from the keyboard; the full path is
        // the tooltip, since several projects often share a file name.
        QAction *a = m_recentMenu->addAction(
            QStringLiteral("&%1  %2").arg(++n).arg(QFileInfo(p).fileName()));
        a->setToolTip(p);
        a->setStatusTip(p);
        connect(a, &QAction::triggered, this, [this, p] { openFile(p); });
    }
    m_recentMenu->addSeparator();
    m_recentMenu->addAction(QStringLiteral("&Clear list"), this, [this] {
        QSettings().remove(QStringLiteral("recentFiles"));
        rebuildRecentMenu();
    });
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
