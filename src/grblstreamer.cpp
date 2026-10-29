#include "grblstreamer.h"

#include <QRegularExpression>
#ifdef HAVE_SERIALPORT
#include <QSerialPort>
#include <QSerialPortInfo>
#else
#include "serialstub.h"
#endif
#include <QTimer>

namespace c2d {

// `M0 ;T<n>` is the tool-change pause the post emits. It is never sent to the
// controller: the stream parks in front of it, the operator swaps the tool
// (and the BitSetter measures it), then the stream continues.
static const QString kToolMarker = QStringLiteral("@TOOL:");
static const QString kHoldMarker = QStringLiteral("@HOLD");

static int toolMarkerNumber(const QString &line)
{
    static const QRegularExpression re(QStringLiteral("^M0\\s*;\\s*T(\\d+)"));
    const QRegularExpressionMatch m = re.match(line);
    return m.hasMatch() ? m.captured(1).toInt() : -1;
}

GrblStreamer::GrblStreamer(QObject *parent)
    : QObject(parent), m_statusTimer(new QTimer(this)), m_errorResetTimer(new QTimer(this))
{
    qRegisterMetaType<c2d::MachineStatus>("c2d::MachineStatus");
    m_statusTimer->setInterval(250);
    connect(m_statusTimer, &QTimer::timeout, this, &GrblStreamer::requestStatus);
    m_errorResetTimer->setInterval(100);
    connect(m_errorResetTimer, &QTimer::timeout, this, &GrblStreamer::resetAfterError);
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
        if (e == QSerialPort::NoError)
            return;
        emit errorOccurred(QStringLiteral("serial port error %1 (%2)")
                               .arg(int(e), 0)
                               .arg(m_port ? m_port->errorString() : QString()));
        // The port is gone - cable pulled, adapter reset, permissions lost.
        // Only reporting it leaves isConnected() true, m_streaming true and
        // the status timer writing `?` into a dead handle, so the app claims
        // the program is still running while the machine stalls mid-cut with
        // the spindle live. Tear the connection down so the panel says so.
        switch (e) {
        case QSerialPort::ResourceError:
        case QSerialPort::DeviceNotFoundError:
        case QSerialPort::PermissionError:
        case QSerialPort::NotOpenError:
            disconnectPort();
            break;
        default:
            break;
        }
    });
    m_rx.clear();
    m_status = MachineStatus();
    m_adhocPending = 0;
    m_adhocLines.clear();
    m_needsReset = false;
    m_resetNagged = false;
    m_heard = false;
    m_afterReset.clear();
    // The last connection's $$ and WCO belong to whatever answered then: a
    // reconnect may be another machine (machine profiles) or a controller
    // whose settings changed. Until the new $$ arrives the fallbacks apply.
    m_settings.clear();
    m_wcoX = m_wcoY = m_wcoZ = 0;
    m_haveWco = false;
    m_statusTimer->start();
    emit connected(portName);
    return true;
}

void GrblStreamer::disconnectPort()
{
    m_statusTimer->stop();
    m_errorResetTimer->stop();
    if (m_port) {
        // A soft reset or feed hold written just before (a disconnect in the
        // middle of a job) must reach the controller, not die in our buffer.
        if (m_port->isOpen() && m_port->bytesToWrite() > 0)
            m_port->waitForBytesWritten(200);
        m_port->close();
        m_port->deleteLater();
        m_port = nullptr;
        const bool wasProgram = m_streaming, wasMacro = m_macroActive;
        m_streaming = false;
        m_waitingTool = -1;
        m_waitingHold = false;
        m_holdQueued = false;
        m_macroActive = false;
        m_macroInflight = false;
        m_macroSkipAck = false;
        m_macroQueue.clear();
        m_adhocPending = 0;
        m_adhocLines.clear();
        m_status = MachineStatus();
        if (wasMacro)
            emit macroFinished(false);
        if (wasProgram)
            emit streamFinished(false);
        emit disconnected();
    }
}

bool GrblStreamer::isConnected() const
{
    return m_port && m_port->isOpen();
}

bool GrblStreamer::canSendCommand() const
{
    // Not while an error's reset is pending either: GRBL is still answering
    // the program lines in its buffer, and those oks would be counted
    // against whatever was sent now.
    // Nor after a limit alarm, until the reset: nothing would be answered.
    return isConnected() && !m_macroActive && !m_errorResetTimer->isActive()
           && !m_needsReset && (!m_streaming || m_waitingTool >= 0);
}

void GrblStreamer::writeLine(const QString &line)
{
    const QByteArray data = line.trimmed().toLatin1() + '\n';
    m_port->write(data);
    emit consoleLine(QStringLiteral("> %1").arg(line.trimmed()));
}

bool GrblStreamer::sendCommand(const QString &line)
{
    const QString l = line.trimmed();
    // After a hard or soft limit GRBL reads nothing but realtime bytes until
    // a soft reset: the line would never be answered, and pendingCommands()
    // would block every later stream, macro and jog behind it.
    if (m_needsReset && !l.isEmpty() && isConnected()) {
        if (!m_resetNagged)
            emit errorOccurred(QStringLiteral("%1 not sent: the controller stopped on a limit "
                                              "and listens only after a reset (Initialize or "
                                              "Unlock does it)").arg(l));
        m_resetNagged = true;
        return false;
    }
    if (!canSendCommand())
        return false;
    if (!l.isEmpty()) {
        // Empty lines (the wake newline right after open) are sent uncounted:
        // an Arduino-class board is still resetting then and would swallow it,
        // and a lost ack would block every later stream and macro.
        ++m_adhocPending;
        m_adhocLines << l;
        m_adhocClock.restart();
    }
    writeLine(l);
    return true;
}

void GrblStreamer::requestStatus()
{
    if (isConnected())
        m_port->write("?", 1);
}

void GrblStreamer::sendRealtime(char c)
{
    // No cycle start while holding before the reset after an error: GRBL
    // would run the rest of the buffered program.
    if (c == '~' && m_errorResetTimer->isActive())
        return;
    if (isConnected())
        m_port->write(&c, 1);
}

void GrblStreamer::jogCancel()
{
    sendRealtime(char(0x85));
}

void GrblStreamer::startStream(const QStringList &lines)
{
    if (!isConnected() || m_streaming || m_macroActive || m_errorResetTimer->isActive())
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
    m_waitingHold = false;
    m_holdQueued = false;
    m_toolChanges = 0;
    m_streaming = !m_queue.isEmpty();
    m_ackClock.start();
    emit progressChanged(0, m_queue.size());
    pump();
}

// 2026-10-25: after a tool change the machine stood 2.5 mm from the BitSetter
// and the program's first line, a Z-only rapid in work Z, plunged into it. A
// Z-only line carries no XY to go across first, so look ahead for the first
// X and Y the program names and go there (work coordinates, at the top).
QString GrblStreamer::crossToFirstCut(const QStringList &lines, int from)
{
    static const QRegularExpression wx(QStringLiteral("(?:^|[^A-Z])X(-?[0-9.]+)"));
    static const QRegularExpression wy(QStringLiteral("(?:^|[^A-Z])Y(-?[0-9.]+)"));
    static const QRegularExpression paren(QStringLiteral("\\([^)]*\\)?"));
    // A pause: M0, M00, M1, M01. Not M03 or M05: 2026-10-26 the spindle-on
    // line ("M03S18000", after the post's retract and before its first X/Y)
    // ended the look-ahead, nothing crossed, and the retract came down at the
    // Park spot beside the BitSetter with the spindle starting.
    static const QRegularExpression pause(QStringLiteral("^M0*[01](?![0-9])"));
    QString gx, gy;
    for (int i = from; i < lines.size() && i < from + 400 && (gx.isEmpty() || gy.isEmpty()); ++i) {
        QString l = lines.at(i);
        const int c = l.indexOf(QChar(';'));
        if (c >= 0) l.truncate(c);
        l.remove(paren);                 // "(Pocket X2)" names no position
        l = l.trimmed().toUpper();
        // A tool change next: the machine parks first, and the resume crosses then.
        if (pause.match(l).hasMatch() || l.contains(QStringLiteral("G91"))
            || l.contains(QStringLiteral("G53")))
            break;
        const auto mx = wx.match(l), my = wy.match(l);
        if (mx.hasMatch()) gx = mx.captured(1);
        if (my.hasMatch()) gy = my.captured(1);
    }
    if (gx.isEmpty() || gy.isEmpty())
        return {};
    return QStringLiteral("G90 G0 X%1 Y%2").arg(gx, gy);
}

GrblStreamer::Modal GrblStreamer::scanModal(const QStringList &lines, int upTo)
{
    static const QRegularExpression gWord(QStringLiteral("(?:^|[^A-Z0-9.])G0*([0-9]+)(?![0-9.])"));
    static const QRegularExpression fWord(QStringLiteral("(?:^|[^A-Z])F(-?[0-9.]+)"));
    static const QRegularExpression sWord(QStringLiteral("(?:^|[^A-Z])S(-?[0-9.]+)"));
    static const QRegularExpression zWord(QStringLiteral("(?:^|[^A-Z])Z(-?[0-9.]+)"));
    static const QRegularExpression mWord(QStringLiteral("(?:^|[^A-Z0-9.])M0*([345])(?![0-9.])"));
    Modal m;
    for (int i = 0; i < upTo && i < lines.size(); ++i) {
        QString l = lines.at(i);
        l = l.left(l.indexOf(QChar(';')) < 0 ? l.size() : l.indexOf(QChar(';'))).toUpper();
        if (l.startsWith(kToolMarker) || l == kHoldMarker || l.trimmed().isEmpty())
            continue;
        bool machine = false;
        for (auto it = gWord.globalMatch(l); it.hasNext();) {
            const int g = it.next().captured(1).toInt();
            if (g >= 0 && g <= 3)
                m.motion = g;
            else if (g == 91)
                m.valid = false;             // relative moves: Z cannot be followed
            else if (g == 53)
                machine = true;
        }
        auto fm = fWord.match(l);
        if (fm.hasMatch()) { m.feed = fm.captured(1).toDouble(); m.haveFeed = true; }
        auto sm = sWord.match(l);
        if (sm.hasMatch()) { m.speed = sm.captured(1).toDouble(); m.haveSpeed = true; }
        for (auto it = mWord.globalMatch(l); it.hasNext();)
            m.spindle = it.next().captured(1).at(0);
        auto zm = zWord.match(l);
        if (zm.hasMatch() && !machine) {
            m.z = zm.captured(1).toDouble();
            m.haveZ = true;
        }
    }
    return m;
}

bool GrblStreamer::canLiftHold() const
{
    if (!m_streaming || m_waitingTool >= 0 || m_waitingHold || m_holdQueued
        || m_adhocPending != 0 || m_macroActive || m_errorResetTimer->isActive() || m_needsReset)
        return false;
    // Something real must be left behind the marker, or the program would end at it.
    bool cutting = false;
    for (int i = m_nextIndex; i < m_queue.size() && !cutting; ++i)
        cutting = !m_queue.at(i).startsWith(kToolMarker) && m_queue.at(i) != kHoldMarker;
    if (!cutting)
        return false;
    const Modal m = scanModal(m_queue, m_nextIndex);
    return m.valid && m.motion >= 0 && m.haveZ && m.haveFeed;
}

bool GrblStreamer::queueHoldPoint(const QStringList &lines)
{
    if (!canLiftHold())
        return false;
    m_holdAt = m_nextIndex;
    int at = m_nextIndex;
    for (const QString &l : lines) {
        const QString t = l.trimmed();
        if (!t.isEmpty() && !t.startsWith(QChar('(')))
            m_queue.insert(at++, t);
    }
    m_queue.insert(at, kHoldMarker);
    m_holdQueued = true;
    emit consoleLine(QStringLiteral("== hold: finishing what is sent, Z up, spindle off =="));
    emit progressChanged(m_ackedCount, m_queue.size());
    pump();
    return true;
}

QStringList GrblStreamer::resumeLinesAfterHold() const
{
    const Modal m = scanModal(m_queue, m_holdAt);
    QStringList r;
    r << QStringLiteral("G90");
    if ((m.spindle == QChar('3') || m.spindle == QChar('4')) && m.haveSpeed) {
        r << QStringLiteral("M0%1S%2").arg(m.spindle).arg(m.speed, 0, 'f', 0)
          << QStringLiteral("G4P3.00");                // the post's own spin-up wait
    }
    if (m.haveZ) {
        // Down to where the program was: a rapid to just above the stock, then
        // slowly into the cut. The work Z is the program's own (G43.1 and the
        // zero stand), XY did not change.
        if (m.z >= 2.5) {
            r << QStringLiteral("G0Z%1").arg(m.z, 0, 'f', 3);
        } else {
            r << QStringLiteral("G0Z2.500")
              << QStringLiteral("G1Z%1F%2").arg(m.z, 0, 'f', 3)
                     .arg(qMin(m.haveFeed ? m.feed : 600.0, 600.0), 0, 'f', 0);
        }
    }
    // The program's modal state: its feed and motion mode, as it left them.
    if (m.haveFeed)
        r << QStringLiteral("G1F%1").arg(m.feed, 0, 'f', 1);
    if (m.motion == 0)
        r << QStringLiteral("G0");
    else if (m.motion >= 2)
        r << QStringLiteral("G1");     // arcs always name their own G word
    return r;
}

void GrblStreamer::continueAfterHold(const QStringList &lines)
{
    if (!m_streaming || !m_waitingHold)
        return;
    int at = m_nextIndex;
    for (const QString &l : lines) {
        const QString t = l.trimmed();
        if (!t.isEmpty())
            m_queue.insert(at++, t);
    }
    m_waitingHold = false;
    m_holdQueued = false;
    m_ackClock.restart();
    emit consoleLine(QStringLiteral("== hold over, continuing =="));
    if (at != m_nextIndex)
        emit progressChanged(m_ackedCount, m_queue.size());
    pump();
}

void GrblStreamer::continueAfterToolChange(const QStringList &first)
{
    if (!m_streaming || m_waitingTool < 0)
        return;
    // The park travel or the BitSetter probe is still running. Resuming here
    // would put two writers on the line: pump() writes cutting moves while
    // pumpMacro() is still working through M5 / G53 moves, the macro branch
    // takes every ok before the stream branch sees it (so the stream never
    // releases its inflight entries and wedges), and the macro's own lines are
    // written with no byte accounting at all - past GRBL's 128-byte buffer.
    if (m_macroActive) {
        emit consoleLine(QStringLiteral("== still moving - wait for the tool "
                                        "measurement to finish =="));
        return;
    }
    // Lines to run before the program resumes (the rise to the top) go into
    // the program itself, so they are sent and acknowledged like any other
    // program line instead of as commands on the side.
    int at = m_nextIndex;
    for (const QString &l : first) {
        const QString t = l.trimmed();
        if (!t.isEmpty() && !t.startsWith(QChar('(')))
            m_queue.insert(at++, t);
    }
    // The resume can start with a rapid down in work Z (the post's retract),
    // and the spindle may be standing over the BitSetter still: cross to the
    // first cut at the top before the program may come down.
    if (!first.isEmpty()) {
        const QString across = crossToFirstCut(m_queue, at);
        if (!across.isEmpty())
            m_queue.insert(at++, across);
    }
    m_waitingTool = -1;
    m_waitingHold = false;
    m_holdQueued = false;
    m_ackClock.restart();
    emit consoleLine(QStringLiteral("== tool change done, continuing =="));
    if (at != m_nextIndex)
        emit progressChanged(m_ackedCount, m_queue.size());
    pump();
}

// Everything in flight is void after a controller reset we did not ask for.
// Drop the program and the macro, and tell whoever is listening that they
// ended - unsuccessfully.
void GrblStreamer::abortAfterReset()
{
    const bool wasProgram = m_streaming, wasMacro = m_macroActive;
    m_streaming = false;
    m_waitingTool = -1;
    m_waitingHold = false;
    m_holdQueued = false;
    m_queue.clear();
    m_inflightSizes.clear();
    m_inflightBytes = 0;
    m_macroActive = false;
    m_macroInflight = false;
    m_macroSkipAck = false;                     // its ack died with the reset
    m_macroQueue.clear();
    m_tloPending = (m_tlo != 0.0);
    m_tloRetries = 0;
    emit errorOccurred(QStringLiteral("controller reset: %1 stopped")
                           .arg(wasProgram ? QStringLiteral("the program")
                                           : QStringLiteral("the tool measurement")));
    if (wasMacro)
        emit macroFinished(false);
    if (wasProgram)
        emit streamFinished(false);
}

void GrblStreamer::pauseStream()
{
    if (isConnected())
        m_port->write("!", 1);
}

bool GrblStreamer::resumeStream()
{
    if (m_errorResetTimer->isActive()) {     // see sendRealtime
        emit errorOccurred(QStringLiteral("resume refused: the controller is being reset "
                                          "after an error"));
        return false;
    }
    if (!isConnected())
        return false;
    m_port->write("~", 1);
    return true;
}

void GrblStreamer::abortStream(const QString &why, bool holdMachine)
{
    if (!m_streaming)
        return;
    // A feed hold decelerates to a stop and keeps the machine's position.
    // It is not the end of it: GRBL still holds the program lines already
    // sent, and the spindle keeps turning, so a Resume (~) would run them
    // with nothing tracking their acks. Once the hold has settled, a soft
    // reset throws them away and stops the spindle, as cancelJob() does;
    // ctrl-X while moving would lose the position, hence the hold first.
    const bool reset = holdMachine && isConnected();
    if (reset) {
        const char hold = '!';
        m_port->write(&hold, 1);
    }
    m_streaming = false;
    m_waitingTool = -1;
    m_waitingHold = false;
    m_holdQueued = false;
    m_queue.clear();
    m_inflightSizes.clear();
    m_inflightBytes = 0;
    m_hadError = true;
    emit errorOccurred(reset ? QStringLiteral("program stopped: %1 - holding, then resetting "
                                              "the controller").arg(why)
                             : QStringLiteral("program stopped: %1").arg(why));
    if (reset) {
        m_errorResetClock.start();
        m_errorResetTimer->start();
    }
    emit streamFinished(false);
}

// Polled after a held abort: soft-reset as soon as the hold has come to rest
// (Hold:0), or after 5 s whatever the state, the same wait as cancelJob().
// The first 300 ms are not trusted: the last status report may predate the
// hold.
void GrblStreamer::resetAfterError()
{
    if (!isConnected()) {
        m_errorResetTimer->stop();
        return;
    }
    const QString &st = m_status.state;
    const bool still = m_errorResetClock.elapsed() >= 300
                       && (st == QLatin1String("Hold:0") || st == QLatin1String("Idle"));
    if (!still && m_errorResetClock.elapsed() < 5000)
        return;
    stopStream();                               // stops the timer too
    emit errorOccurred(QStringLiteral("controller reset after the error: the rest of the "
                                      "program was discarded and the spindle stopped"));
}

void GrblStreamer::stopStream()
{
    if (!isConnected())
        return;
    m_afterReset.clear();                       // a reset of its own: no $H after it
    const char reset = 0x18;                    // ctrl-X: GRBL soft reset
    m_port->write(&reset, 1);
    m_errorResetTimer->stop();                  // this is the reset it waited for
    const bool wasProgram = m_streaming;
    m_streaming = false;
    m_waitingTool = -1;
    m_waitingHold = false;
    m_holdQueued = false;
    m_queue.clear();
    m_inflightSizes.clear();
    m_inflightBytes = 0;
    const bool wasMacro = m_macroActive;
    m_macroActive = false;
    m_macroInflight = false;
    // A cancelled macro's line still waiting for its ok is flushed by the
    // reset and never answered: left set, the flag would eat the first ok of
    // the next macro or program, which then waits for ever.
    m_macroSkipAck = false;
    m_macroQueue.clear();
    m_adhocPending = 0;                         // reset flushes GRBL's buffer
    m_adhocLines.clear();
    m_tloPending = (m_tlo != 0.0);              // GRBL forgets G43.1 on reset
    m_tloRetries = 0;
    emit consoleLine(QStringLiteral("> [soft reset]"));
    if (wasMacro)
        emit macroFinished(false);
    if (wasProgram)
        emit streamFinished(false);
}

void GrblStreamer::resetThen(const QString &line)
{
    if (!isConnected())
        return;
    stopStream();
    m_afterReset = line.trimmed();
    m_afterResetClock.start();
}

void GrblStreamer::pump()
{
    // Locked by a limit (an ALARM:1 taken as the answer of a command, say):
    // nothing written now is answered, and the program would wait for ever.
    if (m_needsReset && m_streaming) {
        abortStream(QStringLiteral("the controller stopped on a limit (reset the controller "
                                   "to continue)"), false);
        return;
    }
    while (m_streaming && m_waitingTool < 0 && !m_waitingHold && m_adhocPending == 0
           && m_nextIndex < m_queue.size()) {
        const QString &line = m_queue.at(m_nextIndex);
        if (line == kHoldMarker) {
            // Everything before it answered, then stand here.
            if (!m_inflightSizes.isEmpty())
                break;
            m_waitingHold = true;
            ++m_nextIndex;
            ++m_ackedCount;                      // the marker counts as done
            emit progressChanged(m_ackedCount, m_queue.size());
            emit consoleLine(QStringLiteral("== held at the top, spindle off =="));
            emit holdPointReached();
            break;
        }
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
            emit toolChangeRequested(m_waitingTool, m_toolChanges++);
            break;
        }
        if (m_conservative && !m_inflightSizes.isEmpty())
            break;
        const QByteArray data = line.toLatin1() + '\n';
        // A line longer than the whole window can never satisfy the "wait for
        // room" test, so waiting meant waiting for ever: nothing was written,
        // so no ack could arrive, and the program stalled silently with the
        // spindle running. Send it on its own once the window is empty --
        // GRBL's own buffer is larger than ours and will answer it, with
        // error:14 at worst, which keeps the byte accounting sound.
        if (data.size() > RX_BUFFER) {
            if (!m_inflightSizes.isEmpty())
                break;                           // drain first, then send alone
        } else if (m_inflightBytes + data.size() > RX_BUFFER) {
            break;
        }
        m_port->write(data);
        if (m_conservative)
            emit consoleLine(QStringLiteral(">> %1").arg(line));
        m_inflightSizes.append(data.size());
        m_inflightBytes += data.size();
        ++m_nextIndex;
    }
    if (m_streaming && m_waitingTool < 0 && !m_waitingHold && m_ackedCount >= m_queue.size()) {
        m_streaming = false;
        emit streamFinished(!m_hadError);
        flushTlo();
    }
}

void GrblStreamer::startMacro(const QStringList &lines)
{
    if (!isConnected() || m_macroActive || m_errorResetTimer->isActive()
        || (m_streaming && m_waitingTool < 0) || m_needsReset) {
        if (m_needsReset && !m_resetNagged)
            emit errorOccurred(QStringLiteral("not started: the controller stopped on a limit "
                                              "and listens only after a reset"));
        m_resetNagged = m_resetNagged || m_needsReset;
        emit macroFinished(false);
        return;
    }
    m_macroDone = 0;
    m_macroQueue.clear();
    for (const QString &l : lines)
        if (!l.trimmed().isEmpty())
            m_macroQueue << l.trimmed();
    m_macroActive = true;
    m_macroInflight = false;
    m_macroError = false;
    pumpMacro();
}

void GrblStreamer::cancelMacro()
{
    if (!m_macroActive)
        return;
    m_macroQueue.clear();
    m_macroActive = false;
    m_macroSkipAck = m_macroInflight;     // its ok is still to come
    m_macroInflight = false;
    m_macroError = true;
    emit macroFinished(false);
}

void GrblStreamer::pumpMacro()
{
    // The same for a macro: its line in flight and the rest are never
    // answered once the controller is locked.
    if (m_macroActive && m_needsReset) {
        m_macroQueue.clear();
        m_macroActive = false;
        m_macroInflight = false;
        m_macroError = true;
        emit macroFinished(false);
        return;
    }
    if (!m_macroActive || m_macroInflight || m_adhocPending > 0)
        return;
    if (m_macroQueue.isEmpty()) {
        m_macroActive = false;
        emit macroFinished(!m_macroError);
        flushTlo();
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

QStringList GrblStreamer::toolChangeRehearsalLines(const BitSetterConfig &cfg, int tool)
{
    // No M2: there is no job to end, and the offset the measurement applied
    // should be left exactly as it is for the operator to look at.
    const QString safe = QStringLiteral("G53 G0 Z%1").arg(cfg.safeZ, 0, 'f', 3);
    return {QStringLiteral("M5"), QStringLiteral("G90"), safe,
            QStringLiteral("M0 ;T%1").arg(tool), safe, QStringLiteral("M5")};
}

void GrblStreamer::measureTool(const BitSetterConfig &cfg, bool returnHome,
                               double returnX, double returnY)
{
    m_probeOk = false;
    startMacro(measureToolLines(cfg, returnHome, returnX, returnY));
}

void GrblStreamer::applyToolLengthOffset(double mm)
{
    m_tloBefore = m_tlo;
    m_tlo = mm;
    m_tloPending = true;
    m_tloRetries = 0;
    flushTlo();
}

void GrblStreamer::flushTlo()
{
    if (!m_tloPending || m_tloWaitIdle || !canSendCommand())
        return;                                 // busy: sent when the line frees up
    m_tloPending = false;
    ++m_adhocPending;
    m_adhocLines << QStringLiteral("G43.1");
    m_adhocClock.restart();
    writeLine(QStringLiteral("G43.1 Z%1").arg(m_tlo, 0, 'f', 3));
}

void GrblStreamer::resyncAdhoc(const QString &why)
{
    if (m_adhocPending == 0)
        return;
    emit consoleLine(QStringLiteral("!! %1 — dropping %2 unanswered command(s)")
                         .arg(why).arg(m_adhocPending));
    m_adhocPending = 0;
    m_adhocLines.clear();
    pumpMacro();
    pump();
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

double GrblStreamer::reportScale() const
{
    return m_settings.value(13, 0) == 1 ? 25.4 : 1.0;
}

void GrblStreamer::handleLine(const QByteArray &line)
{
    // `$$` listing: $130=1250.000. Kept for the jog limits.
    if (line.startsWith('$') && line.contains('=')) {
        const int eq = line.indexOf('=');
        bool okN = false, okV = false;
        const int n = line.mid(1, eq - 1).toInt(&okN);
        const double v = line.mid(eq + 1).toDouble(&okV);
        if (okN && okV)
            m_settings.insert(n, v);
    }
    // Realtime status: <Idle|MPos:10.000,20.000,-1.000|FS:500,8000|WCO:0,0,0>
    if (line.startsWith('<')) {
        const QList<QByteArray> parts = line.mid(1).chopped(line.endsWith('>') ? 1 : 0).split('|');
        MachineStatus st = m_status;
        st.state = parts.isEmpty() ? QString() : QString::fromLatin1(parts.first());
        st.probePin = false;
        st.limitPins.clear();
        bool haveM = false, haveW = false;
        double px = 0, py = 0, pz = 0;
        // $13=1 reports positions in inches; everything here works in mm
        // (a jog went 86 mm the wrong way, Run refused a job that fit). The
        // WCO is kept as reported: GRBL sends it rarely, the first one before
        // the $$ listing says which unit it is in.
        const double k = reportScale();
        for (const QByteArray &p : parts) {
            if (p.startsWith("WCO:")) {
                const QList<QByteArray> c = p.mid(4).split(',');
                if (c.size() >= 3) {
                    m_wcoX = c.at(0).toDouble();   // raw: scaled on use
                    m_wcoY = c.at(1).toDouble();   // raw: scaled on use
                    m_wcoZ = c.at(2).toDouble();   // raw: scaled on use
                    m_tloAtWco = m_tlo;            // what this WCO Z carries, if G43.1 was sent
                    m_haveWco = true;
                }
            } else if (p.startsWith("MPos:") || p.startsWith("WPos:")) {
                const QList<QByteArray> c = p.mid(5).split(',');
                if (c.size() >= 3) {
                    px = c.at(0).toDouble() * k;
                    py = c.at(1).toDouble() * k;
                    pz = c.at(2).toDouble() * k;
                    haveM = p.startsWith("MPos:");
                    haveW = !haveM;
                }
            } else if (p.startsWith("FS:")) {
                const QList<QByteArray> c = p.mid(3).split(',');
                if (c.size() >= 2) {
                    st.feed = c.at(0).toDouble() * k;   // inch/min with $13=1
                    st.spindle = c.at(1).toDouble();
                }
            } else if (p.startsWith("Ov:")) {
                const QList<QByteArray> c = p.mid(3).split(',');
                if (c.size() >= 3) {
                    st.ovFeed = c.at(0).toInt();
                    st.ovRapid = c.at(1).toInt();
                    st.ovSpindle = c.at(2).toInt();
                }
            } else if (p.startsWith("Pn:")) {
                st.probePin = p.indexOf('P', 3) >= 0;   // past the "Pn:" itself
                for (char a : {'X', 'Y', 'Z', 'A'})
                    if (p.indexOf(a, 3) >= 0)
                        st.limitPins += QLatin1Char(a);
            }
        }
        if (haveM) {
            st.mx = px; st.my = py; st.mz = pz;
            st.wx = px - m_wcoX * k; st.wy = py - m_wcoY * k; st.wz = pz - m_wcoZ * k;
        } else if (haveW) {
            st.wx = px; st.wy = py; st.wz = pz;
            st.mx = px + m_wcoX * k; st.my = py + m_wcoY * k; st.mz = pz + m_wcoZ * k;
        }
        st.valid = true;
        m_status = st;
        if (m_tloWaitIdle && st.state == QLatin1String("Idle")) {
            m_tloWaitIdle = false;
            flushTlo();
        }
        // An ad-hoc line that is still unanswered while the controller sits
        // Idle for seconds was never received (typical right after a DTR
        // reset). Without this every stream and macro would wait forever.
        if (m_adhocPending > 0 && st.state == QLatin1String("Idle")
            && m_adhocClock.isValid() && m_adhocClock.elapsed() > 3000)
            resyncAdhoc(QStringLiteral("no ack while Idle"));
        // GRBL answers every line at once in Alarm (error:9 at worst). One
        // left unanswered there means a controller locked by a limit: one
        // that was locked before this connection opened, which never said
        // ALARM:1 to us. Without this $H and $X wait for ever.
        if (m_adhocPending > 0 && !m_needsReset && st.state.startsWith(QLatin1String("Alarm"))
            && m_adhocClock.isValid() && m_adhocClock.elapsed() > 2000) {
            m_needsReset = true;
            emit errorOccurred(QStringLiteral("the controller does not answer in Alarm: it "
                                              "stopped on a limit and listens only after a "
                                              "reset (Initialize or Unlock does it)"));
            m_resetNagged = true;
            // A $H or $X among them was the operator asking out of the alarm:
            // do it the way that works, reset first.
            QString retry;
            for (const QString &l : std::as_const(m_adhocLines))
                if (l == QLatin1String("$H") || l == QLatin1String("$X"))
                    retry = l;
            resyncAdhoc(QStringLiteral("controller locked"));
            if (!retry.isEmpty())
                resetThen(retry);
        }
        emit statusReport(st.state, st.mx, st.my, st.mz);
        emit statusChanged(st);
        // Waiting to reset after an error: do it on the report that shows
        // the hold at rest, not on the next poll, so a Resume can't slip in.
        if (m_errorResetTimer->isActive())
            resetAfterError();
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
            const double k = reportScale();
            m_probeZ = c.at(2).toDouble() * k;
            emit probeResult(c.at(0).toDouble() * k, c.at(1).toDouble() * k, m_probeZ, m_probeOk);
        }
        return;
    }

    // After a reset GRBL forgets G43.1 (and nothing sent before the reset
    // will be answered). Put the offset back once the controller is ready;
    // if it boots into Alarm the $X / $H ack re-tries it.
    if (line.startsWith("Grbl ")) {
        m_heard = true;
        m_adhocPending = 0;
        m_adhocLines.clear();
        m_macroSkipAck = false;             // nothing from before is answered now
        // The controller rebooted under us - reset button, brown-out, a USB
        // stall. Its RX buffer, modal state and G43.1 are gone and every line
        // in flight will never be answered. Pushing the rest of the program at
        // it would cut with the wrong modal state and the wrong tool offset;
        // and because those acks never arrive, m_ackedCount would stay short
        // of the queue for ever - progress frozen near 100%, streamFinished
        // never emitted, canSendCommand() false, so even jog and $X are
        // refused. Abort instead, and say so.
        if (m_streaming || m_macroActive)
            abortAfterReset();
        if (m_tlo != 0.0) {
            m_tloPending = true;
            QTimer::singleShot(400, this, [this] { flushTlo(); });
        }
        m_needsReset = false;
        m_resetNagged = false;
        // The $H / $X a resetThen() waited for. Only right after that reset:
        // a banner minutes later (the reset button) must not start homing.
        const QString after = m_afterReset;
        m_afterReset.clear();
        if (!after.isEmpty() && m_afterResetClock.isValid()
            && m_afterResetClock.elapsed() < 10000)
            sendCommand(after);
        return;
    }

    const bool ok = (line == "ok");
    const bool alarm = line.startsWith("ALARM:");
    const bool err = line.startsWith("error:") || alarm;
    if (ok || line.startsWith("error:"))
        m_heard = true;
    // Hard limit (1) or soft limit (2): GRBL locks up until a soft reset and
    // says "Reset to continue". Remember it, or $H and $X go unanswered.
    if (alarm) {
        const int n = line.mid(6).toInt();
        if (n == 1 || n == 2)
            m_needsReset = true;
    } else if (line.contains("Reset to continue")) {
        m_needsReset = true;
    }

    if ((ok || err) && m_adhocPending > 0) {
        --m_adhocPending;
        m_adhocClock.restart();
        const QString sent = m_adhocLines.isEmpty() ? QString() : m_adhocLines.takeFirst();
        if (err)
            emit errorOccurred(QString::fromLatin1(line));
        // Unlock and homing both leave GRBL without the G43.1 it had.
        if (ok && (sent == QLatin1String("$X") || sent == QLatin1String("$H")) && m_tlo != 0.0)
            m_tloPending = true;
        // A refused G43.1 (error:9 while a jog still runs) left the controller
        // without the offset the panel shows: the next job cut that far off.
        // Sent again once the controller reports Idle.
        // A controller that keeps refusing it gets three tries, not one per
        // status report forever.
        if (sent == QLatin1String("G43.1")) {
            if (!err) {
                m_tloRetries = 0;
            } else if (++m_tloRetries <= 3) {
                m_tloPending = true;
                m_tloWaitIdle = true;
            } else {
                m_tloRetries = 0;
                emit errorOccurred(QStringLiteral("the controller refuses the tool length offset "
                                                  "(G43.1) — it is not applied"));
            }
        }
        emit commandAnswered(sent, ok);
        if (m_adhocPending == 0) {
            flushTlo();
            pumpMacro();
            pump();
        }
        return;
    }

    if (m_macroSkipAck && (ok || err)) {
        m_macroSkipAck = false;             // the cancelled macro's last line
        return;
    }
    if (m_macroActive && (ok || err)) {
        m_macroInflight = false;
        if (ok)
            ++m_macroDone;
        if (err) {
            m_macroError = true;
            m_macroQueue.clear();
            emit errorOccurred(QString::fromLatin1(line));
        }
        pumpMacro();
        return;
    }

    // A critical alarm - a hard or soft limit, which is a routine event on a
    // Shapeoko - is not a per-line error. GRBL stops, answers nothing but `?`
    // until it is reset, and never acks the lines already in its buffer. It
    // does not reboot, so the "Grbl " banner path above never fires. Counting
    // the alarm as one ack and waiting for the others left m_streaming true
    // for ever: progress frozen, streamFinished never emitted, and because
    // canSendCommand() is false while streaming, unlock, homing and jog all
    // silently did nothing.
    if (alarm && m_streaming) {
        abortStream(QString::fromLatin1(line)
                        + QStringLiteral(" (reset the controller to continue)"),
                    false);
        return;
    }

    if ((ok || err) && m_streaming && !m_inflightSizes.isEmpty()) {
        m_inflightBytes -= m_inflightSizes.takeFirst();
        ++m_ackedCount;
        m_ackClock.restart();
        if (err) {
            emit errorOccurred(QString::fromLatin1(line));
            // GRBL throws the rejected block away and carries straight on with
            // whatever else is in its buffer. If the rejected line was the
            // retract, the next move is a rapid at cutting depth: 50 mm across
            // the part with the tool 3 mm under the surface. Every other sender
            // halts here, and so do we.
            abortStream(QStringLiteral("%1 on line %2")
                            .arg(QString::fromLatin1(line)).arg(m_ackedCount),
                        true);
            return;
        }
        emit progressChanged(m_ackedCount, m_queue.size());
        pump();                                  // also handles completion
    } else if (err) {
        emit errorOccurred(QString::fromLatin1(line));
    }
}

} // namespace c2d
