#include "incomingfiles.h"

#include "padlink.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace c2d::incoming {

namespace {

// The vector formats the pad can import: PDF, Illustrator and EPS go through
// programs a pad does not have (importers.cpp, importFileFilter).
const QStringList &vectorSuffixes()
{
    static const QStringList s{
        QStringLiteral("svg"), QStringLiteral("dxf"), QStringLiteral("plt"),
        QStringLiteral("hpgl"), QStringLiteral("hpg"), QStringLiteral("hgl"),
#ifndef Q_OS_ANDROID
        QStringLiteral("pdf"), QStringLiteral("ai"), QStringLiteral("eps"),
        QStringLiteral("ps"),
#endif
    };
    return s;
}

QByteArray readHead(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.read(4096);
}

bool sameContents(const QString &a, const QString &b)
{
    QFile fa(a), fb(b);
    if (fa.size() != fb.size() || !fa.open(QIODevice::ReadOnly) || !fb.open(QIODevice::ReadOnly))
        return false;
    while (!fa.atEnd()) {
        if (fa.read(1 << 16) != fb.read(1 << 16))
            return false;
    }
    return true;
}

// The live designs in the old app's folder, in a fixed order (by file name).
QList<QJsonObject> padDesigns(const QString &fromDir, int *unreadable)
{
    QList<QJsonObject> out;
    const QFileInfoList files = QDir(fromDir).entryInfoList({QStringLiteral("*.json")},
                                                             QDir::Files, QDir::Name);
    for (const QFileInfo &fi : files) {
        QFile f(fi.filePath());
        // A design is a few kB to a few MB; anything far bigger is not one.
        if (fi.size() > 64 * 1024 * 1024 || !f.open(QIODevice::ReadOnly)) {
            ++*unreadable;
            continue;
        }
        const QJsonObject d = QJsonDocument::fromJson(f.readAll()).object();
        if (d.value(QStringLiteral("format")).toString() != QLatin1String("phobicccp-pad")
            || d.value(QStringLiteral("id")).toString().isEmpty()) {
            ++*unreadable;
            continue;
        }
        out.append(d);
    }
    return out;
}

} // namespace

Kind kindOf(const QString &fileName)
{
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    if (suffix == QLatin1String("c2d"))
        return Kind::Design;
    if (suffix == QLatin1String("cccp"))
        return Kind::Cccp;
    if (vectorSuffixes().contains(suffix))
        return Kind::Vectors;
    return Kind::Unsupported;
}

QString sniffSuffix(const QByteArray &head)
{
    if (head.startsWith(QByteArrayLiteral("SQLite format 3")))
        return QStringLiteral("c2d");
    if (head.startsWith(QByteArrayLiteral("AutoCAD Binary DXF")))
        return QStringLiteral("dxf");
    // An ASCII DXF opens with group code 0 and SECTION, each on its own line.
    const QList<QByteArray> lines = head.left(64).split('\n');
    if (lines.size() >= 2 && lines.at(0).trimmed() == "0" && lines.at(1).trimmed() == "SECTION")
        return QStringLiteral("dxf");
    if (head.left(1024).contains("<svg"))
        return QStringLiteral("svg");
    return {};
}

QString cleanName(const QString &displayName, const QByteArray &head)
{
    // Only the last part of anything that looks like a path.
    QString s = displayName.section(QRegularExpression(QStringLiteral("[/\\\\]")), -1);
    s.replace(QRegularExpression(QStringLiteral("[\\x00-\\x1f:*?\"<>|]")), QStringLiteral("_"));
    s = s.trimmed();
    while (s.startsWith(QLatin1Char('.')))
        s.remove(0, 1);
    s = s.trimmed();
    if (s.size() > 120) {
        const QString suffix = QFileInfo(s).suffix();
        s = s.left(120 - suffix.size() - 1).trimmed() + QLatin1Char('.') + suffix;
    }
    if (s.isEmpty())
        s = QStringLiteral("file");
    if (kindOf(s) == Kind::Unsupported) {
        const QString sniffed = sniffSuffix(head);
        if (!sniffed.isEmpty())
            s += QLatin1Char('.') + sniffed;
    }
    return s;
}

QString uniquePath(const QString &dir, const QString &name)
{
    const QDir d(dir);
    if (!QFileInfo::exists(d.filePath(name)))
        return d.filePath(name);
    const QFileInfo fi(name);
    const QString base = fi.completeBaseName();
    const QString suffix = fi.suffix().isEmpty() ? QString() : QLatin1Char('.') + fi.suffix();
    for (int n = 2; n < 1000; ++n) {
        const QString p = d.filePath(QStringLiteral("%1 (%2)%3").arg(base).arg(n).arg(suffix));
        if (!QFileInfo::exists(p))
            return p;
    }
    return {};
}

QString settle(const QString &cachedPath, const QString &displayName,
               const QString &designsDir, QString *error)
{
    auto fail = [&](const QString &why) {
        if (error)
            *error = why;
        return QString();
    };
    if (!QFileInfo(cachedPath).isFile())
        return fail(QStringLiteral("%1 did not arrive").arg(displayName));
    const QString name = cleanName(displayName.isEmpty() ? QFileInfo(cachedPath).fileName() : displayName,
                                   readHead(cachedPath));
    if (kindOf(name) == Kind::Design) {
        if (!QDir().mkpath(designsDir))
            return fail(QStringLiteral("cannot make the folder %1").arg(designsDir));
        // Picked again (the same file from Downloads, say): the copy already here.
        const QString same = QDir(designsDir).filePath(name);
        if (QFileInfo::exists(same) && sameContents(same, cachedPath)) {
            QFile::remove(cachedPath);
            return same;
        }
        const QString dest = uniquePath(designsDir, name);
        if (dest.isEmpty())
            return fail(QStringLiteral("too many files called %1 in %2").arg(name, designsDir));
        // Rename where it can (the same file system); copy where it cannot.
        if (!QFile::rename(cachedPath, dest)) {
            if (!QFile::copy(cachedPath, dest))
                return fail(QStringLiteral("could not copy %1 into %2").arg(name, designsDir));
            QFile::remove(cachedPath);
        }
        return dest;
    }
    const QFileInfo cached(cachedPath);
    if (cached.fileName() == name)
        return cached.filePath();
    const QString dest = uniquePath(cached.absolutePath(), name);
    if (dest.isEmpty() || !QFile::rename(cachedPath, dest))
        return fail(QStringLiteral("could not rename %1 to %2").arg(cached.fileName(), name));
    return dest;
}

int padDesignsWaiting(const QString &fromDir, const QSet<QString> &alreadyAdopted)
{
    int unreadable = 0, n = 0;
    for (const QJsonObject &d : padDesigns(fromDir, &unreadable))
        if (!d.value(QStringLiteral("deleted")).toBool()
            && !alreadyAdopted.contains(d.value(QStringLiteral("id")).toString()))
            ++n;
    return n;
}

Adopted adoptPadDesigns(const QString &fromDir, const QString &toDir,
                        const QSet<QString> &alreadyAdopted)
{
    Adopted out;
    const QList<QJsonObject> designs = padDesigns(fromDir, &out.skipped);
    if (designs.isEmpty())
        return out;
    QDir().mkpath(toDir);
    for (const QJsonObject &d : designs) {
        const QString id = d.value(QStringLiteral("id")).toString();
        if (d.value(QStringLiteral("deleted")).toBool() || alreadyAdopted.contains(id)) {
            ++out.skipped;
            continue;
        }
        QString name = d.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty())
            name = QStringLiteral("Pad design");
        name.replace(QRegularExpression(QStringLiteral("[/\\\\]")), QStringLiteral("-"));
        const QString path = uniquePath(toDir, cleanName(name + QStringLiteral(".c2d")));
        QString err;
        if (path.isEmpty() || !padlink::buildDocument(d, path, nullptr, &err)) {
            out.failed << QStringLiteral("%1: %2").arg(name, path.isEmpty() ? QStringLiteral("no free file name") : err);
            continue;
        }
        out.made << path;
        out.ids << id;
    }
    return out;
}

} // namespace c2d::incoming
