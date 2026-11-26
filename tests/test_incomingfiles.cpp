// Files that reach the pad from other apps (src/incomingfiles.h): what kind
// a file is by its name or its first bytes, the name it is kept under, where
// it goes, and the designs the earlier (Flutter) pad app left behind turned
// into .c2d files without touching them. The Android calls around these
// (src/androidfiles.cpp) need a tablet; the rules do not.
#include "../src/c2ddocument.h"
#include "../src/incomingfiles.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using namespace c2d;
using incoming::Kind;

class TestIncomingFiles : public QObject
{
    Q_OBJECT

    static void write(const QString &path, const QByteArray &data)
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QCOMPARE(f.write(data), data.size());
    }

    static QByteArray read(const QString &path)
    {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }

    static QJsonObject fixture()
    {
        QFile f(QStringLiteral(PHOBICCCP_SOURCE_DIR "/tests/data/pad-design.json"));
        if (!f.open(QIODevice::ReadOnly))
            return {};
        return QJsonDocument::fromJson(f.readAll()).object();
    }

    static QByteArray sqliteHead() { return QByteArray("SQLite format 3\0", 16) + QByteArray(84, '\0'); }

private slots:
    void kindByName()
    {
        QCOMPARE(incoming::kindOf("Sign.c2d"), Kind::Design);
        QCOMPARE(incoming::kindOf("SIGN.C2D"), Kind::Design);
        QCOMPARE(incoming::kindOf("/x/y/part.cccp"), Kind::Cccp);
        QCOMPARE(incoming::kindOf("logo.svg"), Kind::Vectors);
        QCOMPARE(incoming::kindOf("plate.DXF"), Kind::Vectors);
        QCOMPARE(incoming::kindOf("cut.plt"), Kind::Vectors);
        QCOMPARE(incoming::kindOf("cut.hpgl"), Kind::Vectors);
        // G-code: the pad has nothing to do with it, so it is not offered.
        QCOMPARE(incoming::kindOf("job.nc"), Kind::Unsupported);
        QCOMPARE(incoming::kindOf("job.gcode"), Kind::Unsupported);
        QCOMPARE(incoming::kindOf("photo.jpg"), Kind::Unsupported);
        QCOMPARE(incoming::kindOf("noextension"), Kind::Unsupported);
        // On a computer PDF goes through the external converters; on the pad it does not.
        QCOMPARE(incoming::kindOf("drawing.pdf"), Kind::Vectors);
    }

    void kindByContents()
    {
        QCOMPARE(incoming::sniffSuffix(sqliteHead()), QStringLiteral("c2d"));
        QCOMPARE(incoming::sniffSuffix("  0\r\nSECTION\r\n  2\r\nHEADER\r\n"), QStringLiteral("dxf"));
        QCOMPARE(incoming::sniffSuffix("0\nSECTION\n"), QStringLiteral("dxf"));
        QCOMPARE(incoming::sniffSuffix("AutoCAD Binary DXF\r\n\x1a"), QStringLiteral("dxf"));
        QCOMPARE(incoming::sniffSuffix("\xef\xbb\xbf<?xml version=\"1.0\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\">"),
                 QStringLiteral("svg"));
        QCOMPARE(incoming::sniffSuffix("PK\x03\x04"), QString());
        QCOMPARE(incoming::sniffSuffix("G21\nG90\nG0 Z5\n"), QString());
        QCOMPARE(incoming::sniffSuffix({}), QString());
    }

    void names()
    {
        // Only the last part of a path; nothing a file system refuses; never hidden or empty.
        QCOMPARE(incoming::cleanName("../../etc/passwd"), QStringLiteral("passwd"));
        QCOMPARE(incoming::cleanName("C:\\Users\\me\\sign.c2d"), QStringLiteral("sign.c2d"));
        QCOMPARE(incoming::cleanName("a:b?<c>.svg"), QStringLiteral("a_b__c_.svg"));
        QCOMPARE(incoming::cleanName(".hidden.svg"), QStringLiteral("hidden.svg"));
        QCOMPARE(incoming::cleanName(""), QStringLiteral("file"));
        QCOMPARE(incoming::cleanName("  ..  "), QStringLiteral("file"));
        QCOMPARE(incoming::cleanName(QStringLiteral("Skilt for d\u00f8ra.c2d")), QStringLiteral("Skilt for d\u00f8ra.c2d"));
        // Drive and mail apps often drop the extension: the contents say it.
        QCOMPARE(incoming::cleanName("Door sign", sqliteHead()), QStringLiteral("Door sign.c2d"));
        QCOMPARE(incoming::cleanName("logo", "<svg/>"), QStringLiteral("logo.svg"));
        QCOMPARE(incoming::cleanName("notes", "hello"), QStringLiteral("notes"));
        // A known extension is kept whatever the contents.
        QCOMPARE(incoming::cleanName("plate.dxf", sqliteHead()), QStringLiteral("plate.dxf"));
        // A very long name is cut, its extension kept.
        const QString longName = incoming::cleanName(QString(300, QLatin1Char('x')) + QStringLiteral(".svg"));
        QVERIFY(longName.size() <= 120);
        QVERIFY(longName.endsWith(QStringLiteral(".svg")));
    }

    void uniqueNames()
    {
        QTemporaryDir tmp;
        QCOMPARE(incoming::uniquePath(tmp.path(), "a.c2d"), tmp.filePath("a.c2d"));
        write(tmp.filePath("a.c2d"), "1");
        QCOMPARE(incoming::uniquePath(tmp.path(), "a.c2d"), tmp.filePath("a (2).c2d"));
        write(tmp.filePath("a (2).c2d"), "2");
        QCOMPARE(incoming::uniquePath(tmp.path(), "a.c2d"), tmp.filePath("a (3).c2d"));
        write(tmp.filePath("plain"), "");
        QCOMPARE(incoming::uniquePath(tmp.path(), "plain"), tmp.filePath("plain (2)"));
    }

    void settleDesign()
    {
        QTemporaryDir tmp;
        const QString cache = tmp.filePath("cache/incoming/1");
        const QString designs = tmp.filePath("Documents/phobiCCCP");
        QDir().mkpath(cache);
        const QByteArray design = sqliteHead() + "one";

        // A design arriving without an extension lands with the others as a .c2d.
        write(cache + "/Door sign", design);
        QString err;
        const QString first = incoming::settle(cache + "/Door sign", "Door sign", designs, &err);
        QCOMPARE(first, designs + "/Door sign.c2d");
        QCOMPARE(read(first), design);
        QVERIFY(!QFile::exists(cache + "/Door sign"));

        // The same file picked again: the copy already there, not a second one.
        write(cache + "/Door sign.c2d", design);
        QCOMPARE(incoming::settle(cache + "/Door sign.c2d", "Door sign.c2d", designs, &err), first);
        QVERIFY(!QFile::exists(cache + "/Door sign.c2d"));
        QCOMPARE(QDir(designs).entryList(QDir::Files).size(), 1);

        // Another file of that name never replaces the one there.
        write(cache + "/Door sign.c2d", sqliteHead() + "two");
        const QString second = incoming::settle(cache + "/Door sign.c2d", "Door sign.c2d", designs, &err);
        QCOMPARE(second, designs + "/Door sign (2).c2d");
        QCOMPARE(read(first), design);

        // A file that is not there.
        err.clear();
        QVERIFY(incoming::settle(cache + "/gone.c2d", "gone.c2d", designs, &err).isEmpty());
        QVERIFY(!err.isEmpty());
    }

    void settleOthers()
    {
        QTemporaryDir tmp;
        const QString cache = tmp.filePath("cache/incoming/2");
        const QString designs = tmp.filePath("Documents/phobiCCCP");
        QDir().mkpath(cache);
        // A drawing stays in the cache, renamed so the importer knows it.
        write(cache + "/logo", "<?xml version=\"1.0\"?><svg xmlns=\"http://www.w3.org/2000/svg\"/>");
        QString err;
        const QString svg = incoming::settle(cache + "/logo", "logo", designs, &err);
        QCOMPARE(svg, cache + "/logo.svg");
        QCOMPARE(incoming::kindOf(svg), Kind::Vectors);
        QVERIFY(!QDir(designs).exists());
        // A name the other app gave with folders in it.
        write(cache + "/x", "0\nSECTION\n");
        QCOMPARE(incoming::settle(cache + "/x", "../../plate.dxf", designs, &err), cache + "/plate.dxf");
        // Something else keeps its name and is left to the window to refuse.
        write(cache + "/job.nc", "G0 X0\n");
        const QString nc = incoming::settle(cache + "/job.nc", "job.nc", designs, &err);
        QCOMPARE(nc, cache + "/job.nc");
        QCOMPARE(incoming::kindOf(nc), Kind::Unsupported);
    }

    // The designs the Flutter pad app kept (one <id>.json each in its
    // application support folder), made into .c2d files.
    void adoptOldPadDesigns()
    {
        const QJsonObject good = fixture();
        QVERIFY(!good.isEmpty());
        QTemporaryDir tmp;
        const QString from = tmp.filePath("files/designs");
        const QString to = tmp.filePath("Documents/phobiCCCP");
        QDir().mkpath(from);
        const QString goodFile = from + "/" + good.value("id").toString() + ".json";
        write(goodFile, QJsonDocument(good).toJson());

        QJsonObject slashy = good;
        slashy["id"] = "slashy-1";
        slashy["name"] = "Shelf A/B";
        write(from + "/slashy-1.json", QJsonDocument(slashy).toJson());

        QJsonObject deleted = good;
        deleted["id"] = "gone-1";
        deleted["deleted"] = true;
        write(from + "/gone-1.json", QJsonDocument(deleted).toJson());

        QJsonObject broken = good;
        broken["id"] = "broken-1";
        broken["name"] = "Broken";
        broken["stock"] = QJsonObject{{"width", -5}, {"height", 0}, {"thickness", 0}};
        write(from + "/broken-1.json", QJsonDocument(broken).toJson());

        write(from + "/junk.json", "not json at all");
        write(from + "/half.json.tmp", QJsonDocument(good).toJson());   // a write the old app never finished
        write(from + "/.replica", "abc");

        const QByteArray original = read(goodFile);
        const QStringList before = QDir(from).entryList(QDir::Files | QDir::Hidden);

        QCOMPARE(incoming::padDesignsWaiting(from, {}), 3);   // good, slashy, broken
        const incoming::Adopted r = incoming::adoptPadDesigns(from, to);
        QCOMPARE(r.made.size(), 2);
        QCOMPARE(r.ids.size(), 2);
        QCOMPARE(r.failed.size(), 1);
        QVERIFY(r.failed.first().startsWith("Broken: "));
        QCOMPARE(r.skipped, 2);   // the deleted design and the junk
        QVERIFY(r.made.contains(to + "/Door sign.c2d"));
        QVERIFY(r.made.contains(to + "/Shelf A-B.c2d"));
        QVERIFY(r.ids.contains(good.value("id").toString()));
        QVERIFY(!QFile::exists(to + "/Broken.c2d"));

        // An ordinary design: its shapes and its four toolpaths.
        Document doc;
        QString err;
        QVERIFY2(doc.load(to + "/Door sign.c2d", &err), qPrintable(err));
        QCOMPARE(doc.toolpaths().size(), 4);
        QVERIFY(doc.elements().size() >= 5);
        QCOMPARE(doc.boardWidth(), 300.0);

        // The old app's files are as they were.
        QCOMPARE(read(goodFile), original);
        QCOMPARE(QDir(from).entryList(QDir::Files | QDir::Hidden), before);

        // Asked again with what was brought in: nothing more, nothing twice.
        const QSet<QString> done(r.ids.begin(), r.ids.end());
        QCOMPARE(incoming::padDesignsWaiting(from, done), 1);   // only the broken one
        const incoming::Adopted again = incoming::adoptPadDesigns(from, to, done);
        QVERIFY(again.made.isEmpty());
        QCOMPARE(QDir(to).entryList({"*.c2d"}, QDir::Files).size(), 2);

        // Brought in once more without that list: new names, never over the first.
        const incoming::Adopted twice = incoming::adoptPadDesigns(from, to);
        QVERIFY(twice.made.contains(to + "/Door sign (2).c2d"));

        // No folder at all (a pad that never ran the old app).
        QCOMPARE(incoming::padDesignsWaiting(tmp.filePath("nothing"), {}), 0);
        QVERIFY(incoming::adoptPadDesigns(tmp.filePath("nothing"), to).made.isEmpty());
    }
};

QTEST_MAIN(TestIncomingFiles)
#include "test_incomingfiles.moc"
