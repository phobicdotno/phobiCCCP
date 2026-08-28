#include "grblstreamer.h"

#include <QSerialPort>
#include <QSerialPortInfo>
#include <QTimer>

namespace c2d {

GrblStreamer::GrblStreamer(QObject *parent)
    : QObject(parent), m_statusTimer(new QTimer(this))
{
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
    m_rx.clear();
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
        emit disconnected();
    }
}

bool GrblStreamer::isConnected() const
{
    return m_port && m_port->isOpen();
}

void GrblStreamer::sendCommand(const QString &line)
{
    if (!isConnected() || m_streaming)
        return;
    const QByteArray data = line.trimmed().toLatin1() + '\n';
    m_port->write(data);
    emit consoleLine(QStringLiteral("> %1").arg(line.trimmed()));
}

void GrblStreamer::requestStatus()
{
    if (isConnected())
        m_port->write("?", 1);
}

void GrblStreamer::startStream(const QStringList &lines)
{
    if (!isConnected() || m_streaming)
        return;
    m_queue.clear();
    for (const QString &l : lines) {
        const QString s = l.trimmed();
        if (!s.isEmpty())
            m_queue << s;
    }
    m_nextIndex = 0;
    m_ackedCount = 0;
    m_inflightSizes.clear();
    m_inflightBytes = 0;
    m_hadError = false;
    m_streaming = !m_queue.isEmpty();
    emit progressChanged(0, m_queue.size());
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
    m_streaming = false;
    m_queue.clear();
    m_inflightSizes.clear();
    m_inflightBytes = 0;
    emit consoleLine(QStringLiteral("> [soft reset]"));
    emit streamFinished(false);
}

void GrblStreamer::pump()
{
    while (m_streaming && m_nextIndex < m_queue.size()) {
        const QByteArray data = m_queue.at(m_nextIndex).toLatin1() + '\n';
        if (m_inflightBytes + data.size() > RX_BUFFER)
            break;
        m_port->write(data);
        m_inflightSizes.append(data.size());
        m_inflightBytes += data.size();
        ++m_nextIndex;
    }
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
    // Realtime status: <Idle|MPos:10.000,20.000,-1.000|FS:500,8000>
    if (line.startsWith('<')) {
        const QList<QByteArray> parts = line.mid(1).chopped(line.endsWith('>') ? 1 : 0).split('|');
        QString state = parts.isEmpty() ? QString() : QString::fromLatin1(parts.first());
        double mx = 0, my = 0, mz = 0;
        for (const QByteArray &p : parts) {
            if (p.startsWith("MPos:") || p.startsWith("WPos:")) {
                const QList<QByteArray> c = p.mid(5).split(',');
                if (c.size() >= 3) {
                    mx = c.at(0).toDouble();
                    my = c.at(1).toDouble();
                    mz = c.at(2).toDouble();
                }
            }
        }
        emit statusReport(state, mx, my, mz);
        return;
    }

    emit consoleLine(QString::fromLatin1(line));

    const bool ok = (line == "ok");
    const bool err = line.startsWith("error:") || line.startsWith("ALARM:");
    if ((ok || err) && m_streaming && !m_inflightSizes.isEmpty()) {
        m_inflightBytes -= m_inflightSizes.takeFirst();
        ++m_ackedCount;
        if (err) {
            m_hadError = true;
            emit errorOccurred(QString::fromLatin1(line));
        }
        emit progressChanged(m_ackedCount, m_queue.size());
        if (m_ackedCount >= m_queue.size()) {
            m_streaming = false;
            emit streamFinished(!m_hadError);
        } else {
            pump();
        }
    } else if (err) {
        emit errorOccurred(QString::fromLatin1(line));
    }
}

} // namespace c2d
