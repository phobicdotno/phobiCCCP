// Projects sent from Syntexophobic (File > Send to phobiCCCP…): the protocol
// reader on good and hostile input, saving what arrived, and a whole transfer
// over loopback including discovery and the answer the sender waits for.
#include "../src/projectreceiver.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUdpSocket>
#include <cstdio>
#include <functional>

using namespace c2d;

static int g_fail = 0;
static void check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

static bool waitFor(const std::function<bool()> &done, int ms = 5000)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

static QByteArray package(const QJsonArray &files, const QList<QByteArray> &bodies,
                          const QString &name = QStringLiteral("bracket"))
{
    QJsonObject m;
    m.insert(QStringLiteral("name"), name);
    m.insert(QStringLiteral("from"), QStringLiteral("laptop"));
    m.insert(QStringLiteral("app"), QStringLiteral("Syntexophobic 0.7.1"));
    m.insert(QStringLiteral("stock"), QJsonObject{{QStringLiteral("width"), 56},
                                                  {QStringLiteral("height"), 36},
                                                  {QStringLiteral("thickness"), 11}});
    m.insert(QStringLiteral("files"), files);
    QByteArray out = "PHOBICCCP-SEND 1\n" + QJsonDocument(m).toJson(QJsonDocument::Compact) + '\n';
    for (const QByteArray &b : bodies)
        out += b;
    return out;
}

static QJsonObject file(const char *name, const char *role, double size)
{
    return {{QStringLiteral("name"), QLatin1String(name)},
            {QStringLiteral("role"), QLatin1String(role)},
            {QStringLiteral("size"), size}};
}

static QByteArray goodPackage()
{
    QJsonObject stl = file("part.stl", "model", 5);
    stl.insert(QStringLiteral("x"), 3);
    stl.insert(QStringLiteral("y"), 3);
    return package({stl, file("logo.dxf", "vectors", 3), file("bracket.cccp", "project", 0)},
                   {"SOLID", "DXF"});
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // ---- reader: a good package, fed one byte at a time ----
    {
        PackageReader r;
        const QByteArray all = goodPackage();
        bool ok = true;
        for (char c : all)
            ok = ok && r.feed(QByteArray(1, c));
        check(ok && r.state() == PackageReader::Done, "byte-at-a-time package completes");
        const ReceivedProject &p = r.project();
        check(p.name == QLatin1String("bracket") && p.from == QLatin1String("laptop"), "name and sender");
        check(p.stockWidth == 56 && p.stockHeight == 36 && p.stockThickness == 11, "stock size");
        check(p.files.size() == 3 && p.files[0].data == "SOLID" && p.files[1].data == "DXF"
                  && p.files[2].data.isEmpty(), "file bodies split by size (empty one too)");
        check(p.first(QStringLiteral("model")) && p.first(QStringLiteral("model"))->x == 3,
              "model placement");
        check(p.count(QStringLiteral("vectors")) == 1, "one sketch");
    }

    // ---- reader: hostile or broken input ----
    auto fails = [](const QByteArray &bytes) {
        PackageReader r;
        return !r.feed(bytes) && r.state() == PackageReader::Failed && !r.error().isEmpty();
    };
    check(fails("GET / HTTP/1.1\r\n"), "not the protocol");
    check(fails("PHOBICCCP-SEND 1\nnot json\n"), "manifest not JSON");
    check(fails(package({}, {})), "no files");
    check(fails(package({file("../evil.stl", "model", 1)}, {"x"})), "path in a file name");
    check(fails(package({file(".bashrc", "vectors", 1)}, {"x"})), "hidden file name");
    check(fails(package({file("a/b.dxf", "vectors", 1)}, {"x"})), "separator in a file name");
    check(fails(package({file("run.sh", "script", 1)}, {"x"})), "unknown role");
    check(fails(package({file("a.dxf", "vectors", 1), file("A.dxf", "vectors", 1)}, {"x", "y"})),
          "duplicate names");
    check(fails(package({file("a", "vectors", 1), file("a ", "vectors", 1)}, {"x", "y"})),
          "names that are the same once made safe");
    check(fails(package({file("a.stl", "model", -1)}, {})), "negative size");
    check(fails(package({file("a.stl", "model", 1.5)}, {})), "fractional size");
    check(fails(package({file("a.stl", "model", double(PackageReader::kMaxTotal)),
                         file("b.stl", "model", 1)}, {})), "over the size limit");
    check(fails(package({file("a.dxf", "vectors", 1)}, {"xy"})), "more data than announced");
    check(fails("PHOBICCCP-SEND 1\n" + QByteArray(PackageReader::kMaxManifest + 10, 'x')),
          "endless manifest line");
    {
        QJsonArray many;
        for (int i = 0; i <= PackageReader::kMaxFiles; ++i)
            many.append(file(qPrintable(QStringLiteral("f%1.dxf").arg(i)), "vectors", 0));
        check(fails(package(many, {})), "too many files");
    }
    {
        PackageReader r;
        r.feed(package({file("a.dxf", "vectors", 1)}, {"x"}, QStringLiteral("../../etc")));
        check(r.state() == PackageReader::Done && !r.project().name.contains(QLatin1Char('/'))
                  && !r.project().name.startsWith(QLatin1Char('.')), "project name made safe");
    }
    check(safeFileName(QStringLiteral("  ..")) == QLatin1String("project"), "empty name falls back");
    {
        const QString longName = QString(90, QLatin1Char('a')) + QStringLiteral(".svg");
        const QString safe = safeFileName(longName);
        check(safe.size() <= 80 && safe.endsWith(QLatin1String(".svg")), "a long name keeps its extension");
    }

    // ---- saving ----
    QTemporaryDir tmp;
    {
        PackageReader r;
        r.feed(goodPackage());
        QStringList paths;
        QString err;
        const QString a = saveReceived(r.project(), tmp.path(), &paths, &err);
        const QString b = saveReceived(r.project(), tmp.path(), nullptr, &err);
        check(QFileInfo(a).fileName() == QLatin1String("bracket")
                  && QFileInfo(b).fileName() == QLatin1String("bracket-2"),
              "saved into a new folder each time");
        QFile f(paths.value(0));
        check(paths.size() == 3 && f.open(QIODevice::ReadOnly) && f.readAll() == "SOLID",
              "files written");
    }
    // Two files asking for one name (the reader refuses such a manifest, a
    // project put together any other way can still hold them): the second
    // used to be written over the first. It goes beside it as name-2, and
    // the very same file listed twice is kept once.
    {
        auto named = [](const char *name, const char *data) {
            ReceivedFile f;
            f.name = QLatin1String(name);
            f.role = QStringLiteral("vectors");
            f.data = data;
            return f;
        };
        auto read = [](const QString &path) {
            QFile f(path);
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<unreadable>");
        };
        ReceivedProject p;
        p.name = QStringLiteral("twins");
        p.files = {named("a.dxf", "one"), named("a.dxf", "two"), named("a.dxf", "one"),
                   named("A.dxf", "three"), named("plan", "p1"), named("plan", "p2"),
                   named("a-2.dxf", "four"), named("b.dxf", "five")};
        QStringList paths;
        QString err;
        const QString dir = saveReceived(p, tmp.path(), &paths, &err);
        QStringList names;
        for (const QString &path : paths)
            names << QFileInfo(path).fileName();
        check(!dir.isEmpty() && paths.size() == p.files.size(), "twins: saved, one path per file");
        check(names.value(0) == QLatin1String("a.dxf") && names.value(1) == QLatin1String("a-2.dxf"),
              "twins: the second of a name is saved beside the first as name-2");
        check(read(paths.value(0)) == "one" && read(paths.value(1)) == "two",
              "twins: and the first keeps what it held");
        check(paths.value(2) == paths.value(0), "twins: the same file listed twice is one file");
        check(names.value(3).compare(QLatin1String("a-3.dxf"), Qt::CaseInsensitive) == 0
                  && read(paths.value(3)) == "three",
              "twins: a name differing only in case counts as the same name");
        check(names.value(4) == QLatin1String("plan") && names.value(5) == QLatin1String("plan-2")
                  && read(paths.value(4)) == "p1" && read(paths.value(5)) == "p2",
              "twins: a name without an extension gets its number at the end");
        check(names.value(6).compare(names.value(1), Qt::CaseInsensitive) != 0
                  && read(paths.value(6)) == "four" && read(paths.value(1)) == "two",
              "twins: a file really called name-2 does not land on the renamed one");
        check(names.value(7) == QLatin1String("b.dxf") && read(paths.value(7)) == "five",
              "twins: a name of its own is kept as it is");
        bool allIn = true;
        for (const QString &path : paths)
            allIn = allIn && QFileInfo(path).absolutePath() == QFileInfo(dir).absoluteFilePath();
        check(allIn && QDir(dir).entryList(QDir::Files).size() == 7,
              "twins: seven files in the project's folder for eight listed");
    }

    // ---- loopback: discovery, transfer, the user's answer ----
    ProjectReceiver rx;
    QString err;
    if (!rx.start(0, &err)) {
        std::printf("FAIL listen: %s\n", qPrintable(err));
        return 1;
    }
    check(rx.isListening() && rx.port() > 0, "listening");

    {
        QUdpSocket u;
        u.bind(QHostAddress::LocalHost, 0);
        u.writeDatagram(QByteArray("PHOBICCCP-DISCOVER 1"), QHostAddress::LocalHost, rx.port());
        check(waitFor([&] { return u.hasPendingDatagrams(); }), "discovery answered");
        const QJsonObject o = QJsonDocument::fromJson(u.receiveDatagram().data()).object();
        check(o.value(QStringLiteral("app")).toString() == QLatin1String("phobiCCCP")
                  && o.value(QStringLiteral("port")).toInt() == rx.port(),
              "discovery says who and where");
    }

    int gotId = 0;
    ReceivedProject got;
    QObject::connect(&rx, &ProjectReceiver::received, [&](int id, const ReceivedProject &p) {
        gotId = id;
        got = p;
    });
    auto transfer = [&](bool accept, const QByteArray &expect) {
        gotId = 0;
        QTcpSocket s;
        s.connectToHost(QHostAddress::LocalHost, rx.port());
        if (!s.waitForConnected(3000))
            return false;
        s.write(goodPackage());
        if (!waitFor([&] { return gotId != 0; }))
            return false;
        rx.answer(gotId, accept, accept ? QStringLiteral("opened bracket.c2d") : QStringLiteral("declined"));
        if (!waitFor([&] { return s.canReadLine() || s.state() != QAbstractSocket::ConnectedState; }))
            return false;
        return s.readLine() == expect;
    };
    check(transfer(true, "OK opened bracket.c2d\n"), "accepted transfer answered OK");
    check(got.peer == QLatin1String("127.0.0.1") && got.files.size() == 3, "peer and files reported");
    check(transfer(false, "ERR declined\n"), "declined transfer answered ERR");

    {
        QTcpSocket s;
        s.connectToHost(QHostAddress::LocalHost, rx.port());
        s.waitForConnected(3000);
        s.write("GET / HTTP/1.1\r\n\r\n");
        check(waitFor([&] { return s.canReadLine(); }) && s.readLine().startsWith("ERR "),
              "garbage gets ERR and is dropped");
    }

    // One transfer at a time: a second manifest while one is still being sent
    // is told busy, so only one project's bytes are held in memory.
    {
        QTcpSocket a;
        a.connectToHost(QHostAddress::LocalHost, rx.port());
        a.waitForConnected(3000);
        a.write(package({file("big.stl", "model", 1000)}, {"part of it"}));
        a.flush();
        waitFor([] { return false; }, 200);
        QTcpSocket b;
        b.connectToHost(QHostAddress::LocalHost, rx.port());
        b.waitForConnected(3000);
        b.write(goodPackage());
        check(waitFor([&] { return b.canReadLine(); }) && b.readLine().startsWith("ERR phobiCCCP is busy"),
              "second transfer while one is in progress is turned away");
        check(a.state() == QAbstractSocket::ConnectedState, "the first one carries on");
    }

    // The pad app: "PHOBICCCP-API 1", then one JSON request per line, each
    // answered before the next is read, on the same port as transfers.
    {
        rx.setDiscoveryExtra(QJsonObject{{QStringLiteral("id"), QStringLiteral("abc")},
                                         {QStringLiteral("port"), 1}});
        QUdpSocket u;
        u.bind(QHostAddress::LocalHost, 0);
        u.writeDatagram(QByteArray("PHOBICCCP-DISCOVER 1"), QHostAddress::LocalHost, rx.port());
        waitFor([&] { return u.hasPendingDatagrams(); });
        const QJsonObject o = QJsonDocument::fromJson(u.receiveDatagram().data()).object();
        check(o.value(QStringLiteral("id")).toString() == QLatin1String("abc")
                  && o.value(QStringLiteral("port")).toInt() == rx.port(),
              "discovery carries the extra keys, never over its own");

        QList<QPair<int, QJsonObject>> asked;
        // Disconnected at the end of this block: later requests must not reach
        // a list that is gone.
        const auto asking = QObject::connect(&rx, &ProjectReceiver::request,
                         [&](int id, const QJsonObject &r, const QString &) { asked.append({id, r}); });
        QTcpSocket s;
        s.connectToHost(QHostAddress::LocalHost, rx.port());
        s.waitForConnected(3000);
        // Split hello, two requests in one write, then garbage.
        s.write("PHOBICCCP-");
        s.flush();
        waitFor([] { return false; }, 100);
        s.write("API 1\n{\"op\":\"info\"}\n{\"op\":\"tools\"}\n");
        check(waitFor([&] { return asked.size() == 1; }) &&
                  asked.first().second.value(QStringLiteral("op")).toString() == QLatin1String("info"),
              "API request arrives");
        waitFor([] { return false; }, 200);
        check(asked.size() == 1, "the next request waits for the answer");
        rx.respond(asked.first().first, QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("n"), 1}});
        check(waitFor([&] { return s.canReadLine(); })
                  && QJsonDocument::fromJson(s.readLine()).object().value(QStringLiteral("n")).toInt() == 1,
              "answer is one JSON line");
        check(waitFor([&] { return asked.size() == 2; })
                  && asked.at(1).second.value(QStringLiteral("op")).toString() == QLatin1String("tools"),
              "then the queued request is read");
        rx.respond(asked.at(1).first, QJsonObject{{QStringLiteral("ok"), true}});
        waitFor([&] { return s.canReadLine(); });
        s.readLine();
        s.write("not json\n");
        check(waitFor([&] { return s.canReadLine(); })
                  && !QJsonDocument::fromJson(s.readLine()).object().value(QStringLiteral("ok")).toBool(true)
                  && s.state() == QAbstractSocket::ConnectedState,
              "a bad line is answered with an error and the connection stays");
        rx.respond(9999, QJsonObject{});   // unknown id: nothing happens
        check(!rx.isWaiting(asked.at(1).first), "an answered request is no longer waiting");
        QObject::disconnect(asking);
    }

    // A flood of empty lines: answered in a loop (no deep recursion), and cut
    // off after a few, without taking the receiver down.
    {
        QTcpSocket s;
        s.connectToHost(QHostAddress::LocalHost, rx.port());
        s.waitForConnected(3000);
        s.write("PHOBICCCP-API 1\n" + QByteArray(200000, '\n'));
        check(waitFor([&] { return s.state() != QAbstractSocket::ConnectedState; }),
              "a flood of empty lines gets the connection cut");
    }
    // A request waiting on the user, with more streamed in behind it: the
    // connection is cut instead of buffering without end.
    {
        QList<int> asked;
        auto c = QObject::connect(&rx, &ProjectReceiver::request,
                                  [&](int id, const QJsonObject &, const QString &) { asked.append(id); });
        QTcpSocket s;
        s.connectToHost(QHostAddress::LocalHost, rx.port());
        s.waitForConnected(3000);
        s.write("PHOBICCCP-API 1\n{\"op\":\"send\"}\n");
        waitFor([&] { return !asked.isEmpty(); });
        check(rx.isWaiting(asked.first()), "the send waits for an answer");
        const QByteArray chunk(1024 * 1024, 'x');
        for (int i = 0; i < 40 && s.state() == QAbstractSocket::ConnectedState; ++i) {
            s.write(chunk);
            waitFor([&] { return s.bytesToWrite() == 0; }, 2000);
        }
        check(waitFor([&] { return s.state() != QAbstractSocket::ConnectedState; }),
              "streaming behind a waiting request gets the connection cut");
        check(!rx.isWaiting(asked.first()), "and its request no longer waits");
        QObject::disconnect(c);
    }

    // An absolute deadline: a sender that keeps the idle timer happy with a
    // byte now and then is still cut off.
    {
        ProjectReceiver slow;
        slow.setTimeouts(400, 600);
        slow.start(0);
        QTcpSocket s;
        s.connectToHost(QHostAddress::LocalHost, slow.port());
        s.waitForConnected(3000);
        const QByteArray all = goodPackage();
        int sent = 0;
        QElapsedTimer t;
        t.start();
        while (!s.canReadLine() && s.state() == QAbstractSocket::ConnectedState && t.elapsed() < 5000) {
            if (sent < all.size() - 1)
                s.write(all.mid(sent++, 1));
            waitFor([] { return false; }, 100);
        }
        check(s.readLine() == "ERR timed out\n" && t.elapsed() < 3000, "trickling sender hits the deadline");
    }

    // The pad's side: a request has a deadline of its own from its first
    // byte. One trickled in a byte at a time kept the idle timer happy and
    // held the connection (one of a few) for as long as it liked.
    {
        ProjectReceiver slow;
        slow.setTimeouts(400, 600);
        slow.start(0);
        QTcpSocket s;
        s.connectToHost(QHostAddress::LocalHost, slow.port());
        s.waitForConnected(3000);
        s.write("PHOBICCCP-API 1\n");
        const QByteArray req = "{\"op\":\"info\",\"pad\":\"" + QByteArray(100, 'x');   // never finished
        int sent = 0;
        QElapsedTimer t;
        t.start();
        while (!s.canReadLine() && s.state() == QAbstractSocket::ConnectedState && t.elapsed() < 3000) {
            if (sent < req.size())
                s.write(req.mid(sent++, 1));
            waitFor([] { return false; }, 100);
        }
        check(s.readLine() == "{\"ok\":false,\"error\":\"timed out\"}\n" && t.elapsed() < 2500,
              "a trickling pad request hits its deadline, and is told so in the pad's own format");
        check(waitFor([&] { return s.state() != QAbstractSocket::ConnectedState; }, 2000),
              "and its connection is closed");
    }
    // Only a request still arriving is on that clock: not a pad waiting for
    // its answer, not one that is quiet between requests, and not the start
    // of a next request queued behind one that waits (its time starts when
    // it is read).
    {
        ProjectReceiver pad;
        pad.setTimeouts(5000, 300);
        pad.start(0);
        QList<int> asked;
        QObject::connect(&pad, &ProjectReceiver::request,
                         [&](int id, const QJsonObject &, const QString &) { asked.append(id); });
        auto pause = [](int ms) { waitFor([] { return false; }, ms); };
        auto answered = [&](QTcpSocket &s) {
            return waitFor([&] { return s.canReadLine(); }, 2000)
                   && QJsonDocument::fromJson(s.readLine()).object().value(QStringLiteral("ok")).toBool();
        };
        QTcpSocket s;
        s.connectToHost(QHostAddress::LocalHost, pad.port());
        s.waitForConnected(3000);
        s.write("PHOBICCCP-API 1\n{\"op\":");
        s.flush();
        pause(100);
        s.write("\"info\"}\n");
        check(waitFor([&] { return asked.size() == 1; }, 2000), "deadline: a request in two parts arrives");
        pause(500);                          // the user takes longer than the deadline to answer
        check(asked.size() == 1 && pad.isWaiting(asked.value(0)) && !s.canReadLine()
                  && s.state() == QAbstractSocket::ConnectedState,
              "deadline: a request waiting for its answer is not timed");
        pad.respond(asked.value(0), QJsonObject{{QStringLiteral("ok"), true}});
        check(answered(s), "deadline: answered");
        pause(500);
        check(!s.canReadLine() && s.state() == QAbstractSocket::ConnectedState,
              "deadline: a pad quiet between requests is not timed");
        s.write("{\"op\":\"tools\"}\n{\"op\":");
        check(waitFor([&] { return asked.size() == 2; }, 2000), "deadline: the next request arrives");
        pause(500);
        check(pad.isWaiting(asked.value(1)) && !s.canReadLine()
                  && s.state() == QAbstractSocket::ConnectedState,
              "deadline: half a request behind a waiting one is not timed yet");
        QElapsedTimer t;
        t.start();
        pad.respond(asked.value(1), QJsonObject{{QStringLiteral("ok"), true}});
        check(answered(s), "deadline: the waiting one is answered");
        check(waitFor([&] { return s.canReadLine(); }, 2000)
                  && s.readLine() == "{\"ok\":false,\"error\":\"timed out\"}\n" && t.elapsed() >= 250,
              "deadline: then the half request has its own time, and runs out of it");
    }

    std::printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
