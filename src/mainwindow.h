#pragma once
#include "c2ddocument.h"
#include "backgroundimage.h"
#include "canvas.h"
#include <QMainWindow>

class QAction;
class QTabWidget;
class QResizeEvent;
class QStackedWidget;
class QSplitter;
class QMenu;
class QMenuBar;
class QToolBar;
class QLabel;
class QPlainTextEdit;

namespace c2d {

class IsoPreview;
class MachinePanel;
class ModelPanel;
class PropertiesPanel;
class SimPanel;
class ToolpathPanel;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    void openFile(const QString &path);
    // A blank design the size of the machine in Settings, not yet saved
    // anywhere: what the window starts on when no file is given.
    void newUntitled();
    // show(), or full screen if it was left that way (View > Full Screen).
    void showStartupWindow();
    void showToolpathPreview();   // Toolpaths mode with the overlay on (used by --shot)
    void showMachinePanel(const QString &page = QString());   // Machine mode, optional card page (used by --shot)
    void showSimulation();        // Simulate mode + run it synchronously (used by --shot)
    void showModel();             // Design mode, Model tab, composited synchronously (used by --shot)

    // The window works in one mode at a time, Carbide's order of work:
    // Design and Toolpaths share the canvas with their own side panels,
    // Simulate and Machine are full pages. Ctrl+1…4, or the switch at the
    // top right. `autoRun` lets Toolpaths/Simulate kick off their preview.
    enum class Mode { Design, Toolpaths, Simulate, Machine };
    void setMode(Mode m, bool autoRun = true);
    Mode mode() const { return m_mode; }

private slots:
    void onNew();
    void onOpen();
    void onSave();
    void onSaveAs();
    void onSettings();
    void onExportGcode();
    void onExportGcodeTiled();   // one program per tile_height band in Y
    // `interactive` puts a cancellable progress dialog in front of a long
    // export. --shot needs the synchronous form: it screenshots straight after.
    void refreshPreview(bool interactive = true);

protected:
    void closeEvent(QCloseEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    // Writes the relief and the background into the container that
    // Document::save has already put in place, and reports it if either fails.
    void saveExtras(const QString &path);
    void refreshInfo();
    void refreshIso(bool interactive = true);   // rebuild the 3D preview from the document
    void markIsoStale();      // refresh now if the Preview tab is visible, else defer

    void updateTitle();
    void openPath(const QString &path, bool untitled);
    void removeUntitledFile();    // the scratch file behind an untitled design
    // Ask before throwing edits away. False = the user cancelled.
    bool confirmDiscard(const QString &what);

    // File > Open Recent: the last five documents opened, most recent first,
    // kept in QSettings so the list survives a restart.
    void rememberRecent(const QString &path);
    void rebuildRecentMenu();

    QMenu *m_recentMenu = nullptr;
    QMenuBar *m_menuBar = nullptr;   // in the header widget, not QMainWindow's own
    QLabel *m_fileLabel = nullptr;   // open file name in the header
    QLabel *m_updateLabel = nullptr; // "↑ vX on GitHub", hidden until known
    QStringList m_updateNotes;       // "v1.1.3 — one line" per release newer than this one
    void checkForUpdate();
    void installUpdate(const QString &version);   // one click: fetch, build, install, restart
    static QIcon fullScreenIcon();

    Canvas *m_canvas;
    QPlainTextEdit *m_info;      // params + item summary sidebar
    PropertiesPanel *m_props;    // numeric editor for the selection
    ToolpathPanel *m_tp;         // toolpath list + parameter editor
    MachinePanel *m_machine;     // GRBL serial control + streaming (Machine page)
    IsoPreview *m_iso;           // isometric animated route preview (Simulate page)
    QSplitter *m_simPage;        // 3D route | material removal
    SimPanel *m_sim;             // material-removal simulation (Simulate page)
    QStackedWidget *m_stack;     // canvas / simulation / machine pages
    QSplitter *m_canvasPage;     // canvas | right column
    QStackedWidget *m_rightStack; // Design page / Toolpaths page
    QSplitter *m_designPage;     // Properties over Document / Model tabs
    QWidget *m_tpPage;
    QTabWidget *m_docTabs;
    bool m_vectorsWrapped = false;
    QToolBar *m_palette = nullptr;   // drawing tools (Design)
    QToolBar *m_tpBar = nullptr;     // toolpath kinds (Toolpaths)
    QList<int> m_canvasSizes, m_designSizes;   // splitter sizes, kept across mode switches
    void applySplits();
    Mode m_mode = Mode::Design;
    QAction *m_modeActs[4] = {};
    ModelPanel *m_model;         // 3D modelling components + composited relief
    bool m_isoStale = false;
    QAction *m_previewAct;       // toolpath overlay toggle
    QAction *m_fullScreenAct = nullptr;
    bool m_dirty = false;
    bool m_untitled = false;     // Save asks for a name (the file is scratch)
    QString m_untitledFrom;      // the vector file an untitled design was opened from
    QString m_untitledPath;
    Document m_doc;
    BackgroundImage m_bg;        // tracing background (params + sqlar/background.png)
};

} // namespace c2d
