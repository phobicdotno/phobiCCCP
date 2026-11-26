// Saving stages the new file beside the destination under a name of its own
// (process id and a random part): two copies of the app saving to one file
// shared <dest>.phobisave and spoilt each other's copy. A save leaves no
// staging file, clears ones a crashed save left (an hour old or more) and
// leaves a recent one alone, which may be another process's save in progress.
#include "../src/c2ddocument.h"
#include "../src/element.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <cstdio>

static int g_fail = 0;
static void check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++g_fail;
}

static QStringList staging(const QString &dir)
{
    return QDir(dir).entryList({QStringLiteral("*phobisave*")}, QDir::Files | QDir::Hidden);
}

static bool touch(const QString &path, const QDateTime &when)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write("half a save");
    f.flush();   // or the close writes it later and moves the time to now
    return f.setFileTime(when, QFileDevice::FileModificationTime);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    const QString blank = dir.filePath(QStringLiteral("new.c2d"));
    const QString dest = dir.filePath(QStringLiteral("dest.c2d"));
    QString err;
    check(c2d::Document::createBlank(blank, 100, 100, 10, &err), "createBlank");

    c2d::Document doc;
    check(doc.load(blank, &err), "load blank");
    doc.elementsRef().append(c2d::Element::makeCircle(QPointF(50, 50), 10, doc.defaultLayer()));
    check(doc.save(dest, &err), "save");
    check(staging(dir.path()).isEmpty(), "a save leaves no staging file");

    const QDateTime old = QDateTime::currentDateTime().addSecs(-2 * 3600);
    const QString legacy = dest + QStringLiteral(".phobisave");
    const QString crashed = dest + QStringLiteral(".4242-0badf00d.phobisave");
    const QString crashedJournal = crashed + QStringLiteral("-journal");
    const QString running = dest + QStringLiteral(".4343-12345678.phobisave");
    const QString otherFile = dir.filePath(QStringLiteral("other.c2d.1-00000000.phobisave"));
    check(touch(legacy, old) && touch(crashed, old) && touch(crashedJournal, old)
              && touch(running, QDateTime::currentDateTime()) && touch(otherFile, old),
          "stale and live staging files set up");

    doc.elementsRef().append(c2d::Element::makeCircle(QPointF(20, 20), 5, doc.defaultLayer()));
    check(doc.save(dest, &err), "save again");
    check(!QFileInfo::exists(legacy), "the old shared staging name, an hour old, is cleared");
    check(!QFileInfo::exists(crashed) && !QFileInfo::exists(crashedJournal),
          "a crashed save's staging file and journal are cleared");
    check(QFileInfo::exists(running), "a recent staging file (another save running) is left alone");
    check(QFileInfo::exists(otherFile), "another document's staging files are not touched");
    QStringList left = staging(dir.path());
    left.removeAll(QFileInfo(running).fileName());
    left.removeAll(QFileInfo(otherFile).fileName());
    check(left.isEmpty(), "the save itself leaves nothing behind");

    c2d::Document back;
    check(back.load(dest, &err) && back.elements().size() == 2, "the saved file reads back");
    return g_fail ? 1 : 0;
}
