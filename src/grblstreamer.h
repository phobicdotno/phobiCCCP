#pragma once
#include <QHash>
#include <QElapsedTimer>
#include <QObject>
#include <QStringList>

class QSerialPort;
class QTimer;

namespace c2d {

// Everything a `?` report tells us, in both coordinate systems. GRBL reports
// MPos (or WPos with $10) plus a periodic WCO; we keep the last WCO and
// derive the other frame so both are always available.
struct MachineStatus {
    QString state;                       // Idle / Run / Jog / Hold:0 / Alarm / Home …
    double mx = 0, my = 0, mz = 0;       // machine coordinates
    double wx = 0, wy = 0, wz = 0;       // work coordinates (G54 + TLO applied)
    double feed = 0, spindle = 0;        // FS: field
    // Ov: field - feed / rapid / spindle override, percent. GRBL sends it
    // only every few reports and after a change, so the last seen stays.
    int ovFeed = 100, ovRapid = 100, ovSpindle = 100;
    bool probePin = false;               // Pn:P — the probe input is closed
    QString limitPins;                   // Pn: limit inputs that are active (X, Y, Z, A)
    bool valid = false;
};

// Tool-length probe (Carbide's BitSetter: a spring-loaded switch at a fixed
// machine location, wired to GRBL's probe input). Coordinates are MACHINE
// coordinates (G53), because they never move when work zero does.
struct BitSetterConfig {
    bool enabled = false;
    double x = -20, y = -20;             // button centre, machine XY
    double safeZ = -5;                   // travel height, machine Z
    double fastFeed = 600;               // first touch, mm/min
    double slowFeed = 50;                // second (measuring) touch
    double maxTravel = 120;              // how far down the first probe may go
    double retract = 3;                  // back-off between the two touches
};

// Streams g-code to a GRBL controller (Shapeoko) over serial, replacing
// Carbide Motion. Uses GRBL's character-counting flow control: keep at most
// RX_BUFFER (127) unacknowledged bytes in the controller's serial buffer so
// its planner never starves. Realtime commands (?, !, ~, ctrl-X) bypass the
// line protocol entirely.
//
// Three things can own the serial line, never two at once:
//  - a PROGRAM stream (startStream), which parks itself at every tool change
//    marker (`M0 ;T<n>`) and waits for continueAfterToolChange();
//  - a MACRO (startMacro / measureTool): a short sequential script, one line
//    in flight, that reports probe results and completion;
//  - ad-hoc commands (sendCommand): jogs, zeroing, unlock — allowed whenever
//    no program is actively pumping and no macro is running.
class GrblStreamer : public QObject
{
    Q_OBJECT
public:
    explicit GrblStreamer(QObject *parent = nullptr);
    ~GrblStreamer() override;

    static QStringList availablePorts();          // names, e.g. ttyACM0
    static QStringList availablePortDetails();    // "ttyACM0 — Carbide 3D …"
    bool connectPort(const QString &portName, int baud = 115200);
    void disconnectPort();
    bool isConnected() const;
    bool isStreaming() const { return m_streaming; }
    bool isParkedForTool() const { return m_streaming && m_waitingTool >= 0; }
    // Hold with a lift: instead of stopping dead, the program carries on
    // with the lines already sent, then runs `lines` (Z up, spindle off) and
    // waits at a point that no cutting line passes until continueAfterHold().
    // Nothing moves in XY meanwhile. False when it cannot be done now (not
    // streaming, parked, nothing left to cut, or the program's modal state
    // cannot be read back): the caller falls back to a feed hold.
    bool canLiftHold() const;
    bool queueHoldPoint(const QStringList &lines);
    bool isHoldQueued() const { return m_streaming && m_holdQueued; }
    bool isHeldAtPoint() const { return m_streaming && m_waitingHold; }
    // After the hold: spindle back on (and its spin-up), down to where the
    // program was, and its motion mode and feed as the program left them.
    QStringList resumeLinesAfterHold() const;
    void continueAfterHold(const QStringList &lines);
    // What the program has sent up to line `upTo`: the last motion mode (G0..G3),
    // feed, spindle command and speed, and work Z. `valid` is false when a
    // line cannot be followed (G91, an unreadable word).
    struct Modal {
        bool valid = true;
        int motion = -1;                 // 0..3, -1 unknown
        double feed = 0;     bool haveFeed = false;
        QChar spindle;                   // '3', '4', '5' or null
        double speed = 0;    bool haveSpeed = false;
        double z = 0;        bool haveZ = false;
    };
    static Modal scanModal(const QStringList &lines, int upTo);
    // sendCommand() lines written but not yet answered. Continuous jog uses it
    // to stop queueing faster than the controller retires.
    int pendingCommands() const { return m_adhocPending; }
    bool isMacroRunning() const { return m_macroActive; }
    bool canSendCommand() const;
    // A hard or soft limit (ALARM:1/2, "Reset to continue") stopped the
    // controller: it answers no line, not even $H or $X, until a soft reset.
    // Cleared by the "Grbl" banner that follows the reset.
    bool needsReset() const { return m_needsReset; }
    // Alarm on a connection that has not answered a line yet: maybe a
    // controller still locked by a limit from before the port was opened
    // (it does not reboot on open). A reset first costs nothing in Alarm.
    bool maybeLocked() const
    {
        return m_needsReset
               || (!m_heard && m_status.state.startsWith(QLatin1String("Alarm")));
    }
    // Number of lines of the running (or last) macro answered with ok.
    int macroLinesDone() const { return m_macroDone; }

    // Conservative streaming: one line in flight, next only after its ack.
    // Slower than the 120-byte window but immune to USB/usbip stalls that
    // swallow burst writes. Each transmitted line is echoed to the console.
    void setConservative(bool on) { m_conservative = on; }
    qint64 msSinceAck() const;    // ms since the last ok/error while streaming

    const MachineStatus &status() const { return m_status; }
    bool lastProbeOk() const { return m_probeOk; }
    double lastProbeZ() const { return m_probeZ; }      // machine Z at contact
    double toolLengthOffset() const { return m_tlo; }
    // The work offset's Z (G54, mm) as last reported on this connection.
    bool hasWorkOffset() const { return m_haveWco; }
    double toolLengthOffsetAtWco() const { return m_tloAtWco; }
    // The offset before the latest applyToolLengthOffset: a WCO report that
    // left GRBL before it applied the new G43.1 still carries this one.
    double toolLengthOffsetBefore() const { return m_tloBefore; }
    double workOffsetZ() const { return m_wcoZ * reportScale(); }
    // A setting from the controller's `$$` listing ($130 = X max travel…),
    // or `fallback` when it has not been seen on this connection (the list
    // is emptied on every connect: another machine may answer now).
    double setting(int n, double fallback) const { return m_settings.value(n, fallback); }
    bool haveSettings() const { return !m_settings.isEmpty(); }   // a $$ listing came in

public slots:
    bool sendCommand(const QString &line);      // jog / $X / G10 — immediate
    void startStream(const QStringList &lines); // run a whole program
    // Resume past a tool marker; `first` runs ahead of the rest of the
    // program, as part of it (byte-counted and acked like its own lines).
    void continueAfterToolChange(const QStringList &first = {});
    void pauseStream();                         // '!' feed hold
    bool resumeStream();                        // '~' cycle start (false: refused)
    void stopStream();                          // abort: soft reset (ctrl-X)
    // Soft reset, then send `line` ($H, $X) as a command once the banner
    // says the controller listens again. For the way out of needsReset().
    void resetThen(const QString &line);
    void requestStatus();                       // '?' realtime report
    void sendRealtime(char c);                  // override bytes (0x90-0x97 &c.)

    void jogCancel();                           // realtime jog-cancel (0x85)

    // Sequential script: one line in flight, aborts on the first error.
    void startMacro(const QStringList &lines);
    // Drop the rest of a running macro without touching the controller;
    // macroFinished(false) follows. For a probe that found nothing.
    void cancelMacro();
    // BitSetter measurement script. Ends back over `returnX/returnY` (machine
    // coords) at safeZ when returnHome is true; probe result via probeResult()
    // and lastProbeZ() once macroFinished(true) arrives.
    void measureTool(const BitSetterConfig &cfg, bool returnHome,
                     double returnX = 0, double returnY = 0);
    // The same script as a line list, to prefix with e.g. zeroing commands.
    // "G90 G0 X Y" to the first X/Y the lines name from `from` on, or empty.
    static QString crossToFirstCut(const QStringList &lines, int from);
    static QStringList measureToolLines(const BitSetterConfig &cfg, bool returnHome,
                                        double returnX = 0, double returnY = 0);
    // A program that is nothing but one tool change: spindle off, up to safe
    // Z, the `M0 ;T<tool>` marker the post emits, and up to safe Z again. Run
    // through startStream() it takes the exact park / measure / G43.1 /
    // continue path a real job does, with no spindle and no cut, so the
    // tool-change flow can be checked on the machine before a job relies on it.
    static QStringList toolChangeRehearsalLines(const BitSetterConfig &cfg, int tool);
    // G43.1 dynamic tool-length offset. Remembered; sent when the line is
    // free (never injected into a running program) and re-applied after a
    // reset, $X and $H (GRBL clears it on reset, refuses it in Alarm).
    void applyToolLengthOffset(double mm);

signals:
    void connected(const QString &port);
    void disconnected();
    void statusReport(const QString &state, double mx, double my, double mz);
    void statusChanged(const c2d::MachineStatus &st);
    void progressChanged(int acked, int total);
    void streamFinished(bool ok);
    // The lines of a lift-hold are all answered: the program waits for continueAfterHold().
    void holdPointReached();
    // Program parked at `M0 ;T<n>`. `ordinal` counts the program's tool
    // changes from 0: two bits can share a number, so the number alone does
    // not say which bit the program wants next.
    void toolChangeRequested(int tool, int ordinal);
    void probeResult(double mx, double my, double mz, bool ok);
    void macroFinished(bool ok);
    void consoleLine(const QString &line);      // raw traffic for the log
    // A sendCommand() line was answered: `ok` true for "ok", false for an
    // error or alarm. The panel counts a zero as done only on its ok.
    void commandAnswered(const QString &line, bool ok);
    void errorOccurred(const QString &message);

private:
    void onReadyRead();
    void handleLine(const QByteArray &line);
    void pump();                                // fill GRBL's RX window
    void pumpMacro();
    void abortAfterReset();                     // controller rebooted under us
    // Stop the running program without pretending it finished. `holdMachine`
    // sends a feed hold first, which stops motion where it is and keeps the
    // position (unlike ctrl-X); pass false when the controller has already
    // stopped itself and will only listen to a reset.
    void abortStream(const QString &why, bool holdMachine);
    void resetAfterError();                     // the ctrl-X that follows a held abort
    void writeLine(const QString &line);
    void flushTlo();                            // send a pending G43.1 if allowed
    void resyncAdhoc(const QString &why);       // forget ad-hoc lines that will never ack

    QSerialPort *m_port = nullptr;
    QTimer *m_statusTimer;
    QTimer *m_errorResetTimer;                  // waits for Hold:0 after an error
    QElapsedTimer m_errorResetClock;
    QByteArray m_rx;
    MachineStatus m_status;
    QHash<int, double> m_settings;   // $n=value lines seen
    double m_wcoX = 0, m_wcoY = 0, m_wcoZ = 0;
    bool m_haveWco = false;
    double m_tloAtWco = 0.0;
    double reportScale() const;   // 25.4 when $13=1 (reports in inches)

    QStringList m_queue;
    int m_nextIndex = 0;                        // next line to transmit
    int m_ackedCount = 0;                       // ok/error responses seen
    QList<int> m_inflightSizes;                 // bytes per unacknowledged line
    int m_inflightBytes = 0;
    bool m_streaming = false;
    bool m_hadError = false;
    bool m_conservative = false;
    int m_waitingTool = -1;                     // >=0: parked at a tool marker
    bool m_holdQueued = false;                  // a lift-hold's lines are in the queue
    bool m_waitingHold = false;                 // standing at its marker
    int m_holdAt = 0;                           // queue index of its first line
    int m_toolChanges = 0;                      // markers parked at in this program
    QElapsedTimer m_ackClock;                   // restarted on stream start + acks

    QStringList m_macroQueue;
    bool m_macroActive = false;
    bool m_macroInflight = false;
    bool m_macroError = false;
    bool m_macroSkipAck = false;                // cancelled with a line in flight
    int m_adhocPending = 0;                     // sendCommand() lines awaiting ok
    QStringList m_adhocLines;                   // the same lines, oldest first
    QElapsedTimer m_adhocClock;                 // since the last ad-hoc send/ack
    bool m_tloPending = false;                  // G43.1 not yet on the controller
    int m_tloRetries = 0;                       // refused G43.1 resends so far
    bool m_tloWaitIdle = false;                 // refused (error:9 mid-jog): resend at Idle
    bool m_needsReset = false;                  // critical alarm: lines go unanswered
    bool m_resetNagged = false;                 // said so once for this alarm
    bool m_heard = false;                       // a line answered on this connection
    int m_macroDone = 0;                        // macro lines answered ok
    QString m_afterReset;                       // resetThen()'s line, sent on the banner
    QElapsedTimer m_afterResetClock;            // ...if the banner comes soon enough

    double m_probeZ = 0;
    bool m_probeOk = false;
    double m_tlo = 0;
    double m_tloBefore = 0;

    static const int RX_BUFFER = 120;           // GRBL has 128; keep headroom
};

} // namespace c2d

Q_DECLARE_METATYPE(c2d::MachineStatus)
