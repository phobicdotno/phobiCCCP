#pragma once
#include <QElapsedTimer>
#include <QObject>
#include <QStringList>

class QSerialPort;
class QTimer;

namespace c2d {

// Streams g-code to a GRBL controller (Shapeoko) over serial, replacing
// Carbide Motion. Uses GRBL's character-counting flow control: keep at most
// RX_BUFFER (127) unacknowledged bytes in the controller's serial buffer so
// its planner never starves. Realtime commands (?, !, ~, ctrl-X) bypass the
// line protocol entirely.
class GrblStreamer : public QObject
{
    Q_OBJECT
public:
    explicit GrblStreamer(QObject *parent = nullptr);
    ~GrblStreamer() override;

    static QStringList availablePorts();
    bool connectPort(const QString &portName, int baud = 115200);
    void disconnectPort();
    bool isConnected() const;
    bool isStreaming() const { return m_streaming; }

    // Conservative streaming: one line in flight, next only after its ack.
    // Slower than the 120-byte window but immune to USB/usbip stalls that
    // swallow burst writes. Each transmitted line is echoed to the console.
    void setConservative(bool on) { m_conservative = on; }
    qint64 msSinceAck() const;    // ms since the last ok/error while streaming

public slots:
    void sendCommand(const QString &line);      // jog / $X / G10 — immediate
    void startStream(const QStringList &lines); // run a whole program
    void pauseStream();                         // '!' feed hold
    void resumeStream();                        // '~' cycle start
    void stopStream();                          // abort: soft reset (ctrl-X)
    void requestStatus();                       // '?' realtime report
    void sendRealtime(char c);                  // override bytes (0x90-0x97 &c.)

signals:
    void connected(const QString &port);
    void disconnected();
    void statusReport(const QString &state, double mx, double my, double mz);
    void progressChanged(int acked, int total);
    void streamFinished(bool ok);
    void consoleLine(const QString &line);      // raw traffic for the log
    void errorOccurred(const QString &message);

private:
    void onReadyRead();
    void handleLine(const QByteArray &line);
    void pump();                                // fill GRBL's RX window

    QSerialPort *m_port = nullptr;
    QTimer *m_statusTimer;
    QByteArray m_rx;

    QStringList m_queue;
    int m_nextIndex = 0;                        // next line to transmit
    int m_ackedCount = 0;                       // ok/error responses seen
    QList<int> m_inflightSizes;                 // bytes per unacknowledged line
    int m_inflightBytes = 0;
    bool m_streaming = false;
    bool m_hadError = false;
    bool m_conservative = false;
    QElapsedTimer m_ackClock;                   // restarted on stream start + acks

    static const int RX_BUFFER = 120;           // GRBL has 128; keep headroom
};

} // namespace c2d
