#include "recovery.h"
#include "c2ddocument.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QMutex>
#include <QSet>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUuid>

#include <algorithm>

namespace c2d {

static QString sidecar(const QString &copy) { return copy + QStringLiteral(".source"); }
static QString lockName(const QString &copy) { return copy + QStringLiteral(".lock"); }

// Locks this process holds right now, by path. QLockFile tells a live owner
// from a dead one by its PID, and after a crash and a reboot the new session
// often gets the dead one's PID back: its leftover lock then reads as "held
// by a running process" (ours) for ever, so the copy is never offered and
// write() silently stops saving. A lock that names this process but that
// this process did not take is such a leftover.
static QMutex g_heldMutex;
static QSet<QString> &heldHere()
{
    static QSet<QString> held;
    return held;
}

static std::unique_ptr<QLockFile> takeLock(const QString &copy)
{
    const QString path = lockName(copy);
    auto lock = std::make_unique<QLockFile>(path);
    lock->setStaleLockTime(0);   // stale only when its process is gone
    if (!lock->tryLock(0)) {
        qint64 pid = 0;
        QString host, app;
        QMutexLocker guard(&g_heldMutex);
        if (lock->error() != QLockFile::LockFailedError || heldHere().contains(path)
            || !lock->getLockInfo(&pid, &host, &app)
            || pid != QCoreApplication::applicationPid()
            || host != QSysInfo::machineHostName()
            || !lock->removeStaleLockFile() || !lock->tryLock(0))
            return nullptr;
    }
    QMutexLocker guard(&g_heldMutex);
    heldHere().insert(path);
    return lock;
}

static void releaseLock(std::unique_ptr<QLockFile> &lock)
{
    if (!lock)
        return;
    {
        QMutexLocker guard(&g_heldMutex);
        heldHere().remove(lock->fileName());
    }
    lock.reset();
}

// Tells this process's untitled copies from those of an earlier process that
// had the same PID.
static QString sessionTag()
{
    static const QString tag = QUuid::createUuid().toString(QUuid::Id128).left(12);
    return tag;
}

Recovery::Recovery(const QString &dir)
    : m_dir(dir.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                                + QStringLiteral("/recovery")
                          : dir)
{
    static int instances = 0;
    m_instance = instances++;
}

Recovery::~Recovery()
{
    releaseLock(m_lock);
}

QString Recovery::pathFor(const QString &source, bool untitled) const
{
    // Untitled designs are per process; a file is the same copy whichever
    // way its path was spelled.
    QString name;
    if (untitled) {
        name = QStringLiteral("untitled-%1-%2").arg(QCoreApplication::applicationPid())
                   .arg(sessionTag());
        if (m_instance > 0)                     // a second window in this process
            name += QStringLiteral("-%1").arg(m_instance);
    } else {
        const QFileInfo fi(source);
        const QString key = fi.exists() ? fi.canonicalFilePath() : fi.absoluteFilePath();
        name = QString::fromLatin1(
            QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
    }
    return QDir(m_dir).filePath(name + QStringLiteral(".c2d"));
}

QString Recovery::write(const Document &doc, const QString &source, bool untitled,
                        QString *error)
{
    const QString copy = pathFor(source, untitled);
    // A new copy is written before the old one is removed: if this write
    // fails, the last good copy is still there.
    std::unique_ptr<QLockFile> newLock;
    if (copy != m_current) {
        if (!QDir().mkpath(m_dir)) {
            if (error) *error = QStringLiteral("Cannot create %1").arg(m_dir);
            return {};
        }
        newLock = takeLock(copy);
        if (!newLock)
            return {};                  // another window has this file open
    }
    QFile side(sidecar(copy));
    if (!side.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || side.write(source.toUtf8() + (untitled ? "\nuntitled\n" : "\n")) < 0) {
        if (error) *error = side.errorString();
        return {};
    }
    side.close();
    // Document::save moves the document onto the path it writes; a copy
    // keeps the caller's document on the user's file. save() also starts
    // from the document's own container: when that file has gone (a pulled
    // stick, a moved file), start from the last recovery copy instead.
    Document d = doc;
    if (!QFileInfo::exists(d.filePath())) {
        if (QFileInfo::exists(copy))
            d.setFilePath(copy);
        else if (!m_current.isEmpty() && QFileInfo::exists(m_current))
            d.setFilePath(m_current);
    }
    if (!d.save(copy, error)) {
        if (newLock) {
            QFile::remove(sidecar(copy));
            releaseLock(newLock);
        }
        return {};
    }
    if (newLock) {
        if (!m_current.isEmpty())
            remove(m_current);
        releaseLock(m_lock);
        m_lock = std::move(newLock);
        m_current = copy;
    }
    return copy;
}

void Recovery::discard()
{
    if (!m_current.isEmpty())
        remove(m_current);
    releaseLock(m_lock);
    m_current.clear();
}

void Recovery::adopt(const QString &copy)
{
    discard();
    m_lock = takeLock(copy);
    m_current = copy;
}

void Recovery::remove(const QString &copy)
{
    QFile::remove(copy);
    QFile::remove(sidecar(copy));
}

QVector<Recovery::Entry> Recovery::pending() const
{
    QVector<Entry> out;
    const QDir d(m_dir);
    for (const QFileInfo &fi : d.entryInfoList({QStringLiteral("*.c2d")}, QDir::Files)) {
        const QString copy = fi.absoluteFilePath();
        if (copy == m_current)
            continue;
        std::unique_ptr<QLockFile> lock = takeLock(copy);
        if (!lock)
            continue;                // its session is still running
        releaseLock(lock);
        QFile side(sidecar(copy));
        if (!side.open(QIODevice::ReadOnly)) {
            remove(copy);            // no record of what it belongs to
            continue;
        }
        const QStringList lines = QString::fromUtf8(side.readAll()).split(QLatin1Char('\n'));
        Entry e;
        e.copy = copy;
        e.source = lines.value(0);
        e.untitled = lines.value(1) == QLatin1String("untitled");
        e.saved = fi.lastModified();
        // An untitled design's scratch file never holds its edits; a real
        // file saved after the copy was written supersedes it.
        const QFileInfo src(e.source);
        if (!e.untitled && src.exists() && src.lastModified() >= e.saved) {
            remove(copy);
            continue;
        }
        out.append(e);
    }
    std::sort(out.begin(), out.end(),
              [](const Entry &a, const Entry &b) { return a.saved > b.saved; });
    return out;
}

} // namespace c2d
