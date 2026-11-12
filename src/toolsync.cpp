#include "toolsync.h"

#include "toollibrary.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUrl>

namespace c2d {

namespace {
constexpr qint64 kMaxFile = 1 << 20;

QByteArray sha1Of(const QByteArray &b)
{
    return QCryptographicHash::hash(b, QCryptographicHash::Sha1).toHex();
}
} // namespace

ToolSync::ToolSync(QObject *parent) : QObject(parent), m_net(new QNetworkAccessManager(this)) {}

QString ToolSync::defaultBaseUrl()
{
    const QByteArray env = qgetenv("PHOBIC_TOOLLIB_URL");
    if (!env.isEmpty())
        return QString::fromLocal8Bit(env);
    return QStringLiteral("https://raw.githubusercontent.com/phobicdotno/phobiCCCP/main/toollib/");
}

bool ToolSync::validPath(const QString &rel)
{
    return ToolLibrary::validPath(rel);
}

void ToolSync::start(const QString &baseUrl, const QString &cacheDir)
{
    // One sync at a time: a second start would clear the queue and the
    // fetched files under the replies still on their way.
    if (m_busy)
        return;
    m_busy = true;
    m_base = baseUrl.endsWith(QLatin1Char('/')) ? baseUrl : baseUrl + QLatin1Char('/');
    m_cache = cacheDir;
    m_queue.clear();
    m_sha.clear();
    m_got.clear();
    m_listed.clear();
    m_index.clear();
    m_updated = m_failed = m_missed = 0;
    QNetworkRequest rq{QUrl(m_base + QStringLiteral("index.json"))};
    rq.setTransferTimeout(8000);
    QNetworkReply *r = m_net->get(rq);
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError) {
            done(QStringLiteral("offline: %1").arg(r->errorString()));
            return;
        }
        m_index = r->read(kMaxFile);
        const QJsonObject files = QJsonDocument::fromJson(m_index).object()
                                      .value(QStringLiteral("files")).toObject();
        if (files.isEmpty()) {
            done(QStringLiteral("index.json has no files"));
            return;
        }
        const QMap<QString, QByteArray> built = ToolLibrary::readTree(QStringLiteral(":/data/toollib"));
        const int builtSerial = ToolLibrary::indexSerial(built.value(QStringLiteral("index.json")));
        if (ToolLibrary::indexSerial(m_index) <= builtSerial) {
            // The app was built from this toollib/ or a later one. A synced
            // copy older than the built-in one is of no use any more.
            QFile ix(m_cache + QStringLiteral("/index.json"));
            if (!ix.open(QIODevice::ReadOnly) || ToolLibrary::indexSerial(ix.read(kMaxFile)) <= builtSerial) {
                ix.close();
                prune({});
            }
            done(QStringLiteral("up to date"));
            return;
        }
        // What the app has now: the synced copy, else the built-in one.
        const QMap<QString, QByteArray> cached = ToolLibrary::readTree(m_cache);
        for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
            const QString sha = it.value().toString();
            if (!validPath(it.key()) || sha.size() != 40) {
                ++m_failed;
                continue;
            }
            m_listed << it.key();
            if ((cached.contains(it.key()) && QString::fromLatin1(sha1Of(cached.value(it.key()))) == sha)
                || (built.contains(it.key()) && QString::fromLatin1(sha1Of(built.value(it.key()))) == sha))
                continue;
            m_sha.insert(it.key(), sha);
            m_queue << it.key();
        }
        fetchNext();
    });
}

void ToolSync::fetchNext()
{
    if (m_queue.isEmpty()) {
        commit();
        return;
    }
    const QString rel = m_queue.takeFirst();
    QNetworkRequest rq{QUrl(m_base + rel)};
    rq.setTransferTimeout(8000);
    QNetworkReply *r = m_net->get(rq);
    connect(r, &QNetworkReply::finished, this, [this, r, rel] {
        r->deleteLater();
        const QByteArray body = r->error() == QNetworkReply::NoError ? r->read(kMaxFile + 1) : QByteArray();
        bool ok = !body.isEmpty() && body.size() <= kMaxFile
                  && QString::fromLatin1(sha1Of(body)) == m_sha.value(rel);
        if (ok && rel.endsWith(QLatin1String(".png")))
            ok = !QImage::fromData(body).isNull();
        else if (ok)
            ok = QJsonDocument::fromJson(body).isObject();
        if (ok)
            m_got.insert(rel, body);
        else
            ++m_missed;
        fetchNext();
    });
}

// All of it or none: the index is what makes the synced copy count, and a
// copy with one file of the new index missing would show that bit as the
// built-in (older) one, or not at all. The files first, the index last.
void ToolSync::commit()
{
    if (m_missed > 0) {
        m_failed += m_missed;
        done(QStringLiteral("%1 file(s) did not arrive; trying again at the next start").arg(m_missed));
        return;
    }
    bool ok = true;
    auto write = [&](const QString &rel, const QByteArray &body) {
        const QString path = m_cache + QLatin1Char('/') + rel;
        QDir().mkpath(QFileInfo(path).absolutePath());
        QSaveFile f(path);
        return f.open(QIODevice::WriteOnly) && f.write(body) == body.size() && f.commit();
    };
    for (auto it = m_got.constBegin(); ok && it != m_got.constEnd(); ++it) {
        ok = write(it.key(), it.value());
        if (ok)
            ++m_updated;
    }
    QFile cur(m_cache + QStringLiteral("/index.json"));
    const bool sameIndex = cur.open(QIODevice::ReadOnly) && cur.read(kMaxFile + 1) == m_index;
    cur.close();
    if (ok && !sameIndex)
        ok = write(QStringLiteral("index.json"), m_index);
    if (!ok) {
        ++m_failed;
        done(QStringLiteral("could not write to %1").arg(m_cache));
        return;
    }
    if (!m_listed.isEmpty())
        prune(m_listed);
    done(m_updated || m_failed
             ? QStringLiteral("%1 updated, %2 rejected").arg(m_updated).arg(m_failed)
             : QStringLiteral("up to date"));
}

// Library files in the cache that are not in `keep` (and, with nothing to
// keep, its index too). Only files of the library's own pattern are touched.
void ToolSync::prune(const QStringList &keep)
{
    const QSet<QString> want(keep.constBegin(), keep.constEnd());
    const QMap<QString, QByteArray> cached = ToolLibrary::readTree(m_cache);
    QSet<QString> dirs;
    for (auto it = cached.constBegin(); it != cached.constEnd(); ++it) {
        const bool ours = validPath(it.key()) || (keep.isEmpty() && it.key() == QLatin1String("index.json"));
        if (!ours || want.contains(it.key()))
            continue;
        QFile::remove(m_cache + QLatin1Char('/') + it.key());
        const int slash = it.key().indexOf(QLatin1Char('/'));
        if (slash > 0)
            dirs.insert(it.key().left(slash));
    }
    for (const QString &d : dirs)
        QDir(m_cache).rmdir(d);   // only when it is empty now
}

void ToolSync::done(const QString &note)
{
    m_busy = false;   // before the signal: a slot may start the next sync
    emit finished(m_updated, m_failed, note);
}

} // namespace c2d
