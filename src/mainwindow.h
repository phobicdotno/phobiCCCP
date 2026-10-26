#pragma once
#include "c2ddocument.h"
#include "backgroundimage.h"
#include "canvas.h"
#include "recovery.h"
#include "padlink.h"
#include <QMainWindow>
#include <QPointer>
#include <memory>

class QAction;
class QDialog;
class QTabWidget;
class QResizeEvent;
class QStackedWidget;
class QSplitter;
class QMenu;
class QMenuBar;
class QToolBar;
class QLabel;
class QPlainTextEdit;
class QTimer;

namespace c2d {

struct GcodeResult;
class IsoPreview;
class ProjectReceiver;
struct ReceivedProject;
class MachinePanel;
class ModelPanel;
class ParametersPanel;
class PropertiesPanel;
class SimPanel;
class ToolStrip;
class ToolpathPanel;
class ShapeStrip;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
    void openFile(const QString &path);
    bool hasDocument() const { return !m_doc.filePath().isEmpty(); }
    void showParameters();          // Design page, Parameters tab
    // A blank design the size of the machine in Settings, not yet saved
    // anywhere: what the window starts on when no file is given.
    // A blank untitled design; the stock is the machine bed unless a size is given (mm).
    bool newUntitled(double width = 0, double height = 0, double thickness = 0);   // false: nothing opened
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
    // Rotate 90°: the whole design (vectors, toolpaths, constraints, 3D
    // model, background) a quarter turn clockwise on the same stock, one
    // undo step. See jobrotate.h.
    void rotateJob();

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
    // `interactive` (every edit with Preview on) schedules the export: a
    // moment after the last edit it runs on a worker with a snapshot of the
    // document, and only the result for the newest edit is shown. The window
    // never waits for it. --shot needs the synchronous form: it screenshots
    // straight after.
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
    // Crash recovery (recovery.h): a copy of the unsaved design every couple
    // of minutes, offered again at start-up if the session died.
    void onSendTo();
    void autosave();
    void offerRecovery();
    // Projects sent from Syntexophobic (File > Receive from Syntexophobic).
    void setReceiving(bool on);
    void onProjectReceived(int id, const ReceivedProject &project);
    // The PhobiCCCP pad app on the same port (projectreceiver.h): status,
    // tool library, design sync, machine switch, and designs sent to be
    // opened (and cut, when the pad asks and this computer allows it).
    void onPadRequest(int id, const QJsonObject &request, const QString &peer);
    QJsonObject padSend(int id, const QJsonObject &request, const QString &peer);
    QJsonObject padInfo();
    void refreshDiscovery();
    // A design sent from a pad stays linked to its .c2d both ways: saving the
    // .c2d here updates the pad design (padShareSaved), and a newer pad design
    // rebuilds the .c2d when it holds nothing the pad cannot (padDesignChanged).
    // The id it was shared under, empty when not shared (`error` says why).
    // `startLink`: a file not linked yet becomes a pad design of its own
    // (File > Send to pad…). `lost`: what the pads get it without.
    QString padShareSaved(const QString &c2dPath, bool startLink = false, QStringList *lost = nullptr,
                          QString *error = nullptr);
    // File > Send to pad…: the open file to a pad, to edit it there.
    void onSendToPad();
    // The design as it is now, saved or not, as a .c2d in the cache (to send whole).
    QString sendCopy(const QString &opened, QString *error);
    bool sameDesign(const QString &opened, QString *error) const;
    qint64 sendToPad(const QString &padId, QString *note, QString *error);
    padlink::Pads m_pads;   // pads in touch lately, and designs waiting for them
    void padDesignChanged(const QString &id);
    void padMoveLinks(const QString &id, const QString &forkId);
    // Machine menu: several machines on this computer (machineprofiles.h).
    void buildMachineMenu();
    void rebuildMachineMenu();
    bool switchMachine(const QString &id, QString *why);
    QMenu *m_machineMenu = nullptr;
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
    ParametersPanel *m_parameters; // user parameters, dimensions, constraints (Design tab)
    ToolpathPanel *m_tp;         // toolpath list + parameter editor
    MachinePanel *m_machine;     // GRBL serial control + streaming (Machine page)
    IsoPreview *m_iso;           // isometric animated route preview (Simulate page)
    QSplitter *m_simPage;        // 3D route | material removal
    SimPanel *m_sim;             // material-removal simulation (Simulate page)
    ToolStrip *m_toolStrip;      // tools in use, beside the Simulate views
    QStackedWidget *m_stack;     // canvas / simulation / machine pages
    QSplitter *m_canvasPage;     // canvas | right column
    QStackedWidget *m_rightStack; // Design page / Toolpaths page
    QSplitter *m_designPage;     // Properties over Document / Model tabs
    QWidget *m_tpPage;
    QTabWidget *m_docTabs;
    ShapeStrip *m_shapes = nullptr;
    bool m_vectorsWrapped = false;
    bool m_optionsWrapped = false;
    QToolBar *m_optionsBar = nullptr;   // Delete, Snap, Fit, Rotate, Preview, Undo (Design and Toolpaths)
    // Puts the Options and Vectors bars on rows of their own when a row has
    // no room for them (resize and mode switches).
    void layoutToolBars();
    QToolBar *m_palette = nullptr;   // drawing tools (Design)
    QToolBar *m_tpBar = nullptr;     // toolpath kinds (Toolpaths)
    QList<int> m_canvasSizes, m_designSizes;   // splitter sizes, kept across mode switches
    void applySplits();
    Mode m_mode = Mode::Design;
    QAction *m_modeActs[4] = {};
    ModelPanel *m_model;         // 3D modelling components + composited relief
    ProjectReceiver *m_receiver = nullptr;   // listens while receiving is on
    // A received project or a pad's send is being answered: its questions and
    // the program export run the event loop, and another one must not open a
    // design (or start the machine) in the middle of it.
    bool m_linkBusy = false;
    // A step here works on the open design across a dialog or an export
    // (Export G-code, the setup sheet): a pad or computer must not replace it.
    int m_holdDoc = 0;
    bool docHeld() const;
    // Where Export G-code starts: beside the design (or the file an untitled
    // one came from), named after it.
    QString gcodeSuggestion() const;
    void commitEdits();
    bool typingPending() const;
    QAction *m_receiveAct = nullptr;
    bool m_isoStale = false;
    // refreshIso runs the event loop behind its progress dialog. An edit made
    // meanwhile (Ctrl+Z held down) only marks the program stale and asks for
    // another pass, which the running call makes once it returns; it never
    // starts a second export inside the first.
    bool m_isoBusy = false, m_isoRerun = false;
    // markIsoStale is called from change notifications - inside
    // QUndoCommand::undo() among them - so it queues the rebuild rather than
    // running that event loop there.
    bool m_isoQueued = false;
    // The background preview export (refreshPreview). Every request bumps
    // m_previewGen; a run's result is shown only if no request came after
    // it. m_previewServed is the generation last shown (or rebuilt by
    // refreshIso), so a request still unserved is restarted when the busy
    // run ends.
    struct PreviewRun;
    QTimer *m_previewTimer = nullptr;
    std::shared_ptr<PreviewRun> m_previewRun;   // the run on the worker, if any
    quint64 m_previewGen = 0, m_previewServed = 0;
    void startPreviewRun();
    void dropPreviewRequests();   // what is about to be built synchronously serves them
    void applyPreview(const GcodeResult &r, bool overlay);
    // An install-update step's process is running: the window refuses to
    // close, because the process goes with it (see installUpdate).
    std::shared_ptr<bool> m_updateStep;
    QPointer<QDialog> m_updateDlg;   // the install dialog while it is open
    QAction *m_previewAct;       // toolpath overlay toggle
    QAction *m_fullScreenAct = nullptr;
    bool m_dirty = false;
    bool m_untitled = false;     // Save asks for a name (the file is scratch)
    QString m_untitledFrom;      // the vector file an untitled design was opened from
    QString m_untitledPath;
    Document m_doc;
    BackgroundImage m_bg;        // tracing background (params + sqlar/background.png)
    Recovery m_recovery;
};

} // namespace c2d
