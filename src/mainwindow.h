#pragma once
#include "c2ddocument.h"
#include "canvas.h"
#include <QMainWindow>

class QDockWidget;
class QPlainTextEdit;

namespace c2d {

class ToolpathPanel;

// Main window. Every major surface (canvas, document info, toolpaths) is a
// dock widget, so each can be detached to any screen, resized freely, and -
// thanks to native window frames on floating docks - maximized/fullscreened
// there. Window geometry and the dock layout persist across sessions.
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    void openFile(const QString &path);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onOpen();
    void onSave();
    void onSaveAs();
    void onElementsMoved(const QList<int> &indices, const QList<QPointF> &deltas);
    void onScaleSelected();
    void onAddCircle();
    void onAddRectangle();
    void onDeleteSelected();
    void onUndo();
    void onRedo();
    void onExportGcode();
    void onSendToGrbl();

private:
    // One undo step = the full editable state (elements + toolpaths; params
    // and board size are not editable). QJson values are implicitly shared,
    // so snapshots are cheap.
    struct Snapshot {
        QVector<Element> elements;
        QVector<Toolpath> toolpaths;
    };

    QDockWidget *makeDock(const QString &title, const QString &objectName,
                          QWidget *widget, Qt::DockWidgetArea area);
    void refreshInfo();
    void setDirty(bool dirty);
    bool requireSelection(QList<int> *indices);
    bool maybeSave();   // false = user cancelled, abort the pending action
    void pushUndo();
    void applySnapshot(const Snapshot &snap);
    QString buildGcode();   // CAM over all enabled toolpaths; empty on failure

    Canvas *m_canvas;
    QPlainTextEdit *m_info;      // params + item summary sidebar
    ToolpathPanel *m_toolpaths;  // editable toolpath parameters
    QDockWidget *m_canvasDock;
    QDockWidget *m_infoDock;
    QDockWidget *m_tpDock;
    Document m_doc;
    bool m_dirty = false;
    QList<Snapshot> m_undoStack;
    QList<Snapshot> m_redoStack;
};

} // namespace c2d
