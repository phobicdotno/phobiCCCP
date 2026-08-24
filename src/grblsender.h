#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>

// Line-by-line GRBL streaming protocol, decoupled from the transport: the
// owner passes a writer callback (a QSerialPort in the app, a stub in tests)
// and feeds received bytes into onData(). One line is in flight at a time;
// the next goes out when GRBL answers "ok" (an "error:N" is counted, logged
// and the stream continues, matching how senders usually treat minor errors).
namespace c2d {

class GrblSender
{
public:
    using Writer = std::function<void(const QByteArray &)>;

    explicit GrblSender(Writer writer) : m_writer(std::move(writer)) {}

    void start(const QStringList &lines);
    void onData(const QByteArray &chunk);   // raw bytes from the port
    void abort();

    bool active() const   { return m_active; }
    bool finished() const { return !m_active && m_total > 0 && m_acked == m_total; }
    int total() const     { return m_total; }
    int acked() const     { return m_acked; }
    int errors() const    { return m_errors; }
    QString lastError() const { return m_lastError; }

private:
    void sendNext();

    Writer m_writer;
    QStringList m_pending;
    QByteArray m_rx;
    bool m_active = false;
    int m_total = 0;
    int m_sent = 0;
    int m_acked = 0;
    int m_errors = 0;
    QString m_lastError;
};

} // namespace c2d
