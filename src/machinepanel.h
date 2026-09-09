#pragma once
#include "grblstreamer.h"
#include <QHash>
#include <QWidget>
#include <functional>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTimer;

namespace c2d {

class Gamepad;

class Document;

// Machine control dock: connect to a GRBL controller over serial, jog, set
// work zero, measure tools on the BitSetter, and stream the document's
// generated g-code with automatic tool-change stops — a Carbide-Motion
// replacement.
class MachinePanel : public QWidget
{
    Q_OBJECT
public:
    explicit MachinePanel(QWidget *parent = nullptr);
    // The streamer is a child, so ~QWidget deletes it AFTER this panel's own
    // members are gone, and ~GrblStreamer emits disconnected/streamFinished
    // straight into slots that then touch them. Cut the connections first.
    ~MachinePanel() override;
    void setDocument(Document *doc) { m_doc = doc; }
    GrblStreamer *streamer() const { return m_grbl; }

signals:
    void livePosition(double wx, double wy, double wz, bool valid);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    enum class Phase { Idle, ParkForTool, MeasureAfterChange, MeasureForRef,
                       MeasureManual, GoToZero };

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
    void jogIncrement();
    void stopHoldJog();                  // end a running hold-jog (cancel) safely
    QString currentPortName() const;
    void resetFlow();                    // forget tool-change / macro state
    double jogFeed(char axis) const;
    void zero(const QString &axes);
    void goToXY0();
    void measureTool(Phase why);
    void onMacroFinished(bool ok);
    void onToolChange(int tool);
    void onStatus(const MachineStatus &st);
    void whenIdle(std::function<void()> fn);
    BitSetterConfig bitSetter() const;
    void loadSettings();
    void saveSettings();
    void updateOffsetLabels();
    void runProgram();
    void log(const QString &line);

    Document *m_doc = nullptr;
    GrblStreamer *m_grbl;

    // connection
    QComboBox *m_ports;
    QPushButton *m_connectBtn;
    QLabel *m_state, *m_wpos, *m_mpos, *m_extra;

    // jog
    QComboBox *m_step, *m_speed;
    QTimer *m_holdTimer;                 // press-and-hold detection
    QTimer *m_holdRepeat;                // feeds short jog increments while held
    Gamepad *m_pad = nullptr;
    QLabel *m_padLabel = nullptr;
    QHash<int, QString> m_padMap;        // button number -> action name
    int m_padAxisDir[2] = {0, 0};        // last direction seen on axes 0 and 1
    char m_holdAxis = 0;
    int m_holdDir = 0;
    bool m_holdJogging = false;
    bool m_loading = false;              // loadSettings() in progress

    // bitsetter / tool change
    QGroupBox *m_bsGroup;
    QDoubleSpinBox *m_bsX, *m_bsY, *m_bsSafeZ, *m_bsFast, *m_bsSlow;
    QDoubleSpinBox *m_tcX, *m_tcY;
    QLabel *m_refLabel, *m_tloLabel;
    QPushButton *m_measureBtn;
    bool m_haveRef = false;
    double m_refZ = 0;                   // machine Z where the reference tool tripped
    Phase m_phase = Phase::Idle;
    int m_pendingTool = -1;
    // Whether m_pendingTool has actually been measured on the BitSetter. A
    // failed measurement leaves the *previous* tool's G43.1 in force, so
    // resuming without one cuts as deep as the two tools differ in length.
    bool m_toolMeasured = false;
    std::function<void()> m_onIdle;
    QString m_lastState;

    // program
    QCheckBox *m_airCut;                 // rehearse: spindle stripped, Z lifted
    QDoubleSpinBox *m_airLift;           // air-cut Z lift (mm)
    QPushButton *m_runBtn, *m_pauseBtn, *m_stopBtn;
    bool m_held = false;
    QProgressBar *m_progress;
    QPlainTextEdit *m_console;
};

} // namespace c2d
