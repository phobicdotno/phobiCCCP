#include "grblstreamer.h"

#include <QRegularExpression>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QTimer>

namespace c2d {

// `M0 ;T<n>` is the tool-change pause the post emits. It is never sent to the
// controller: the stream parks in front of it, the operator swaps the tool
// (and the BitSetter measures it), then the stream continues.
static const QString kToolMarker = QStringLiteral("@TOOL:");

static int toolMarkerNumber(const QString &line)
{
    static const QRegularExpression re(QStringLiteral("^M0\\s*;\\s*T(\\d+)"));
    const QRegularExpressionMatch m = re.match(line);
    return m.hasMatch() ? m.captured(1).toInt() : -1;
}

GrblStreamer::GrblStreamer(QObject *parent)
    : QObject(parent), m_statusTimer(new QTimer(this))
{
    qRegisterMetaType<c2d::MachineStatus>("c2d::MachineStatus");
    m_statusTimer->setInterval(250);
    connect(m_statusTimer, &QTimer::timeout, this, &GrblStreamer::requestStatus);
}

GrblStreamer::~GrblStreamer()
{
    disconnectPort();
}

QStringList GrblStreamer::availablePorts()
{
    QStringList out;
    for (const QSerialPortInfo &info : QSerialPortInfo::availablePorts())
        out << info.portName();
    return out;
}

QStringList GrblStreamer::availablePortDetails()
{
    QStringList out;
    for (const QSerialPortInfo &info : QSerialPortInfo::availablePorts()) {
        QString d = info.description();
        if (!info.manufacturer().isEmpty())
            d = d.isEmpty() ? info.manufacturer()
                            : QStringLiteral("%1, %2").arg(d, info.manufacturer());
        out << (d.isEmpty() ? info.portName()
                            : QStringLiteral("%1 — %2").arg(info.portName(), d));
    }
    return out;
}

bool GrblStreamer::connectPort(const QString &portName, int baud)
{
    disconnectPort();
    m_port = new QSerialPort(portName, this);
    m_port->setBaudRate(baud);
    if (!m_port->open(QIODevice::ReadWrite)) {
        emit errorOccurred(m_port->errorString());
        delete m_port;
        m_port = nullptr;
        return false;
    }
    connect(m_port, &QSerialPort::readyRead, this, &GrblStreamer::onReadyRead);
    connect(m_port, &QSerialPort::errorOccurred, this,
            [this](QSerialPort::SerialPortError e) {
        if (e != QSerialPort::NoError)
            emit errorOccurred(QStringLiteral("serial port error %1 (%2)")
                                   .arg(int(e), 0)
                                   .arg(m_port ? m_port->errorString() : QString()));
    });
    m_rx.clear();
    m_status = MachineStatus();
    m_adhocPending = 0;
    m_statusTimer->start();
    emit connected(portName);
    return true;
}

void GrblStreamer::disconnectPort()
{
    m_statusTimer->stop();
    if (m_port) {
        m_port->close();
        m_port->deleteLater();
        m_port = nullptr;
        m_streaming = false;
        m_waitingTool = -1;
        m_macroActive = false;
        m_macroQueue.clear();
        m_status = MachineStatus();
        emit disconnected();
    }
}

bool GrblStreamer::isConnected() const
{
    return m_port && m_port->isOpen();
}

bool GrblStreamer::canSendCommand() const
{
    return isConnected() && !m_macroActive && (!m_streaming || m_waitingTool >= 0);
}

void GrblStreamer::writeLine(const QString &line)
{
    const QByteArray data = line.trimmed().toLatin1() + '\n';
    m_port->write(data);
    emit consoleLine(QStringLiteral("> %1").arg(line.trimmed()));
}

bool GrblStreamer::sendCommand(const QString &line)
{
    if (!canSendCommand())
        return false;
    ++m_adhocPending;
    writeLine(line);
    return true;
}

void GrblStreamer::requestStatus()
{
    if (isConnected())
        m_port->write("?", 1);
}

void GrblStreamer::sendRealtime(char c)
{
    if (isConnected())
        m_port->write(&c, 1);
}

void GrblStreamer::jogStart(char axis, int dir, double feed)
{
    if (!canSendCommand())
        return;
    // Far beyond any table: GRBL clips jogs to soft limits when they are on,
    // and the cancel byte stops it long before that anyway.
    ++m_adhocPending;
    writeLine(QStringLiteral("$J=G91 %1%2 F%3")
                  .arg(QChar::fromLatin1(axis)).arg(dir < 0 ? -1000 : 1000).arg(feed));
}

void GrblStreamer::jogCancel()
{
    sendRealtime(char(0x85));
}

void GrblStreamer::startStream(const QStringList &lines)
{
    if (!isConnected() || m_streaming || m_macroActive)
        return;
    m_queue.clear();
    for (const QString &l : lines) {
        const QString s = l.trimmed();
        // ( )-comment lines are never queued: Carbide's GRBL 1.1h build does
        // not ack them (verified live — the ack stream stops dead at the
        // first one, stalling char-counting). They're for humans anyway.
        if (s.isEmpty() || s.startsWith(QChar('(')))
            continue;
        const int tool = toolMarkerNumber(s);
        m_queue << (tool >= 0 ? kToolMarker + QString::number(tool) : s);
    }
    m_nextIndex = 0;
    m_ackedCount = 0;
    m_inflightSizes.clear();
    m_inflightBytes = 0;
    m_hadError = false;
    m_waitingTool = -1;
    m_streaming = !m_queue.isEmpty();
    m_ackClock.start();
    emit progressChanged(0, m_queue.size());
    pump();
}

void GrblStreamer::continueAfterToolChange()
{
    if (!m_streaming || m_waitingTool < 0)
        return;
    m_waitingTool = -1;
    m_ackClock.restart();
    emit consoleLine(QStringLiteral("== tool change done, continuing =="));
    pump();
}

void GrblStreamer::pauseStream()
{
    if (isConnected())
        m_port->write("!", 1);
}

void GrblStreamer::resumeStream()
{
    if (isConnected())
        m_port->write("~", 1);
}

void GrblStreamer::stopStream()
{
    if (!isConnected())
        return;
    const char reset = 0x18;                    // ctrl-X: GRBL soft reset
    m_port->write(&reset, 1);
    const bool wasProgram = m_streaming;
    m_streaming = false;
    m_waitingTool = -1;
    m_queue.clear();
    m_inflightSizes.clear();
    m_inflightBytes = 0;
    const bool wasMacro = m_macroActive;
    m_macroActive = false;
    m_macroInflight = false;
    m_macroQueue.clear();
    m_adhocPending = 0;                         // reset flushes GRBL's buffer
    emit consoleLine(QStringLiteral("> [soft reset]"));
    if (wasMacro)
        emit macroFinished(false);
    if (wasProgram)
        emit streamFinished(false);
}

void GrblStreamer::pump()
{
    while (m_streaming && m_waitingTool < 0 && m_adhocPending == 0
           && m_nextIndex < m_queue.size()) {
        const QString &line = m_queue.at(m_nextIndex);
        if (line.startsWith(kToolMarker)) {
            // Park only once everything before the marker has been acked, so
            // the operator (and the probe macro) find the machine idle.
            if (!m_inflightSizes.isEmpty())
                break;
            m_waitingTool = line.mid(kToolMarker.size()).toInt();
            ++m_nextIndex;
            ++m_ackedCount;                      // the marker counts as done
            emit progressChanged(m_ackedCount, m_queue.size());
            emit consoleLine(QStringLiteral("== tool change: T%1 ==").arg(m_waitingTool));
            emit toolChangeRequested(m_waitingTool);
            break;
        }
        if (m_conservative && !m_inflightSizes.isEmpty())
            break;
        const QByteArray data = line.toLatin1() + '\n';
        if (m_inflightBytes + data.size() > RX_BUFFER)
            break;
        m_port->write(data);
        if (m_conservative)
            emit consoleLine(QStringLiteral(">> %1").arg(line));
        m_inflightSizes.append(data.size());
        m_inflightBytes += data.size();
        ++m_nextIndex;
    }
    if (m_streaming && m_ackedCount >= m_queue.size()) {
        m_streaming = false;
        emit streamFinished(!m_hadError);
    }
}

void GrblStreamer::startMacro(const QStringList &lines)
{
    if (!isConnected() || m_macroActive || (m_streaming && m_waitingTool < 0)) {
        emit macroFinished(false);
        return;
    }
    m_macroQueue.clear();
    for (const QString &l : lines)
        if (!l.trimmed().isEmpty())
            m_macroQueue << l.trimmed();
    m_macroActive = true;
    m_macroInflight = false;
    m_macroError = false;
    pumpMacro();
}

void GrblStreamer::pumpMacro()
{
    if (!m_macroActive || m_macroInflight || m_adhocPending > 0)
        return;
    if (m_macroQueue.isEmpty()) {
        m_macroActive = false;
        emit macroFinished(!m_macroError);
        return;
    }
    const QString line = m_macroQueue.takeFirst();
    m_macroInflight = true;
    writeLine(line);
}

QStringList GrblStreamer::measureToolLines(const BitSetterConfig &cfg, bool returnHome,
                                          double returnX, double returnY)
{
    // Two touches like Carbide Motion: a fast one to find the button, back
    // off, then a slow one for the number that matters. GRBL blocks the `ok`
    // of a G38.2 until the probe cycle ends, so the macro naturally waits.
    QStringList m;
    m << QStringLiteral("M5")
      << QStringLiteral("G90")
      << QStringLiteral("G53 G0 Z%1").arg(cfg.safeZ, 0, 'f', 3)
      << QStringLiteral("G53 G0 X%1 Y%2").arg(cfg.x, 0, 'f', 3).arg(cfg.y, 0, 'f', 3)
      << QStringLiteral("G91 G38.2 Z-%1 F%2").arg(cfg.maxTravel, 0, 'f', 3).arg(cfg.fastFeed, 0, 'f', 0)
      << QStringLiteral("G0 Z%1").arg(cfg.retract, 0, 'f', 3)
      << QStringLiteral("G38.2 Z-%1 F%2").arg(cfg.retract + 2.0, 0, 'f', 3).arg(cfg.slowFeed, 0, 'f', 0)
      << QStringLiteral("G90")
      << QStringLiteral("G53 G0 Z%1").arg(cfg.safeZ, 0, 'f', 3);
    if (returnHome)
        m << QStringLiteral("G53 G0 X%1 Y%2").arg(returnX, 0, 'f', 3).arg(returnY, 0, 'f', 3);
    return m;
}

void GrblStreamer::measureTool(const BitSetterConfig &cfg, bool returnHome,
                               double returnX, double returnY)
{
    m_probeOk = false;
    startMacro(measureToolLines(cfg, returnHome, returnX, returnY));
}

void GrblStreamer::applyToolLengthOffset(double mm)
{
    m_tlo = mm;
    if (isConnected()) {
        ++m_adhocPending;
        writeLine(QStringLiteral("G43.1 Z%1").arg(mm, 0, 'f', 3));
    }
}

qint64 GrblStreamer::msSinceAck() const
{
    return m_ackClock.isValid() ? m_ackClock.elapsed() : 0;
}

void GrblStreamer::onReadyRead()
{
    m_rx += m_port->readAll();
    int nl;
    while ((nl = m_rx.indexOf('\n')) >= 0) {
        const QByteArray line = m_rx.left(nl).trimmed();
        m_rx.remove(0, nl + 1);
        if (!line.isEmpty())
            handleLine(line);
    }
}

void GrblStreamer::handleLine(const QByteArray &line)
{
    // Realtime status: <Idle|MPos:10.000,20.000,-1.000|FS:500,8000|WCO:0,0,0>
    if (line.startsWith('<')) {
        const QList<QByteArray> parts = line.mid(1).chopped(line.endsWith('>') ? 1 : 0).split('|');
        MachineStatus st = m_status;
        st.state = parts.isEmpty() ? QString() : QString::fromLatin1(parts.first());
        st.probePin = false;
        bool haveM = false, haveW = false;
        double px = 0, py = 0, pz = 0;
        for (const QByteArray &p : parts) {
            if (p.startsWith("WCO:")) {
                const QList<QByteArray> c = p.mid(4).split(',');
                if (c.size() >= 3) {
                    m_wcoX = c.at(0).toDouble();
                    m_wcoY = c.at(1).toDouble();
                    m_wcoZ = c.at(2).toDouble();
                }
            } else if (p.startsWith("MPos:") || p.startsWith("WPos:")) {
                const QList<QByteArray> c = p.mid(5).split(',');
                if (c.size() >= 3) {
                    px = c.at(0).toDouble();
                    py = c.at(1).toDouble();
                    pz = c.at(2).toDouble();
                    haveM = p.startsWith("MPos:");
                    haveW = !haveM;
                }
            } else if (p.startsWith("FS:")) {
                const QList<QByteArray> c = p.mid(3).split(',');
                if (c.size() >= 2) {
                    st.feed = c.at(0).toDouble();
                    st.spindle = c.at(1).toDouble();
                }
            } else if (p.startsWith("Pn:")) {
                st.probePin = p.contains('P');
            }
        }
        if (haveM) {
            st.mx = px; st.my = py; st.mz = pz;
            st.wx = px - m_wcoX; st.wy = py - m_wcoY; st.wz = pz - m_wcoZ;
        } else if (haveW) {
            st.wx = px; st.wy = py; st.wz = pz;
            st.mx = px + m_wcoX; st.my = py + m_wcoY; st.mz = pz + m_wcoZ;
        }
        st.valid = true;
        m_status = st;
        emit statusReport(st.state, st.mx, st.my, st.mz);
        emit statusChanged(st);
        return;
    }

    emit consoleLine(QString::fromLatin1(line));

    // Probe report precedes the G38.2's ok: [PRB:-5.000,-200.000,-80.123:1]
    if (line.startsWith("[PRB:")) {
        const QByteArray body = line.mid(5).chopped(line.endsWith(']') ? 1 : 0);
        const int colon = body.lastIndexOf(':');
        const QList<QByteArray> c = body.left(colon).split(',');
        if (c.size() >= 3) {
            m_probeOk = (colon >= 0 && body.mid(colon + 1).trimmed() == "1");
            m_probeZ = c.at(2).toDouble();
            emit probeResult(c.at(0).toDouble(), c.at(1).toDouble(), m_probeZ, m_probeOk);
        }
        return;
    }

    // After a reset GRBL forgets G43.1; put the current tool's offset back.
    if (line.startsWith("Grbl ") && m_tlo != 0.0 && isConnected()) {
        QTimer::singleShot(400, this, [this] {
            if (isConnected() && !m_macroActive && !m_streaming)
                applyToolLengthOffset(m_tlo);
        });
    }

    const bool ok = (line == "ok");
    const bool err = line.startsWith("error:") || line.startsWith("ALARM:");

    if ((ok || err) && m_adhocPending > 0) {
        --m_adhocPending;
        if (err)
            emit errorOccurred(QString::fromLatin1(line));
        if (m_adhocPending == 0) {
            pumpMacro();
            pump();
        }
        return;
    }

    if (m_macroActive && (ok || err)) {
        m_macroInflight = false;
        if (err) {
            m_macroError = true;
            m_macroQueue.clear();
            emit errorOccurred(QString::fromLatin1(line));
        }
        pumpMacro();
        return;
    }

    if ((ok || err) && m_streaming && !m_inflightSizes.isEmpty()) {
        m_inflightBytes -= m_inflightSizes.takeFirst();
        ++m_ackedCount;
        m_ackClock.restart();
        if (err) {
            m_hadError = true;
            emit errorOccurred(QString::fromLatin1(line));
        }
        emit progressChanged(m_ackedCount, m_queue.size());
        pump();                                  // also handles completion
    } else if (err) {
        emit errorOccurred(QString::fromLatin1(line));
    }
}

} // namespace c2d
