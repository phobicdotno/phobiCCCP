#pragma once
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
    bool probePin = false;               // Pn:P — the probe input is closed
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
    bool isMacroRunning() const { return m_macroActive; }
    bool canSendCommand() const;

    // Conservative streaming: one line in flight, next only after its ack.
    // Slower than the 120-byte window but immune to USB/usbip stalls that
    // swallow burst writes. Each transmitted line is echoed to the console.
    void setConservative(bool on) { m_conservative = on; }
    qint64 msSinceAck() const;    // ms since the last ok/error while streaming

    const MachineStatus &status() const { return m_status; }
    bool lastProbeOk() const { return m_probeOk; }
    double lastProbeZ() const { return m_probeZ; }      // machine Z at contact
    double toolLengthOffset() const { return m_tlo; }

public slots:
    bool sendCommand(const QString &line);      // jog / $X / G10 — immediate
    void startStream(const QStringList &lines); // run a whole program
    void continueAfterToolChange();             // resume past a tool marker
    void pauseStream();                         // '!' feed hold
    void resumeStream();                        // '~' cycle start
    void stopStream();                          // abort: soft reset (ctrl-X)
    void requestStatus();                       // '?' realtime report
    void sendRealtime(char c);                  // override bytes (0x90-0x97 &c.)

    void jogCancel();                           // realtime jog-cancel (0x85)

    // Sequential script: one line in flight, aborts on the first error.
    void startMacro(const QStringList &lines);
    // BitSetter measurement script. Ends back over `returnX/returnY` (machine
    // coords) at safeZ when returnHome is true; probe result via probeResult()
    // and lastProbeZ() once macroFinished(true) arrives.
    void measureTool(const BitSetterConfig &cfg, bool returnHome,
                     double returnX = 0, double returnY = 0);
    // The same script as a line list, to prefix with e.g. zeroing commands.
    static QStringList measureToolLines(const BitSetterConfig &cfg, bool returnHome,
                                        double returnX = 0, double returnY = 0);
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
    void toolChangeRequested(int tool);         // program parked at `M0 ;T<n>`
    void probeResult(double mx, double my, double mz, bool ok);
    void macroFinished(bool ok);
    void consoleLine(const QString &line);      // raw traffic for the log
    void errorOccurred(const QString &message);

private:
    void onReadyRead();
    void handleLine(const QByteArray &line);
    void pump();                                // fill GRBL's RX window
    void pumpMacro();
    void writeLine(const QString &line);
    void flushTlo();                            // send a pending G43.1 if allowed
    void resyncAdhoc(const QString &why);       // forget ad-hoc lines that will never ack

    QSerialPort *m_port = nullptr;
    QTimer *m_statusTimer;
    QByteArray m_rx;
    MachineStatus m_status;
    double m_wcoX = 0, m_wcoY = 0, m_wcoZ = 0;

    QStringList m_queue;
    int m_nextIndex = 0;                        // next line to transmit
    int m_ackedCount = 0;                       // ok/error responses seen
    QList<int> m_inflightSizes;                 // bytes per unacknowledged line
    int m_inflightBytes = 0;
    bool m_streaming = false;
    bool m_hadError = false;
    bool m_conservative = false;
    int m_waitingTool = -1;                     // >=0: parked at a tool marker
    QElapsedTimer m_ackClock;                   // restarted on stream start + acks

    QStringList m_macroQueue;
    bool m_macroActive = false;
    bool m_macroInflight = false;
    bool m_macroError = false;
    int m_adhocPending = 0;                     // sendCommand() lines awaiting ok
    QStringList m_adhocLines;                   // the same lines, oldest first
    QElapsedTimer m_adhocClock;                 // since the last ad-hoc send/ack
    bool m_tloPending = false;                  // G43.1 not yet on the controller

    double m_probeZ = 0;
    bool m_probeOk = false;
    double m_tlo = 0;

    static const int RX_BUFFER = 120;           // GRBL has 128; keep headroom
};

} // namespace c2d

Q_DECLARE_METATYPE(c2d::MachineStatus)
