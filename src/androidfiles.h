#pragma once
#include <QObject>
#include <QString>
#include <QStringList>

// The pad's way to Android's own file handling (qt-android/src/.../PhobicActivity.java):
// the system file picker, "Open with" and "Share" coming in, and the share
// sheet and the save dialog going out. Everything arrives as a plain file in
// the app's cache (Android's content:// links cannot hold an SQLite design).
// On a computer none of it exists: available() is false and the calls do nothing.
namespace c2d::androidfiles {

enum Purpose { Open = 0, Import = 1, Incoming = 2 };

bool available();

// Sends what arrives from Android and what it says back, on the GUI thread.
class Hub : public QObject
{
    Q_OBJECT
public:
    static Hub *instance();
signals:
    // A copy of the file the user picked, opened with or shared to the app,
    // and the name the other app gave it.
    void arrived(const QString &path, const QString &displayName, int purpose);
    void message(const QString &text, bool error);
};

// The program is ready for files: the one that started the app (if any)
// arrives now, later ones as they come. Connect to Hub first.
void start();

// Android's picker for a file from anywhere; it arrives through Hub::arrived.
void pick(Purpose purpose);
// The share sheet for files in the app's folders (Documents or the cache).
void share(const QStringList &paths, const QString &mimeType, const QString &title);
// Android's save dialog; `path` is copied to where the user chooses.
void saveCopy(const QString &path, const QString &mimeType, const QString &suggestedName);

// Where the earlier PhobiCCCP pad app (Flutter) kept its designs; empty off Android.
QString oldPadDesignsDir();

} // namespace c2d::androidfiles
