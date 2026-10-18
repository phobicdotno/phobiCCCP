// Headless check for File -> New: a blank container must load, take an
// element, save and reload with the element and stock size intact.
#include "../src/c2ddocument.h"
#include "../src/element.h"
#include "../src/recovery.h"

#include <QCoreApplication>
#include <QFile>
#include <QDateTime>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <cstdio>

static int g_fail = 0;
static void check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    // Optional argv[1]: where to write the blank, kept for further manual checks.
    const QString blank = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                   : dir.filePath(QStringLiteral("new.c2d"));
    const QString saved = dir.filePath(QStringLiteral("saved.c2d"));
    QString err;

    check(c2d::Document::createBlank(blank, 300, 200, 19, &err), "createBlank");
    c2d::Document doc;
    check(doc.load(blank, &err), "load blank");
    check(doc.elements().isEmpty(), "blank has no elements");
    check(doc.params().value(QStringLiteral("width")) == QLatin1String("300")
              && doc.params().value(QStringLiteral("height")) == QLatin1String("200"),
          "stock size stored");

    doc.elementsRef().append(
        c2d::Element::makeCircle(QPointF(150, 100), 25, doc.defaultLayer()));
    check(doc.save(saved, &err), "save with an element");

    c2d::Document back;
    check(back.load(saved, &err), "reload saved");
    check(back.elements().size() == 1, "element survives round trip");

    // An element row this build cannot decode must survive a save: save()
    // used to delete every element row and rewrite only what it had read.
    {
        back.elementsRef().append(
            c2d::Element::makeCircle(QPointF(50, 50), 10, back.defaultLayer()));
        check(back.save(saved, &err), "save with two elements");
        auto count = [&](const QString &path, const char *conn) {
            int n = -1;
            {
                QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                            QLatin1String(conn));
                db.setDatabaseName(path);
                if (db.open()) {
                    QSqlQuery q(db);
                    if (q.exec(QStringLiteral("SELECT count(*) FROM items WHERE type='element'")) && q.next())
                        n = q.value(0).toInt();
                    q.exec(QStringLiteral("UPDATE items SET data = x'deadbeef' WHERE id = "
                                          "(SELECT min(id) FROM items WHERE type='element')"));
                }
                db.close();
            }
            QSqlDatabase::removeDatabase(QLatin1String(conn));
            return n;
        };
        check(count(saved, "corrupt") == 2, "two element rows written");   // and corrupts one
        c2d::Document bad;
        check(bad.load(saved, &err) && bad.elements().size() == 1, "the undecodable element is skipped");
        const QString again = dir.filePath(QStringLiteral("again.c2d"));
        check(bad.save(again, &err), "save with an undecodable element");
        check(count(again, "recount") == 2, "the undecodable element row survives the save");
    }
    // Saving over an existing file keeps its permissions, and saving
    // through a symlink updates the file it points at, not the link.
    {
        const QString target = dir.filePath(QStringLiteral("shared.c2d"));
        check(doc.save(target, &err), "save the shared file");
        const QFileDevice::Permissions perms = QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                               | QFileDevice::ReadGroup | QFileDevice::WriteGroup;
        QFile::setPermissions(target, perms);
        check(doc.save(target, &err), "save over it");
        check((QFile::permissions(target) & 0x7077) == (perms & 0x7077),
              "the saved file keeps the permissions it had");

        const QString link = dir.filePath(QStringLiteral("link.c2d"));
        check(QFile::link(target, link), "make a symlink to it");
        doc.elementsRef().append(
            c2d::Element::makeCircle(QPointF(20, 20), 5, doc.defaultLayer()));
        check(doc.save(link, &err), "save through the symlink");
        check(QFileInfo(link).isSymLink(), "the symlink is still a symlink");
        c2d::Document viaTarget;
        check(viaTarget.load(target, &err) && viaTarget.elements().size() == doc.elements().size(),
              "the file it points at was updated");
    }
    // Crash recovery: a copy beside, never over, the user's file; not offered
    // while its session runs; offered once that session is gone; deleted
    // when the file was saved after it, or on discard().
    {
        const QString recDir = dir.filePath(QStringLiteral("recovery"));
        const QString source = dir.filePath(QStringLiteral("shared.c2d"));
        auto touch = [&](int secs) {
            QFile f(source);
            f.open(QIODevice::ReadWrite);
            f.setFileTime(QDateTime::currentDateTime().addSecs(secs), QFileDevice::FileModificationTime);
        };
        touch(-3600);
        QString copy;
        {
            c2d::Recovery live(recDir);
            copy = live.write(doc, source, false, &err);
            check(!copy.isEmpty() && QFile::exists(copy) && copy.startsWith(recDir),
                  "recovery copy written in its own folder");
            check(doc.filePath() == dir.filePath(QStringLiteral("link.c2d")),
                  "writing it does not retarget the document");
            check(c2d::Recovery(recDir).pending().isEmpty(), "a live session's copy is not offered");
        }   // the session ends without discard(): as after a crash
        const QVector<c2d::Recovery::Entry> found = c2d::Recovery(recDir).pending();
        check(found.size() == 1 && found.at(0).copy == copy && found.at(0).source == source
                  && !found.at(0).untitled, "a left-over copy newer than its file is offered");
        c2d::Document rec;
        check(rec.load(copy, &err) && rec.elements().size() == doc.elements().size(),
              "the copy holds the edits");
        touch(3600);
        check(c2d::Recovery(recDir).pending().isEmpty() && !QFile::exists(copy),
              "a copy older than its file is stale and removed");
        c2d::Recovery again(recDir);
        copy = again.write(doc, QString(), true, &err);
        check(!copy.isEmpty(), "untitled recovery copy written");
        again.discard();
        check(!QFile::exists(copy), "discard() deletes it");
    }
    if (!err.isEmpty())
        std::printf("     last error: %s\n", qPrintable(err));
    return g_fail ? 1 : 0;
}
