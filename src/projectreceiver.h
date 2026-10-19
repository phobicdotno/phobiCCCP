#pragma once
#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVector>

class QTcpServer;
class QTcpSocket;
class QUdpSocket;

// Projects sent from Syntexophobic (File > Send to phobiCCCP…) over the local
// network or Tailscale. The link, version 1 — Syntexophobic's side is
// src/syntexophobic/send.py, which documents the same thing:
//
//   * Discovery: a UDP datagram "PHOBICCCP-DISCOVER 1" to kPort; the answer
//     is one JSON object {"app": "phobiCCCP", "version", "host", "port"}.
//   * Transfer: TCP to kPort, "PHOBICCCP-SEND 1\n", one line of JSON (the
//     manifest), then the bytes of every file it lists, in its order. The
//     sender waits for one line back, "OK <what was done>" or "ERR <why>",
//     which is only sent once the user here has said yes or no.
//
// Manifest: name, from (the sending computer), app, stock {width (X), height
// (Y), thickness (Z), mm}, files [{name, size, role}]. Roles: "model" (binary
// STL in stock coordinates: X0 Y0 the stock's front-left corner, Z0 its top,
// with x / y = the mesh's lower-left corner), "vectors" (DXF, same
// coordinates), "project" (the Syntexophobic project with its history, a
// .cccp zip; kept beside the design, not opened here).
//
// The PhobiCCCP Android app (Android/ in this repository) uses the same port:
//
//   * Discovery: as above. The answer also carries "id" (this install, so a
//     pad tells two computers with the same name apart), "api" (1) and
//     "machine" (the active machine's name).
//   * Requests: TCP to kPort, "PHOBICCCP-API 1\n", then any number of
//     requests, one JSON object per line, each answered by one JSON line
//     {"ok": true, ...} or {"ok": false, "error": "..."} before the next is
//     read. The requests themselves ("op": info, tools, designs, get, put,
//     machine, send, open) are answered by the main window; see
//     MainWindow::onPadRequest and Android/README.md.
namespace c2d {

struct ReceivedFile {
    QString name;          // bare file name, already checked (no path parts)
    QString role;          // model | vectors | project
    QByteArray data;
    double x = 0, y = 0;   // model: lower-left corner of the mesh on the stock, mm
};

struct ReceivedProject {
    QString name;          // safe as a file name
    QString from;          // the sending computer's name, as it says
    QString app;           // "Syntexophobic 0.7.1"
    QString peer;          // the address it came from
    double stockWidth = 0, stockHeight = 0, stockThickness = 0;
    QVector<ReceivedFile> files;

    int count(const QString &role) const;
    const ReceivedFile *first(const QString &role) const;
};

// The protocol on one connection, fed as bytes arrive. Everything a sender
// controls is bounded and checked before it is used: the manifest line, file
// count, names (no separators, no leading dot), roles and total size.
class PackageReader
{
public:
    static constexpr qint64 kMaxManifest = 64 * 1024;
    static constexpr qint64 kMaxTotal = 512LL * 1024 * 1024;
    static constexpr int kMaxFiles = 64;

    enum State { Hello, Manifest, Body, Done, Failed };

    // Returns false once the stream is broken; error() says why.
    bool feed(const QByteArray &bytes);
    State state() const { return m_state; }
    QString error() const { return m_error; }
    // Total announced size of the files, once the manifest is read.
    qint64 announcedSize() const;
    const ReceivedProject &project() const { return m_project; }

private:
    bool fail(const QString &why);
    bool parseManifest(const QByteArray &line);

    State m_state = Hello;
    QByteArray m_buf;
    QString m_error;
    ReceivedProject m_project;
    QVector<qint64> m_sizes;   // announced size of each file
    int m_file = 0;            // index of the file being filled
};

// A file or folder name made from something a sender wrote.
QString safeFileName(const QString &name);

// Write the received files into a new folder under `baseDir`
// (<baseDir>/<name>, or <name>-2, -3 … when taken). Returns the folder, or an
// empty string with `error` set. Paths of the written files are filled into
// `paths` (same order as project.files).
QString saveReceived(const ReceivedProject &project, const QString &baseDir,
                     QStringList *paths, QString *error = nullptr);

class ProjectReceiver : public QObject
{
    Q_OBJECT
public:
    static constexpr quint16 kPort = 47652;
    static constexpr int kIdleMs = 60 * 1000;   // a sender that goes quiet mid-transfer is dropped
    // A sender that trickles a byte now and then still has to finish within
    // kDeadlineMs plus one second per kMinRate bytes it announced.
    static constexpr int kDeadlineMs = 2 * 60 * 1000;
    static constexpr qint64 kMinRate = 256 * 1024;
    // Only one transfer past its manifest is held at a time (a second one is
    // told busy), so at most kMaxTotal is buffered; the cap only bounds the
    // connections still sending their hello and manifest.
    // Pads keep a connection open while they sync, so leave room for a few.
    static constexpr int kMaxConns = 8;
    // One API request line; a design with many path points is the big one.
    static constexpr qint64 kMaxRequest = 16LL * 1024 * 1024;
    // Lines that are not a request before the connection is cut off.
    static constexpr int kMaxBadLines = 16;

    explicit ProjectReceiver(QObject *parent = nullptr);
    ~ProjectReceiver() override;

    // Listen for transfers (TCP) and discovery (UDP) on every interface.
    bool start(quint16 port = kPort, QString *error = nullptr);
    void stop();
    bool isListening() const;
    quint16 port() const;

    // The user's decision on transfer `id` (from received()): the sender gets
    // "OK <message>" or "ERR <message>" and the connection is closed.
    void answer(int id, bool ok, const QString &message);

    // Answer API request `id` (from request()); the next request on that
    // connection is read once this is sent.
    void respond(int id, const QJsonObject &reply);
    // True while the pad that sent request `id` is still there to hear the answer.
    bool isWaiting(int id) const { return m_apiWaiting.contains(id); }
    // Merged into every discovery answer.
    void setDiscoveryExtra(const QJsonObject &extra) { m_discoveryExtra = extra; }

    // For tests: shorter timeouts than kIdleMs and kDeadlineMs (base part).
    void setTimeouts(int idleMs, int deadlineMs) { m_idleMs = idleMs; m_deadlineMs = deadlineMs; }

signals:
    // A complete project arrived; call answer(id, …) once the user decided.
    // While one is waiting for an answer, further transfers are turned away.
    void received(int id, const c2d::ReceivedProject &project);
    // A pad's request (one JSON object); call respond(id, …). `peer` is
    // the address it came from.
    void request(int id, const QJsonObject &request, const QString &peer);

private:
    struct Conn;
    void onConnection();
    void onData(QTcpSocket *s);
    void onDatagrams();
    void onApiData(QTcpSocket *s);
    void drop(QTcpSocket *s, const QByteArray &reply);

    QTcpServer *m_tcp = nullptr;
    QUdpSocket *m_udp = nullptr;
    QHash<QTcpSocket *, Conn *> m_conns;
    QHash<int, QTcpSocket *> m_waiting;   // complete, waiting for answer()
    QHash<int, QTcpSocket *> m_apiWaiting;   // API request waiting for respond()
    QJsonObject m_discoveryExtra;
    int m_nextId = 1;
    int m_idleMs = kIdleMs;
    int m_deadlineMs = kDeadlineMs;
};

} // namespace c2d
