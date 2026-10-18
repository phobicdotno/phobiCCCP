#include "recovery.h"
#include "c2ddocument.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QStandardPaths>

#include <algorithm>

namespace c2d {

static QString sidecar(const QString &copy) { return copy + QStringLiteral(".source"); }
static QString lockName(const QString &copy) { return copy + QStringLiteral(".lock"); }

Recovery::Recovery(const QString &dir)
    : m_dir(dir.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                                + QStringLiteral("/recovery")
                          : dir)
{
}

Recovery::~Recovery() = default;

QString Recovery::pathFor(const QString &source, bool untitled) const
{
    // Untitled designs are per process; a file is the same copy whichever
    // way its path was spelled.
    QString name;
    if (untitled) {
        name = QStringLiteral("untitled-%1").arg(QCoreApplication::applicationPid());
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
    if (copy != m_current) {
        discard();
        if (!QDir().mkpath(m_dir)) {
            if (error) *error = QStringLiteral("Cannot create %1").arg(m_dir);
            return {};
        }
        auto lock = std::make_unique<QLockFile>(lockName(copy));
        lock->setStaleLockTime(0);   // stale only when its process is gone
        if (!lock->tryLock(0))
            return {};               // another window has this file open
        m_lock = std::move(lock);
        m_current = copy;
    }
    QFile side(sidecar(copy));
    if (!side.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || side.write(source.toUtf8() + (untitled ? "\nuntitled\n" : "\n")) < 0) {
        if (error) *error = side.errorString();
        return {};
    }
    side.close();
    // Document::save moves the document onto the path it writes; a copy
    // keeps the caller's document on the user's file.
    Document d = doc;
    if (!d.save(copy, error))
        return {};
    return copy;
}

void Recovery::discard()
{
    if (!m_current.isEmpty())
        remove(m_current);
    m_lock.reset();
    m_current.clear();
}

void Recovery::adopt(const QString &copy)
{
    discard();
    auto lock = std::make_unique<QLockFile>(lockName(copy));
    lock->setStaleLockTime(0);
    if (lock->tryLock(0))
        m_lock = std::move(lock);
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
        QLockFile lock(lockName(copy));
        lock.setStaleLockTime(0);
        if (!lock.tryLock(0))
            continue;                // its session is still running
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
