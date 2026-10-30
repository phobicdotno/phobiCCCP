#pragma once
#include <QHash>
#include <QObject>
#include <QStringList>

class QNetworkAccessManager;

namespace c2d {

// Keeps the app's copy of the tool library (the repo's toollib/ folder) up to
// date: fetches toollib/index.json (file path -> sha1), downloads only the
// files whose sha1 differs from what the app already has (the synced copy,
// else the built-in one) and writes them to ToolLibrary::cacheDir(). Every
// file is checked against the index sha1 and against its type before it is
// kept; anything else is dropped. Offline or a bad reply changes nothing.
class ToolSync : public QObject
{
    Q_OBJECT
public:
    explicit ToolSync(QObject *parent = nullptr);
    // PHOBIC_TOOLLIB_URL, else the raw GitHub folder of the main branch.
    static QString defaultBaseUrl();
    void start(const QString &baseUrl, const QString &cacheDir);
    static bool validPath(const QString &rel);

    struct Result { int updated; int failed; };

signals:
    // updated: files written, failed: files rejected or not fetched.
    void finished(int updated, int failed, const QString &note);

private:
    void fetchNext();
    void done(const QString &note);

    QNetworkAccessManager *m_net = nullptr;
    QString m_base, m_cache;
    QStringList m_queue;
    QHash<QString, QString> m_sha;     // wanted sha1 per path
    int m_updated = 0, m_failed = 0;
};

} // namespace c2d
