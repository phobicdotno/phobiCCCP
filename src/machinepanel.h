#pragma once
#include "bitzeropanel.h"
#include "grblstreamer.h"
#include "post_grbl.h"
#include "surfacemap.h"
#include <QVector>
#include <QHash>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QPointer>
#include <QWidget>
#include <functional>

class QCheckBox;
class QDialog;
class QStackedWidget;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QToolButton;
class QTimer;
class QShowEvent;

namespace c2d {

class Gamepad;

class Document;

// Machine control dock: connect to a GRBL controller over serial, jog, set
// work zero, measure tools on the BitSetter, and stream the document's
// generated g-code with automatic tool-change stops — a Carbide-Motion
// replacement.
class JobView;

class MachinePanel : public QWidget
{
    Q_OBJECT
public:
    // Connected and doing something: a program, a macro (probing, BitSetter,
    // park), a probe sequence, homing, a jog or a feed hold. The window
    // refuses to close then.
    bool isBusy() const;
    explicit MachinePanel(QWidget *parent = nullptr);
    // The streamer is a child, so ~QWidget deletes it AFTER this panel's own
    // members are gone, and ~GrblStreamer emits disconnected/streamFinished
    // straight into slots that then touch them. Cut the connections first.
    ~MachinePanel() override;
    void setDocument(Document *doc);
    void showPage(const QString &name);   // "job" / "bitzero" / "bitsetter" (used by --shot)
    GrblStreamer *streamer() const { return m_grbl; }

    // A pad asked to cut the open document (the PhobiCCCP pad app, "Start
    // cutting"). The same program and checks as Run, minus the question on
    // this screen: connected, homed, idle, not busy, no alarm. False with
    // `why` set when it did not start.
    bool startFromRemote(QString *why);
    // The machine/ settings group was swapped for another machine's
    // (machineprofiles.h): disconnect, load it, and connect to the new
    // machine's port if the old one was connected. Refused while busy.
    bool reloadMachine(QString *why = nullptr);
    // For the pad: connected, port, state, homed, busy, job.
    QJsonObject statusJson() const;

signals:
    void livePosition(double wx, double wy, double wz, bool valid);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    enum class Phase { Idle, ParkForTool, MeasureAfterChange, MeasureForRef,
                       MeasureManual, GoToZero,
                       // BitZero stages: the bore's X walls, its Y walls, the
                       // plate top, and the final move back over the corner.
                       BitZeroX, BitZeroY, BitZeroZ, BitZeroHome };

    void refreshPorts();
    void toggleConnect();
    QPushButton *jogButton(const QString &text, char axis, int dir);
    void jogStep(char axis, int dir);
    // Press/release of a jog direction, whatever produced it: an arrow key, a
    // jog button, or a direction on the gamepad. A tap becomes a single step,
    // a hold becomes continuous motion, a release cancels it.
    void beginHoldJog(char axis, int dir);
    void endHoldJog();

    // Gamepad (src/gamepad.h). Buttons are reported by the pad's own numbering,
    // which differs between SNES clones, so every press is logged and the
    // mapping can be overridden in QSettings under gamepad/.
    void gamepadButton(int number, bool pressed);
    void gamepadAxis(int number, int value);
    void loadGamepadMapping();
    void editGamepadMapping();
    void jogIncrement();
    void stopHoldJog();                  // end a running hold-jog (cancel) safely
    QString currentPortName() const;
    void resetFlow();                    // forget tool-change / macro state
    double jogFeed(char axis) const;
    void zero(const QString &axes);
    void goToXY0();
    void measureTool(Phase why);
    void onMacroFinished(bool ok);
    void onToolChange(int tool, int ordinal);
    void parkForTool();                  // spindle off, up, to the tool-change spot
    void askForTool();                   // the tool-change prompt, once parked
    // Idle on a report taken after the last jog went out, with no command
    // unanswered and no jog waiting: GRBL refuses G-code while jogging
    // (error:9), so zeroing and resuming wait for this.
    bool standingStill() const;
    bool movingStill(const QString &what);   // logs and returns true when not standing still
    QString stillMovingWhy() const;
    // Zeroing is refused while a program runs: a bumped pad button would
    // shift the rest of the job. The one exception is Z during a tool-change
    // pause the operator kept open to zero the new bit by hand.
    bool zeroAllowed(bool xy);
    // Feed hold, wait for it to settle (Hold:0 or Idle, at most 5 s), soft
    // reset, then `then`. Used to cancel a job and to disconnect during one.
    void holdResetThen(std::function<void()> then, bool evenAfterStop = false);
    void rehearseToolChange();
    void finishRehearsal(bool ok, bool measured);
    void onStatus(const MachineStatus &st);
    void whenIdle(std::function<void()> fn);
    BitSetterConfig bitSetter() const;
    void loadSettings();
    void saveSettings();
    void updateOffsetLabels();
    void forgetReference();
    // `remoteWhy` set: started from a pad, nothing is asked here and a
    // refusal is written there instead of shown.
    bool runProgram(QString *remoteWhy = nullptr);
    void log(const QString &line);

    Document *m_doc = nullptr;
    GrblStreamer *m_grbl;

    // connection
    QComboBox *m_ports;
    QPushButton *m_connectBtn;
    QLabel *m_state, *m_wAxis[3], *m_mpos, *m_extra;
    void setStatePill(const QString &text, const QString &border);
    QLabel *m_jobName = nullptr, *m_jobInfo = nullptr;
    void refreshJobInfo();
    // The right-hand card of the Machine page: the job, or one of the pages
    // that used to be pop-ups (BitZero probing, BitSetter settings).
    QStackedWidget *m_rightStack = nullptr;
    // The middle column: the jog card, or - while a job streams - the job
    // view (every toolpath and pass with its time left). The zeroing and
    // machine-action buttons that are not in use while cutting hide with
    // it; they come back at a tool-change pause (jog, zero Z) and at the end.
    QStackedWidget *m_midStack = nullptr;
    JobView *m_jobView = nullptr;
    QList<QWidget *> m_idleOnly;
    bool m_jobShown = false;
    void setJobRunning(bool on);
    QWidget *m_jobPage = nullptr;
    QWidget *m_bsPage = nullptr;

    // BitZero V2 work probing (bitzerodialog.h). Each stage is one macro;
    // its [PRB:] contacts are collected here and turned into the next stage
    // or into the G10 L20 that sets the zero.
    struct Hit { double x, y, z; };
    BitZeroPanel *m_bzPanel = nullptr;
    BitZeroConfig m_bz;                  // snapshot taken when Begin is pressed
    BitZeroPanel::Mode m_bzMode = BitZeroPanel::Corner;
    QVector<Hit> m_hits;
    bool m_bzWantX = false, m_bzWantY = false, m_bzWantZ = false;
    double m_bzCx = 0, m_bzCy = 0, m_bzStockZ = 0;   // machine coords found
    double m_bzStartX = 0, m_bzStartY = 0;           // where Begin was pressed
    double m_bzOverZ = 0;                            // V1: over the block (top + 5)
    quint64 m_resets = 0;                            // controller (re)starts seen
    quint64 m_bzStageResets = 0;                     // m_resets when the stage began
    void bitZeroAfterV1(Phase stage);
    static QStringList bitZeroSideLines(char axis, const BitZeroConfig &c, double startAbs);
    void openBitZero();
    void bitZeroBegin();
    void bitZeroStage(Phase stage, QStringList prefix = {});
    void bitZeroAfter(Phase stage, bool ok);
    void bitZeroApply();
    void bitZeroFail(const QString &why);
    static QStringList bitZeroWallLines(char axis, const BitZeroConfig &c);
    static QStringList bitZeroPlateLines(const BitZeroConfig &c);

    // Surface map (surfacemap.h): measured from the BitZero page, kept in
    // QSettings, applied to the program at Run when switched on in the job.
    SurfaceMapProber *m_mapper = nullptr;
    SurfaceMap m_map;
    double mapZeroAt(double wx, double wy) const;
    // The last Z zero: with the map on or off, the offset it sent, the
    // machine XY it was touched at and the work offset Z it left (kept
    // across restarts; judged only while the controller still has that
    // offset). Run refuses a map state that would put Z0 elsewhere.
    void noteZZero(double mx, double my, double sent, double wcoZ);
    QString zzeroGroup() const;
    void loadZZero();
    QString m_connectedPort;             // the port the streamer opened
    bool m_zZeroKnown = false, m_zZeroWithMap = false;
    double m_zZeroSent = 0, m_zZeroMx = 0, m_zZeroMy = 0, m_zZeroWco = 0;
    QCheckBox *m_useMap = nullptr;
    QCheckBox *m_bzAsk = nullptr;
    bool m_liftHold = false;             // Hold with a lift: Z up, spindle off, XY kept; Resume goes back down        // BitZero: wait for OK before moving after the zero
    QLabel *m_mapLabel = nullptr;
    QLabel *m_ovLabel = nullptr;         // live feed/rapid/spindle override, beside the +/- buttons
    void startSurfaceMap();
    void setSurfaceMap(const SurfaceMap &map);
    void refreshMapUi();

    // jog
    QComboBox *m_step, *m_speed;
    QTimer *m_holdTimer;                 // press-and-hold detection
    QTimer *m_holdRepeat;                // feeds short jog increments while held
    Gamepad *m_pad = nullptr;
    QLabel *m_padLabel = nullptr;
    QHash<int, QString> m_padMap;        // button number -> action name
    bool m_padMapping = false;           // mapping dialog open: presses do nothing
    int m_padAxisDir[2] = {0, 0};        // last direction seen on axes 0 and 1
    char m_holdAxis = 0;
    int m_holdDir = 0;
    bool m_holdJogging = false;
    bool m_loading = false;              // loadSettings() in progress

    // bitsetter / tool change
    QToolButton *m_bsEnabled = nullptr;  // BitSetter on/off
    QWidget *m_bsBody = nullptr;         // its settings, dimmed when off
    QDoubleSpinBox *m_bsX, *m_bsY, *m_bsSafeZ, *m_bsFast, *m_bsSlow;
    QDoubleSpinBox *m_tcX, *m_tcY;
    QLabel *m_refLabel, *m_tloLabel;
    QPushButton *m_measureBtn;
    bool m_haveRef = false;
    double m_refZ = 0;                   // machine Z where the reference tool tripped
    Phase m_phase = Phase::Idle;
    int m_pendingTool = -1;
    // The running program's tool changes, in order (toolChanges()), and the
    // index of the one parked at: names the bit when two share a number.
    QVector<Op> m_jobTools;
    int m_pendingOrdinal = -1;
    // BitSetter off: the operator closed the prompt to jog and zero Z by hand
    // and will press Continue; m_zeroedForTool once Z was zeroed meanwhile.
    bool m_manualZero = false;
    bool m_zeroedForTool = false;
    // A manual-zero G10 is on its way; m_zeroedForTool is set on its ok.
    bool m_awaitToolZero = false;
    // The tool-change prompt was answered for the change parked at. It only
    // comes up once the park has arrived, so until then Continue is refused.
    bool m_toolPromptAnswered = false;
    // Whether m_pendingTool has actually been measured on the BitSetter. A
    // failed measurement leaves the *previous* tool's G43.1 in force, so
    // resuming without one cuts as deep as the two tools differ in length.
    bool m_toolMeasured = false;
    bool m_rehearsal = false;            // the running program is a rehearsal
    std::function<void()> m_onIdle;
    quint64 m_idleSeq = 0;      // which whenIdle() step a posted run belongs to
    bool m_idlePosted = false;   // a run of m_onIdle is queued
    QString m_lastState;
    // Carbide Motion's "Initialize machine": after power-up GRBL sits in Alarm
    // until it has homed, and refuses jogs and zeroing (error:9) till then.
    QWidget *m_initBanner = nullptr;
    QPushButton *m_initBtn = nullptr;
    QString m_machineState;      // last reported state, never cleared by whenIdle
    bool needsInit();            // true (and says so in the log) while in Alarm
    void initializeMachine();    // $H, then park()
    void park(bool afterHoming = false);   // safe Z, then the tool-change XY
    void parkSpot(double &x, double &y) const;   // where Park goes (machine)
    QStringList endOfJobMoves();          // a finished job: top, park spot, M5
    bool m_parkAfterHome = false;
    bool m_sawHoming = false;
    quint64 m_flowSeq = 0;       // counts resetFlow(): a Stop since calls off what was planned
    bool m_resetPending = false; // Cancel job / Disconnect waits for the hold to reset
    quint64 m_stopSeq = 0;       // counts real Stops (button, tool prompt, end stop, cable) only
    // Jog limits. Jogs go to absolute machine positions kept inside the
    // travel GRBL reports ($130-$132), because this machine runs with soft
    // and hard limits off ($20=0, $21=0): a jog past the end grinds the
    // stepper against the stop. m_jogPlan is where the queued jogs end up,
    // ahead of the lagging status reports.
    // `defer`: with no fresh report to seed the plan from, queue the jog
    // until one arrives (a tap) instead of dropping it (a hold-jog tick).
    bool jogBy(char axis, double delta, double feed, bool defer = true);
    void unlock();
    void cancelJob();
    // Back to no job: progress, the Running job list and the job's state
    // cleared, the card showing the toolpaths ticked now, ready for Run.
    void clearJob();
    QStringList riseToTop() const;
    void continueAfterToolChange();      // with the rise to the top in front
    void forgetJogPlan();
    double m_jogPlan[3] = {0, 0, 0};
    bool m_jogPlanValid = false;
    // The plan is seeded from a status report only when that report is
    // fresh: Idle, nothing unanswered, and at least the second report after
    // the plan was last forgotten (the first may have been asked for before
    // the jog was cancelled and still show it moving). A tap with no fresh
    // report yet waits here for one, a few at most.
    quint64 m_statusSeq = 0;
    quint64 m_jogFreshAfter = 0;
    bool m_jogSeedOk = false;
    struct PendingJog { char axis; double delta, feed; };
    QVector<PendingJog> m_deferredJogs;
    // A report is fresh for standingStill() from this sequence number on:
    // the second report after the last jog was sent.
    quint64 m_moveFreshAfter = 0;
    // A released hold-jog's second cancel (endHoldJog), still to go out. A
    // jog sent meanwhile would be cancelled with it, so jogs wait for it.
    QPointer<QTimer> m_jogCancelTimer;
    // Hold-jog lookahead when not homed (relative jogs): where the hold began
    // and how far it has been sent since.
    double m_holdOrigin = 0, m_holdSent = 0;
    bool m_holdOriginValid = false;
    // Machine coordinates (G53, the jog limits, the BitSetter and tool-change
    // spots) mean something only after a homing cycle on this connection.
    // False on connect, after Unlock out of an alarm, and after any alarm
    // that loses position; true when a homing cycle finishes.
    bool m_homed = false;
    bool m_homingStarted = false;
    bool m_resumeCheck = false;          // first status after connecting
    bool m_holdChecked = false;          // a hold left from before cleared
    QElapsedTimer m_settingsAsked;       // last $$ sent; asked again until answered
    QElapsedTimer m_busyClock;           // throttles the busy file
    QVariantList m_savedHomedAt;         // last homed position written to settings
    bool requireHomed(const QString &what);
    bool insideTravel(double x, double y) const;
    double switchClearance() const;
    double safeZ() const;
    double riseZ() const;
    bool endRiseWaiting() const;
    bool bitSetterReachable(const BitSetterConfig &cfg, const QString &what);
    bool jobWindingDown(const QString &what);
    QString m_unhomedWhy = QStringLiteral("not connected");
    // Where the program stood (machine X/Y, last report) when it parked for
    // a tool change. The resume goes back there at the top of Z before the
    // program's own lines: those start with a retract to the program's safe
    // height in WORK Z, which can be below the BitSetter button (2026-10-24:
    // 16.7 mm below it, and the rapid drove the bit into the button).
    bool m_preParkValid = false;
    double m_preParkX = 0, m_preParkY = 0;
    // An X/Y zero set before homing on this connection: homing redefines
    // machine coordinates, so that zero points somewhere else afterwards.
    bool m_zeroedUnhomed = false;
    QString m_lastLimitPins;
    bool m_limitWarned = false;
    QPushButton *m_padMapBtn = nullptr;
    QToolButton *m_logToggle = nullptr;
    QLabel *m_logLast = nullptr;

    // program
    QCheckBox *m_airCut;                 // rehearse: spindle stripped, Z lifted
    QDoubleSpinBox *m_airLift;           // air-cut Z lift (mm)
    QPushButton *m_runBtn, *m_pauseBtn, *m_stopBtn;
    bool m_held = false;
    // m_held stays until a report shows the machine out of Hold (seen in
    // Hold before, or never in Hold within 2 s: the hold did not take).
    bool m_holdSeen = false;
    QElapsedTimer m_heldClock;
    void setHeld(bool held);             // m_held and the pause button
    bool m_endRisePending = false;       // the end-of-job rise waits for Idle
    bool m_endOfJobPending = false;      // a real job is streaming: retract + M5 at its end
    QProgressBar *m_progress;
    QPlainTextEdit *m_console;
};

} // namespace c2d
