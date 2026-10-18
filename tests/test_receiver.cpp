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

    std::printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
