#include "cccpfile.h"
#include "zlibutil.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QStringList>
#include <QtEndian>

#include <zlib.h>

namespace c2d {

namespace {

quint16 u16(const QByteArray &b, qsizetype at)
{
    return qFromLittleEndian<quint16>(b.constData() + at);
}

quint32 u32(const QByteArray &b, qsizetype at)
{
    return qFromLittleEndian<quint32>(b.constData() + at);
}

// Zip entries are raw deflate streams (no zlib header), so zlibInflate() can't
// take them; this is the same loop with a negative window size.
bool inflateRaw(const char *data, qsizetype size, qsizetype expected, QByteArray *out)
{
    if (expected < 0 || expected > kMaxInflateBytes)
        return false;
    out->resize(expected);
    z_stream zs{};
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK)
        return false;
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data));
    zs.avail_in = uInt(size);
    zs.next_out = reinterpret_cast<Bytef *>(out->data());
    zs.avail_out = uInt(expected);
    const int r = inflate(&zs, Z_FINISH);
    const bool ok = r == Z_STREAM_END && zs.total_out == uLong(expected);
    inflateEnd(&zs);
    return ok;
}

} // namespace

bool readZip(const QByteArray &zip, const QStringList &want, QHash<QString, QByteArray> *out,
             QString *error)
{
    auto fail = [error](const QString &why) {
        if (error) *error = why;
        return false;
    };
    // The end-of-central-directory record: 22 bytes plus up to 64 KB of comment.
    qsizetype eocd = -1;
    for (qsizetype i = zip.size() - 22; i >= 0 && i >= zip.size() - 22 - 0xFFFF; --i)
        if (u32(zip, i) == 0x06054b50) {
            eocd = i;
            break;
        }
    if (eocd < 0)
        return fail(QStringLiteral("not a zip archive"));
    const int count = u16(zip, eocd + 10);
    qsizetype at = u32(zip, eocd + 16);
    for (int n = 0; n < count; ++n) {
        if (at < 0 || at + 46 > zip.size() || u32(zip, at) != 0x02014b50)
            return fail(QStringLiteral("damaged zip directory"));
        const int method = u16(zip, at + 10);
        const qsizetype packed = u32(zip, at + 20), size = u32(zip, at + 24);
        const int nameLen = u16(zip, at + 28), extraLen = u16(zip, at + 30), commentLen = u16(zip, at + 32);
        const qsizetype local = u32(zip, at + 42);
        if (at + 46 + nameLen > zip.size())
            return fail(QStringLiteral("damaged zip directory"));
        const QString name = QString::fromUtf8(zip.constData() + at + 46, nameLen);
        at += 46 + nameLen + extraLen + commentLen;
        if (name.endsWith(QLatin1Char('/')) || (!want.isEmpty() && !want.contains(name)))
            continue;
        if (local + 30 > zip.size() || u32(zip, local) != 0x04034b50)
            return fail(QStringLiteral("damaged zip entry %1").arg(name));
        const qsizetype start = local + 30 + u16(zip, local + 26) + u16(zip, local + 28);
        if (start + packed > zip.size())
            return fail(QStringLiteral("truncated zip entry %1").arg(name));
        QByteArray bytes;
        if (method == 0) {
            if (packed != size)
                return fail(QStringLiteral("damaged zip entry %1").arg(name));
            bytes = zip.mid(start, packed);
        } else if (method == 8) {
            if (!inflateRaw(zip.constData() + start, packed, size, &bytes))
                return fail(QStringLiteral("could not unpack %1").arg(name));
        } else {
            return fail(QStringLiteral("%1 uses an unsupported zip compression").arg(name));
        }
        out->insert(name, bytes);
    }
    return true;
}

bool isCccpFile(const QString &path)
{
    return QFileInfo(path).suffix().compare(QLatin1String("cccp"), Qt::CaseInsensitive) == 0;
}

CccpProduct readCccp(const QString &path)
{
    CccpProduct p;
    const QString file = QFileInfo(path).fileName();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        p.error = QStringLiteral("Cannot read %1: %2").arg(path, f.errorString());
        return p;
    }
    // A .cccp carries the project's whole history, which phobiCCCP never
    // reads; the bound is for the archive held in memory, not the product.
    if (f.size() > 4 * kMaxInflateBytes) {
        p.error = QStringLiteral("%1 is too large to open").arg(file);
        return p;
    }
    const QByteArray zip = f.readAll();
    QHash<QString, QByteArray> entries;
    QString err;
    const QStringList want = {QStringLiteral("mimetype"), QStringLiteral("final/part.stl"),
                              QStringLiteral("final/drawing.svg"), QStringLiteral("final/info.json")};
    if (!readZip(zip, want, &entries, &err)) {
        p.error = QStringLiteral("%1: %2").arg(file, err);
        return p;
    }
    if (entries.value(QStringLiteral("mimetype")) != QByteArray(kCccpMimetype)) {
        p.error = QStringLiteral("%1 is not a Syntexophobic project file").arg(file);
        return p;
    }
    p.svg = entries.value(QStringLiteral("final/drawing.svg"));
    p.stl = entries.value(QStringLiteral("final/part.stl"));
    p.info = QJsonDocument::fromJson(entries.value(QStringLiteral("final/info.json"))).object();
    p.name = p.info.value(QStringLiteral("name")).toString(QFileInfo(path).completeBaseName());
    if (p.svg.isEmpty() && p.stl.isEmpty()) {
        p.error = QStringLiteral("%1 has no finished part yet: save a version of it in Syntexophobic first")
                      .arg(file);
        return p;
    }
    p.ok = true;
    return p;
}

} // namespace c2d
