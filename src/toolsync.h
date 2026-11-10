#pragma once
#include <QHash>
#include <QObject>
#include <QStringList>

class QNetworkAccessManager;

namespace c2d {

// Keeps the app's copy of the tool library (the repo's toollib/ folder) up to
// date: fetches toollib/index.json (file path -> sha1, and a serial that goes
// up with every change). An index no newer than the built-in one means the
// app already has it all: the synced copy is cleared away. A newer one: the
// files whose sha1 differs from what the app has (the synced copy, else the
// built-in one) are downloaded, checked against the index sha1 and their
// type, and only when every one of them arrived are they written to
// ToolLibrary::cacheDir() with the index, which is what makes the copy count
// (ToolLibrary::currentTree). Files the index no longer lists are removed
// from the cache. Offline or a bad reply changes nothing.
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
    void commit();
    void prune(const QStringList &keep);

    QStringList m_queue;
    QHash<QString, QString> m_sha;     // wanted sha1 per path
    QHash<QString, QByteArray> m_got;  // fetched and checked, not yet written
    QByteArray m_index;                // the index being synced to
    QStringList m_listed;              // every valid path in it
    int m_updated = 0, m_failed = 0, m_missed = 0;   // missed: wanted files that did not arrive
};

} // namespace c2d
