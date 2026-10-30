// The tool library is the repo's toollib/ folder: one JSON per tool named by a
// 6 hex id, built in and overlaid by a synced copy. ToolSync fetches only the
// files whose sha1 differs and refuses anything off-pattern or altered.
#include "../src/toollibrary.h"
#include "../src/toolsync.h"
#include "../src/stocksync.h"
#include <QSettings>
#include <QTimer>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <cstdio>

static int g_fail = 0;
static void check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

static void put(const QString &path, const QByteArray &b)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(b);
}

static QByteArray sha1(const QByteArray &b)
{
    return QCryptographicHash::hash(b, QCryptographicHash::Sha1).toHex();
}

// A source folder like the repo's, with index.json made from what is in it.
static void writeIndex(const QString &dir, const QStringList &extraBad = {})
{
    QJsonObject files;
    const QMap<QString, QByteArray> t = c2d::ToolLibrary::readTree(dir);
    for (auto it = t.constBegin(); it != t.constEnd(); ++it)
        if (it.key() != QLatin1String("index.json"))
            files.insert(it.key(), QString::fromLatin1(sha1(it.value())));
    for (const QString &b : extraBad)
        files.insert(b, QString(40, QLatin1Char('a')));
    QJsonObject root;
    root.insert(QStringLiteral("files"), files);
    put(dir + QStringLiteral("/index.json"), QJsonDocument(root).toJson());
}

static c2d::ToolSync::Result runSync(const QString &srcDir, const QString &cache)
{
    c2d::ToolSync s;
    QEventLoop loop;
    int updated = -1, failed = -1;
    QObject::connect(&s, &c2d::ToolSync::finished, &loop, [&](int u, int f, const QString &) {
        updated = u;
        failed = f;
        loop.quit();
    });
    s.start(QStringLiteral("file://") + srcDir, cache);
    loop.exec();
    return {updated, failed};
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QTemporaryDir tmp;
    const QString cache = tmp.path() + QStringLiteral("/cache");
    qputenv("PHOBIC_TOOLLIB_CACHE", cache.toLocal8Bit());

    c2d::ToolLibrary lib;
    QString err;
    check(lib.loadDefault(&err), "built-in library loads");
    QSet<QString> ids, numbers;
    bool hexOk = true;
    static const QRegularExpression hex(QStringLiteral("^[0-9a-f]{6}$"));
    for (const c2d::LibraryTool &t : lib.tools()) {
        hexOk = hexOk && hex.match(t.id).hasMatch();
        ids.insert(t.id);
        numbers.insert(QString::number(t.number));
    }
    check(lib.tools().size() >= 23, "all tools incl. #1203 are there");
    check(hexOk, "every tool has a 6 hex id");
    check(ids.size() == lib.tools().size(), "ids are unique");
    check(numbers.size() == lib.tools().size(), "tool numbers are unique");
    check(lib.byNumber(1203) && lib.byNumber(1203)->diameter == 1.5, "#1203 is the 1.5 mm bit");
    check(lib.source() == QLatin1String("built-in"), "no synced copy yet: source is built-in");
    for (const QString &k : {"endmill", "ball", "vbit", "engraver", "roundover"})
        check(!c2d::ToolLibrary::kindImage(k).isNull(), qPrintable(QStringLiteral("sketch for ") + k));
    check(c2d::ToolLibrary::kindImage(QStringLiteral("../x")).isNull(), "kind name is not a path");

    // The qrc lists exactly what is in the folder (make_index.py not run = fail).
    const QMap<QString, QByteArray> repo = c2d::ToolLibrary::readTree(QStringLiteral(TOOLLIB_DIR));
    const QMap<QString, QByteArray> built = c2d::ToolLibrary::readTree(QStringLiteral(":/data/toollib"));
    QSet<QString> want;
    for (auto it = repo.constBegin(); it != repo.constEnd(); ++it)
        if (it.key().endsWith(QLatin1String(".json")) || it.key().endsWith(QLatin1String(".png")))
            want.insert(it.key());
    check(QSet<QString>(built.keyBegin(), built.keyEnd()) == want, "qrc matches the toollib folder");
    bool shaOk = true;
    const QJsonObject idx = QJsonDocument::fromJson(repo.value(QStringLiteral("index.json"))).object()
                                .value(QStringLiteral("files")).toObject();
    for (auto it = idx.constBegin(); it != idx.constEnd(); ++it)
        shaOk = shaOk && repo.contains(it.key()) && QString::fromLatin1(sha1(repo.value(it.key()))) == it.value().toString();
    check(shaOk && idx.size() == want.size() - 1, "index.json is current (run toollib/make_index.py)");

    // Overlay: a file in the synced copy replaces the built-in one, a new id adds a tool.
    {
        const c2d::LibraryTool *one = lib.byNumber(1203);
        QJsonObject o = QJsonDocument::fromJson(repo.value(QStringLiteral("endmill/") + one->id + ".json")).object();
        o.insert(QStringLiteral("notes"), QStringLiteral("edited here"));
        put(cache + "/endmill/" + one->id + ".json", QJsonDocument(o).toJson());
        QJsonObject n = o;
        n.insert(QStringLiteral("id"), QStringLiteral("abcdef"));
        n.insert(QStringLiteral("number"), 9999);
        put(cache + "/endmill/abcdef.json", QJsonDocument(n).toJson());
        c2d::ToolLibrary l2;
        check(l2.loadDefault() && l2.byNumber(9999) && l2.byNumber(1203)->notes == QLatin1String("edited here"),
              "synced copy overlays and adds");
        check(l2.source() == QLatin1String("built-in + synced"), "source says synced");
        QDir(cache).removeRecursively();
    }

    // Sync against a local folder standing in for GitHub.
    const QString src = tmp.path() + QStringLiteral("/src");
    for (auto it = repo.constBegin(); it != repo.constEnd(); ++it)
        if (want.contains(it.key()))
            put(src + "/" + it.key(), it.value());
    writeIndex(src);
    auto r = runSync(src, cache);
    check(r.updated == 0 && r.failed == 0, "same as built-in: nothing to fetch");
    check(!QDir(cache).exists() || QDir(cache).isEmpty(), "and nothing written");

    const QString id = lib.byNumber(1203)->id;
    QJsonObject o = QJsonDocument::fromJson(repo.value("endmill/" + id + ".json")).object();
    o.insert(QStringLiteral("notes"), QStringLiteral("new note"));
    put(src + "/endmill/" + id + ".json", QJsonDocument(o).toJson());
    o.insert(QStringLiteral("id"), QStringLiteral("0a0b0c"));
    o.insert(QStringLiteral("number"), 4242);
    put(src + "/endmill/0a0b0c.json", QJsonDocument(o).toJson());
    writeIndex(src, {QStringLiteral("../evil.json"), QStringLiteral("endmill/notahex.json")});
    r = runSync(src, cache);
    check(r.updated == 2, "one changed and one new tool fetched");
    check(r.failed == 2, "paths off the pattern are refused");
    check(!QFileInfo::exists(tmp.path() + "/evil.json") && !QFileInfo::exists(cache + "/../evil.json"),
          "nothing written outside the cache");
    c2d::ToolLibrary l3;
    check(l3.loadDefault() && l3.byNumber(4242) && l3.byNumber(1203)->notes == QLatin1String("new note"),
          "library sees the synced tools");
    r = runSync(src, cache);
    check(r.updated == 0 && r.failed == 2, "second run: up to date");

    // A file that does not match its sha1 is dropped.
    put(src + "/endmill/0a0b0c.json", QJsonDocument(o).toJson(QJsonDocument::Compact));   // differs from the index
    QFile ix(src + "/index.json");
    ix.open(QIODevice::ReadOnly);
    QJsonObject cur = QJsonDocument::fromJson(ix.readAll()).object();
    ix.close();
    QJsonObject cf = cur.value("files").toObject();
    cf.insert(QStringLiteral("endmill/0a0b0c.json"), QString(40, QLatin1Char('b')));
    cur.insert(QStringLiteral("files"), cf);
    put(src + "/index.json", QJsonDocument(cur).toJson());
    QDir(cache).removeRecursively();
    r = runSync(src, cache);
    check(!QFileInfo::exists(cache + "/endmill/0a0b0c.json"), "a file that does not match its sha1 is not kept");

    check(c2d::ToolSync::validPath(QStringLiteral("endmill/aa9b3a.json")) && c2d::ToolSync::validPath(QStringLiteral("vbit/kind.png"))
              && c2d::ToolSync::validPath(QStringLiteral("materials.json"))
              && !c2d::ToolSync::validPath(QStringLiteral("../a.json")) && !c2d::ToolSync::validPath(QStringLiteral("a/b/c.json")),
          "validPath");

    // Two copies of the app talking over loopback, each with its own settings.
    {
        const QString a = tmp.path() + "/a.ini", b = tmp.path() + "/b.ini";
        const QHostAddress lo(QHostAddress::LocalHost);
        c2d::StockSync A(a, 47301, lo, 47302), B(b, 47302, lo, 47301);
        check(A.start() && B.start(), "both sockets bind");
        {
            QSettings sa(a, QSettings::IniFormat);
            sa.setValue("toolStock/201/have", 7);
            sa.setValue("toolStock/201/broken", 2);
            sa.setValue("toolStock/201/ts", 1000);
            sa.setValue("tools/collections", QByteArray("{\"collections\":[{\"name\":\"My Tools\",\"tools\":[201]}]}"));
            sa.setValue("tools/collectionsTs", 2000);
            QSettings sb(b, QSettings::IniFormat);
            sb.setValue("toolStock/102/have", 3);
            sb.setValue("toolStock/102/ts", 1500);
        }
        QEventLoop loop;
        QObject::connect(&B, &c2d::StockSync::changed, &loop, &QEventLoop::quit);
        QTimer::singleShot(3000, &loop, &QEventLoop::quit);
        A.announce();
        loop.exec();
        QSettings sb(b, QSettings::IniFormat);
        check(sb.value("toolStock/201/have").toInt() == 7 && sb.value("toolStock/201/broken").toInt() == 2,
              "B took A's counts over the wire");
        check(sb.value("tools/collections").toByteArray().contains("My Tools"), "and A's collections");
        check(sb.value("toolStock/102/have").toInt() == 3, "B's own bit is untouched");
        // Older numbers lose, newer win, junk is ignored.
        check(!B.merge(A.payload()), "same data again: nothing changed");
        QSettings(b, QSettings::IniFormat).setValue("toolStock/201/ts", 5000);
        QSettings(b, QSettings::IniFormat).setValue("toolStock/201/have", 9);
        check(!B.merge(A.payload()) && QSettings(b, QSettings::IniFormat).value("toolStock/201/have").toInt() == 9,
              "older numbers do not overwrite newer ones");
        check(!B.merge("not json") && !B.merge("{\"app\":\"phobiCCCP-stock\",\"v\":1,\"stock\":{\"201\":{\"have\":99999,\"broken\":0,\"ts\":9999999999999}}}"),
              "junk and out-of-range numbers are ignored");
    }
    return g_fail ? 1 : 0;
}
