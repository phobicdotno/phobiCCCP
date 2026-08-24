#include "grblsender.h"

namespace c2d {

void GrblSender::start(const QStringList &lines)
{
    m_pending.clear();
    for (const QString &line : lines) {
        const QString t = line.trimmed();
        if (!t.isEmpty())
            m_pending.append(t);
    }
    m_rx.clear();
    m_total = m_pending.size();
    m_sent = 0;
    m_acked = 0;
    m_errors = 0;
    m_lastError.clear();
    m_active = m_total > 0;
    if (m_active)
        sendNext();
}

void GrblSender::sendNext()
{
    if (m_sent >= m_total) {
        return;   // everything is out; waiting for the final ok
    }
    const QString line = m_pending.at(m_sent);
    ++m_sent;
    m_writer(line.toLatin1() + '\n');
}

void GrblSender::onData(const QByteArray &chunk)
{
    if (!m_active)
        return;
    m_rx.append(chunk);
    int nl;
    while (m_active && (nl = m_rx.indexOf('\n')) >= 0) {
        const QByteArray line = m_rx.left(nl).trimmed();
        m_rx.remove(0, nl + 1);
        if (line == "ok") {
            ++m_acked;
        } else if (line.startsWith("error:")) {
            ++m_acked;
            ++m_errors;
            m_lastError = QString::fromLatin1(line);
        } else {
            continue;   // banner, status report or alarm text; not an ack
        }
        if (m_acked >= m_total) {
            m_active = false;
            return;
        }
        sendNext();
    }
}

void GrblSender::abort()
{
    m_active = false;
    m_pending.clear();
    m_rx.clear();
}

} // namespace c2d
