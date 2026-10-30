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
    static const QRegularExpression ok(
        QStringLiteral("^(materials\\.json|[a-z0-9_-]+/(kind\\.png|[0-9a-f]{6}\\.json))$"));
    return ok.match(rel).hasMatch();
}

void ToolSync::start(const QString &baseUrl, const QString &cacheDir)
{
    m_base = baseUrl.endsWith(QLatin1Char('/')) ? baseUrl : baseUrl + QLatin1Char('/');
    m_cache = cacheDir;
    m_queue.clear();
    m_sha.clear();
    m_updated = m_failed = 0;
    QNetworkRequest rq{QUrl(m_base + QStringLiteral("index.json"))};
    rq.setTransferTimeout(8000);
    QNetworkReply *r = m_net->get(rq);
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError) {
            done(QStringLiteral("offline: %1").arg(r->errorString()));
            return;
        }
        const QJsonObject files = QJsonDocument::fromJson(r->read(kMaxFile)).object()
                                      .value(QStringLiteral("files")).toObject();
        if (files.isEmpty()) {
            done(QStringLiteral("index.json has no files"));
            return;
        }
        // What the app has now: the synced copy, else the built-in one.
        const QMap<QString, QByteArray> have = [&] {
            QMap<QString, QByteArray> m = ToolLibrary::readTree(QStringLiteral(":/data/toollib"));
            const QMap<QString, QByteArray> c = ToolLibrary::readTree(m_cache);
            for (auto it = c.constBegin(); it != c.constEnd(); ++it)
                m.insert(it.key(), it.value());
            return m;
        }();
        for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
            const QString sha = it.value().toString();
            if (!validPath(it.key()) || sha.size() != 40) {
                ++m_failed;
                continue;
            }
            if (have.contains(it.key()) && QString::fromLatin1(sha1Of(have.value(it.key()))) == sha)
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
        done(m_updated || m_failed
                 ? QStringLiteral("%1 updated, %2 rejected").arg(m_updated).arg(m_failed)
                 : QStringLiteral("up to date"));
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
        if (ok) {
            const QString path = m_cache + QLatin1Char('/') + rel;
            QDir().mkpath(QFileInfo(path).absolutePath());
            QSaveFile f(path);
            ok = f.open(QIODevice::WriteOnly) && f.write(body) == body.size() && f.commit();
        }
        ok ? ++m_updated : ++m_failed;
        fetchNext();
    });
}

void ToolSync::done(const QString &note)
{
    emit finished(m_updated, m_failed, note);
}

} // namespace c2d
