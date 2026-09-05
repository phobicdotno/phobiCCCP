#pragma once
#include "c2ddocument.h"
#include "canvas.h"
#include <QMainWindow>

class QAction;
class QDockWidget;
class QLabel;
class QPlainTextEdit;

namespace c2d {

class IsoPreview;
class MachinePanel;
class PropertiesPanel;
class SimPanel;
class ToolpathPanel;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    void openFile(const QString &path);
    void showToolpathPreview();   // enable the overlay (used by --shot)
    void showMachinePanel();      // raise the Machine tab (used by --shot)
    void showSimulation();        // raise the Simulation tab + run it synchronously (used by --shot)

private slots:
    void onOpen();
    void onSave();
    void onSaveAs();
    void onExportGcode();
    void onExportGcodeTiled();   // one program per tile_height band in Y
    void refreshPreview();

private:
    void refreshInfo();
    void refreshIso();        // rebuild the 3D preview from the current document
    void markIsoStale();      // refresh now if the Preview tab is visible, else defer

    void updateTitle();

    Canvas *m_canvas;
    QPlainTextEdit *m_info;      // params + item summary sidebar
    QLabel *m_cursorLabel;       // live mm position, right side of status bar
    QLabel *m_zoomLabel;         // zoom percentage
    PropertiesPanel *m_props;    // numeric editor for the selection
    ToolpathPanel *m_tp;         // toolpath list + parameter editor
    MachinePanel *m_machine;     // GRBL serial control + streaming
    QDockWidget *m_mcDock = nullptr;
    IsoPreview *m_iso;           // isometric animated route preview
    QDockWidget *m_isoDock;
    SimPanel *m_sim;             // material-removal simulation (heightmap)
    QDockWidget *m_simDock;
    bool m_isoStale = false;
    QAction *m_previewAct;       // toolpath overlay toggle
    bool m_dirty = false;
    Document m_doc;
};

} // namespace c2d
