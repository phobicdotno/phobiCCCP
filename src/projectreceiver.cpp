#include "projectreceiver.h"

#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QRegularExpression>
#include <QSysInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>

#include <cmath>
#include <limits>

namespace c2d {

static const QByteArray kHello = QByteArrayLiteral("PHOBICCCP-SEND 1\n");
static const QByteArray kApiHello = QByteArrayLiteral("PHOBICCCP-API 1\n");
static const QByteArray kDiscover = QByteArrayLiteral("PHOBICCCP-DISCOVER 1");

int ReceivedProject::count(const QString &role) const
{
    int n = 0;
    for (const ReceivedFile &f : files)
        n += f.role == role;
    return n;
}

const ReceivedFile *ReceivedProject::first(const QString &role) const
{
    for (const ReceivedFile &f : files)
        if (f.role == role)
            return &f;
    return nullptr;
}

QString safeFileName(const QString &name)
{
    QString s = name;
    s.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._ -]")), QStringLiteral("_"));
    while (s.startsWith(QLatin1Char('.')) || s.startsWith(QLatin1Char(' ')))
        s.remove(0, 1);
    while (s.endsWith(QLatin1Char('.')) || s.endsWith(QLatin1Char(' ')))
        s.chop(1);
    s.truncate(80);
    return s.isEmpty() ? QStringLiteral("project") : s;
}

// ---- PackageReader --------------------------------------------------------

bool PackageReader::fail(const QString &why)
{
    m_state = Failed;
    m_error = why;
    m_buf.clear();
    return false;
}

bool PackageReader::parseManifest(const QByteArray &line)
{
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &pe);
    if (!doc.isObject())
        return fail(QStringLiteral("the manifest is not a JSON object"));
    const QJsonObject m = doc.object();
    m_project.name = safeFileName(m.value(QStringLiteral("name")).toString());
    m_project.from = m.value(QStringLiteral("from")).toString().left(120);
    m_project.app = m.value(QStringLiteral("app")).toString().left(120);
    const QJsonObject st = m.value(QStringLiteral("stock")).toObject();
    auto dim = [&](const char *key) {
        const double v = st.value(QLatin1String(key)).toDouble();
        return std::isfinite(v) && v > 0 && v <= 10000 ? v : 0.0;
    };
    m_project.stockWidth = dim("width");
    m_project.stockHeight = dim("height");
    m_project.stockThickness = dim("thickness");

    const QJsonArray files = m.value(QStringLiteral("files")).toArray();
    if (files.isEmpty())
        return fail(QStringLiteral("the manifest lists no files"));
    if (files.size() > kMaxFiles)
        return fail(QStringLiteral("too many files (%1)").arg(files.size()));
    static const QRegularExpression okName(QStringLiteral("^[A-Za-z0-9_ -][A-Za-z0-9._ -]{0,99}$"));
    qint64 total = 0;
    QStringList names;   // as they will be saved: "a" and "a " are the same file
    for (const QJsonValue &v : files) {
        const QJsonObject f = v.toObject();
        ReceivedFile rf;
        rf.name = f.value(QStringLiteral("name")).toString();
        rf.role = f.value(QStringLiteral("role")).toString();
        const double size = f.value(QStringLiteral("size")).toDouble(-1);
        if (!okName.match(rf.name).hasMatch() || names.contains(safeFileName(rf.name), Qt::CaseInsensitive))
            return fail(QStringLiteral("bad file name %1").arg(rf.name.left(40)));
        if (rf.role != QLatin1String("model") && rf.role != QLatin1String("vectors")
            && rf.role != QLatin1String("project"))
            return fail(QStringLiteral("unknown file role %1").arg(rf.role.left(20)));
        if (!(size >= 0) || size > kMaxTotal || size != std::floor(size))
            return fail(QStringLiteral("bad size for %1").arg(rf.name));
        total += qint64(size);
        if (total > kMaxTotal)
            return fail(QStringLiteral("the project is larger than %1 MB").arg(kMaxTotal >> 20));
        rf.x = f.value(QStringLiteral("x")).toDouble();
        rf.y = f.value(QStringLiteral("y")).toDouble();
        if (!std::isfinite(rf.x) || !std::isfinite(rf.y))
            rf.x = rf.y = 0;
        // No reserve(size): the size is the sender's word, the data grows as it arrives.
        m_project.files.append(rf);
        names.append(safeFileName(rf.name));
        m_sizes.append(qint64(size));
    }
    return true;
}

qint64 PackageReader::announcedSize() const
{
    qint64 total = 0;
    for (qint64 n : m_sizes)
        total += n;
    return total;
}

bool PackageReader::feed(const QByteArray &bytes)
{
    if (m_state == Failed)
        return false;
    if (m_state == Done)
        return bytes.isEmpty() || fail(QStringLiteral("more data after the last file"));
    m_buf.append(bytes);
    for (;;) {
        if (m_state == Hello) {
            const int n = int(qMin<qsizetype>(m_buf.size(), kHello.size()));
            if (m_buf.left(n) != kHello.left(n))
                return fail(QStringLiteral("not a phobiCCCP project transfer"));
            if (n < kHello.size())
                return true;
            m_buf.remove(0, kHello.size());
            m_state = Manifest;
        }
        if (m_state == Manifest) {
            const qsizetype nl = m_buf.indexOf('\n');
            if (nl < 0) {
                if (m_buf.size() > kMaxManifest)
                    return fail(QStringLiteral("the manifest is too long"));
                return true;
            }
            if (nl > kMaxManifest)
                return fail(QStringLiteral("the manifest is too long"));
            const QByteArray line = m_buf.left(nl);
            m_buf.remove(0, nl + 1);
            if (!parseManifest(line))
                return false;
            m_state = Body;
        }
        if (m_state == Body) {
            while (m_file < m_project.files.size()) {
                QByteArray &d = m_project.files[m_file].data;
                const qint64 want = m_sizes.at(m_file) - d.size();
                if (want > 0) {
                    if (m_buf.isEmpty())
                        return true;
                    const qsizetype take = qsizetype(qMin<qint64>(want, m_buf.size()));
                    d.append(m_buf.constData(), take);
                    m_buf.remove(0, take);
                }
                if (d.size() == m_sizes.at(m_file))
                    ++m_file;
            }
            m_state = Done;
            if (!m_buf.isEmpty())
                return fail(QStringLiteral("more data after the last file"));
            return true;
        }
        return true;
    }
}

// ---- saving ---------------------------------------------------------------

QString saveReceived(const ReceivedProject &project, const QString &baseDir,
                     QStringList *paths, QString *error)
{
    const QString name = safeFileName(project.name);
    QDir base(baseDir);
    if (!base.mkpath(QStringLiteral("."))) {
        if (error) *error = QStringLiteral("Cannot create %1").arg(baseDir);
        return {};
    }
    QString dirName = name;
    for (int n = 2; base.exists(dirName); ++n)
        dirName = QStringLiteral("%1-%2").arg(name).arg(n);
    if (!base.mkdir(dirName)) {
        if (error) *error = QStringLiteral("Cannot create %1").arg(base.filePath(dirName));
        return {};
    }
    const QDir dir(base.filePath(dirName));
    for (const ReceivedFile &f : project.files) {
        const QString path = dir.filePath(safeFileName(f.name));
        QFile out(path);
        if (!out.open(QIODevice::WriteOnly) || out.write(f.data) != f.data.size()) {
            if (error) *error = QStringLiteral("Cannot write %1: %2").arg(path, out.errorString());
            return {};
        }
        if (paths) paths->append(path);
    }
    return dir.absolutePath();
}

// ---- ProjectReceiver ------------------------------------------------------

struct ProjectReceiver::Conn {
    PackageReader reader;
    QTimer idle;
    QTimer deadline;      // absolute: does not restart as bytes arrive
    bool active = false;  // past its manifest: the one transfer being taken
    // Until the hello is in, it is not known which protocol this is.
    QByteArray head;
    bool api = false;     // a pad's request connection
    bool pending = false; // an API request is waiting for respond()
    QByteArray buf;       // API bytes not yet read as a request
    int badLines = 0;     // lines that were not a JSON object
};

ProjectReceiver::ProjectReceiver(QObject *parent) : QObject(parent) {}

ProjectReceiver::~ProjectReceiver()
{
    stop();
}

bool ProjectReceiver::start(quint16 port, QString *error)
{
    stop();
    m_tcp = new QTcpServer(this);
    if (!m_tcp->listen(QHostAddress::Any, port)) {
        if (error) *error = m_tcp->errorString();
        delete m_tcp;
        m_tcp = nullptr;
        return false;
    }
    connect(m_tcp, &QTcpServer::newConnection, this, &ProjectReceiver::onConnection);
    // Discovery is a convenience: without it a host can still be typed in.
    m_udp = new QUdpSocket(this);
    if (m_udp->bind(QHostAddress::Any, m_tcp->serverPort(),
                    QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint))
        connect(m_udp, &QUdpSocket::readyRead, this, &ProjectReceiver::onDatagrams);
    return true;
}

void ProjectReceiver::stop()
{
    for (auto it = m_conns.begin(); it != m_conns.end(); ++it) {
        it.key()->disconnect(this);
        it.key()->abort();
        it.key()->deleteLater();
        delete it.value();
    }
    m_conns.clear();
    m_waiting.clear();
    m_apiWaiting.clear();
    delete m_tcp;
    m_tcp = nullptr;
    delete m_udp;
    m_udp = nullptr;
}

bool ProjectReceiver::isListening() const
{
    return m_tcp && m_tcp->isListening();
}

quint16 ProjectReceiver::port() const
{
    return m_tcp ? m_tcp->serverPort() : 0;
}

void ProjectReceiver::onDatagrams()
{
    while (m_udp && m_udp->hasPendingDatagrams()) {
        const QNetworkDatagram d = m_udp->receiveDatagram(4096);
        if (d.data() != kDiscover)
            continue;
        QJsonObject o;
        o.insert(QStringLiteral("app"), QStringLiteral("phobiCCCP"));
        o.insert(QStringLiteral("version"), QStringLiteral(PHOBICCCP_VERSION));
        o.insert(QStringLiteral("host"), QSysInfo::machineHostName());
        o.insert(QStringLiteral("port"), int(port()));
        for (auto it = m_discoveryExtra.constBegin(); it != m_discoveryExtra.constEnd(); ++it)
            if (!o.contains(it.key()))
                o.insert(it.key(), it.value());
        m_udp->writeDatagram(QJsonDocument(o).toJson(QJsonDocument::Compact),
                             d.senderAddress(), quint16(d.senderPort()));
    }
}

void ProjectReceiver::onConnection()
{
    while (m_tcp && m_tcp->hasPendingConnections()) {
        QTcpSocket *s = m_tcp->nextPendingConnection();
        if (m_conns.size() >= kMaxConns) {
            // As in drop(): a peer that never closes its end is cut off.
            connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            QTimer::singleShot(5000, s, [s] { s->abort(); s->deleteLater(); });
            s->write(QByteArrayLiteral("ERR busy\n"));
            s->disconnectFromHost();
            continue;
        }
        auto *c = new Conn;
        m_conns.insert(s, c);
        c->idle.setSingleShot(true);
        c->idle.setInterval(m_idleMs);
        connect(&c->idle, &QTimer::timeout, this, [this, s] {
            drop(s, QByteArrayLiteral("ERR timed out\n"));
        });
        c->idle.start();
        c->deadline.setSingleShot(true);
        connect(&c->deadline, &QTimer::timeout, this, [this, s] {
            drop(s, QByteArrayLiteral("ERR timed out\n"));
        });
        c->deadline.start(m_deadlineMs);
        connect(s, &QTcpSocket::readyRead, this, [this, s] { onData(s); });
        connect(s, &QTcpSocket::disconnected, this, [this, s] { drop(s, {}); });
    }
}

void ProjectReceiver::onData(QTcpSocket *s)
{
    Conn *c = m_conns.value(s);
    if (!c)
        return;
    c->idle.start();
    QByteArray bytes = s->readAll();
    if (c->api) {
        c->buf += bytes;
        // While a request waits for the user, nothing else is read from the
        // buffer: a pad may queue its next request, but not without limit.
        if (c->buf.size() > 2 * kMaxRequest) {
            drop(s, QByteArrayLiteral("{\"ok\":false,\"error\":\"request too long\"}\n"));
            return;
        }
        onApiData(s);
        return;
    }
    if (c->reader.state() == PackageReader::Hello) {
        c->head += bytes;
        const int n = int(qMin<qsizetype>(c->head.size(), kApiHello.size()));
        if (c->head.left(n) == kApiHello.left(n)) {
            if (n < kApiHello.size())
                return;    // could still be either
            c->api = true;
            c->buf = c->head.mid(kApiHello.size());
            c->head.clear();
            c->deadline.stop();   // a pad may stay a while; idle still applies
            onApiData(s);
            return;
        }
        bytes = c->head;
        c->head.clear();
    }
    if (c->reader.state() == PackageReader::Done)
        return;    // waiting for the user; ignore anything more
    if (!c->reader.feed(bytes)) {
        drop(s, "ERR " + c->reader.error().toUtf8() + '\n');
        return;
    }
    const PackageReader::State st = c->reader.state();
    if (!c->active && (st == PackageReader::Body || st == PackageReader::Done)) {
        // The manifest is in: take this transfer only if no other is being
        // sent or waiting for the user, so one project at a time is in memory.
        bool busy = !m_waiting.isEmpty();
        for (auto it = m_conns.cbegin(); it != m_conns.cend() && !busy; ++it)
            busy = it.value() != c && it.value()->active;
        if (busy) {
            drop(s, QByteArrayLiteral("ERR phobiCCCP is busy with another project; try again shortly\n"));
            return;
        }
        c->active = true;
        const qint64 extra = c->reader.announcedSize() / kMinRate * 1000;
        c->deadline.start(int(qMin<qint64>(m_deadlineMs + extra, std::numeric_limits<int>::max())));
    }
    if (st != PackageReader::Done)
        return;
    c->idle.stop();
    c->deadline.stop();
    const int id = m_nextId++;
    m_waiting.insert(id, s);
    ReceivedProject p = c->reader.project();
    p.peer = s->peerAddress().toString();
    if (p.peer.startsWith(QStringLiteral("::ffff:")))
        p.peer = p.peer.mid(7);
    emit received(id, p);
}

void ProjectReceiver::onApiData(QTcpSocket *s)
{
    Conn *c = m_conns.value(s);
    if (!c || c->pending)
        return;
    // A loop, not recursion: a burst of empty lines must not run the stack out.
    QJsonDocument doc;
    for (;;) {
        const qsizetype nl = c->buf.indexOf('\n');
        if (nl < 0) {
            if (c->buf.size() > kMaxRequest)
                drop(s, QByteArrayLiteral("{\"ok\":false,\"error\":\"request too long\"}\n"));
            return;
        }
        if (nl > kMaxRequest) {
            drop(s, QByteArrayLiteral("{\"ok\":false,\"error\":\"request too long\"}\n"));
            return;
        }
        const QByteArray line = c->buf.left(nl);
        c->buf.remove(0, nl + 1);
        doc = QJsonDocument::fromJson(line);
        if (doc.isObject())
            break;
        if (++c->badLines > kMaxBadLines) {
            drop(s, QByteArrayLiteral("{\"ok\":false,\"error\":\"not a JSON object\"}\n"));
            return;
        }
        s->write(QByteArrayLiteral("{\"ok\":false,\"error\":\"not a JSON object\"}\n"));
    }
    c->pending = true;
    c->idle.stop();
    const int id = m_nextId++;
    m_apiWaiting.insert(id, s);
    QString peer = s->peerAddress().toString();
    if (peer.startsWith(QStringLiteral("::ffff:")))
        peer = peer.mid(7);
    emit request(id, doc.object(), peer);
}

void ProjectReceiver::respond(int id, const QJsonObject &reply)
{
    QTcpSocket *s = m_apiWaiting.take(id);
    Conn *c = s ? m_conns.value(s) : nullptr;
    if (!c)
        return;    // the pad went away meanwhile
    c->pending = false;
    c->idle.start();
    s->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
    // A request that arrived while this one was answered.
    QTimer::singleShot(0, this, [this, s] {
        if (m_conns.contains(s))
            onApiData(s);
    });
}

void ProjectReceiver::answer(int id, bool ok, const QString &message)
{
    QTcpSocket *s = m_waiting.take(id);
    if (!s || !m_conns.contains(s))
        return;    // the sender gave up meanwhile
    QString line = message;
    line.replace(QLatin1Char('\n'), QLatin1Char(' '));
    drop(s, (ok ? "OK " : "ERR ") + line.toUtf8() + '\n');
}

void ProjectReceiver::drop(QTcpSocket *s, const QByteArray &reply)
{
    Conn *c = m_conns.take(s);
    if (!c)
        return;
    for (auto it = m_waiting.begin(); it != m_waiting.end();)
        it = it.value() == s ? m_waiting.erase(it) : std::next(it);
    for (auto it = m_apiWaiting.begin(); it != m_apiWaiting.end();)
        it = it.value() == s ? m_apiWaiting.erase(it) : std::next(it);
    s->disconnect(this);
    delete c;
    if (!reply.isEmpty() && s->state() == QAbstractSocket::ConnectedState) {
        // Gone only once the reply is out; a peer that never reads it is cut off.
        connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
        QTimer::singleShot(5000, s, [s] { s->abort(); s->deleteLater(); });
        s->write(reply);
        s->disconnectFromHost();
    } else {
        s->abort();
        s->deleteLater();
    }
}

} // namespace c2d
