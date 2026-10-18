#pragma once
#include <QDateTime>
#include <QString>
#include <QVector>
#include <memory>

class QLockFile;

// Crash recovery: while a design has unsaved edits, MainWindow writes a copy
// of it every couple of minutes into <AppData>/recovery (never over the
// user's file). A clean save or close deletes the copy; one left behind by a
// session that died is offered at the next start. Each copy is held by a lock
// file while its session runs, so a second window does not offer the first
// one's live copy.
namespace c2d {

class Document;

class Recovery
{
public:
    // `dir` empty: QStandardPaths::AppDataLocation + "/recovery".
    explicit Recovery(const QString &dir = QString());
    ~Recovery();   // releases the lock, keeps the copy (only discard() deletes it)

    // Write `doc` as the recovery copy of `source` (the user's file, or the
    // scratch file of an untitled design). `doc` itself is not retargeted.
    // Returns the copy's path, or empty with `error` set; empty without an
    // error when another running session owns that copy.
    QString write(const Document &doc, const QString &source, bool untitled,
                  QString *error = nullptr);
    // Delete the current copy: the edits are saved, or deliberately dropped.
    void discard();
    // Make an existing copy the current one (the design was just opened from
    // it), so the next write or discard replaces it.
    void adopt(const QString &copy);
    QString current() const { return m_current; }
    QString dir() const { return m_dir; }

    struct Entry {
        QString copy, source;
        bool untitled = false;
        QDateTime saved;
    };
    // Copies no running session owns and that are newer than their source,
    // newest first. Copies older than their source are stale and deleted.
    QVector<Entry> pending() const;
    static void remove(const QString &copy);

private:
    QString pathFor(const QString &source, bool untitled) const;

    QString m_dir;
    QString m_current;
    int m_instance = 0;   // tells apart untitled designs of one process
    std::unique_ptr<QLockFile> m_lock;
};

} // namespace c2d
