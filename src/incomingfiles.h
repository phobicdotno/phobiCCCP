#pragma once
#include <QByteArray>
#include <QSet>
#include <QString>
#include <QStringList>

// Files that reach the pad from outside the app: picked in Android's file
// picker, opened with phobiCCCP from the Files app or a browser's downloads,
// or shared to it from another app. Android hands them over as a copy in the
// app's cache under whatever name the other app gave (sometimes none, or one
// without an extension); what is here decides what kind of file it is and
// where it is kept, so the window only has to open or import it.
// No Android calls in here: the same rules run in the desktop tests.
namespace c2d::incoming {

enum class Kind {
    Design,       // .c2d: kept in the designs folder and opened there
    Cccp,         // a Syntexophobic project: opened as a new design
    Vectors,      // SVG, DXF, HPGL (and PDF/AI/EPS on a computer): imported
    Unsupported,  // anything else (G-code too: a pad has nothing to do with it)
};

// By the name's extension, ignoring case.
Kind kindOf(const QString &fileName);

// What the first bytes of a file say it is: "c2d" (an SQLite file), "svg",
// "dxf", or empty when they say nothing certain.
QString sniffSuffix(const QByteArray &head);

// A plain file name made from the name another app gave: no folders, no
// characters a file system may refuse, never empty. When the name has no
// extension phobiCCCP knows, the one `head` shows is added ("Sign" holding
// an SQLite file becomes "Sign.c2d").
QString cleanName(const QString &displayName, const QByteArray &head = {});

// `dir`/`name`, or "name (2).ext", "name (3).ext", ... when that is taken.
QString uniquePath(const QString &dir, const QString &name);

// Put the copy at `cachedPath`, which arrived as `displayName`, where it
// belongs and return its new path: a design goes into `designsDir` (the same
// file picked again opens the copy already there, not a second one); other
// files stay beside the cached copy under their clean name, which the
// importers go by. Empty with `error` set when it cannot be moved.
QString settle(const QString &cachedPath, const QString &displayName,
               const QString &designsDir, QString *error = nullptr);

// Designs kept by the PhobiCCCP pad app that came before this one (Flutter,
// same app id, so its files are still there after this app replaced it):
// one <id>.json per design in its designs folder.
struct Adopted {
    QStringList made;      // the .c2d files written
    QStringList ids;       // the designs they were made from, in the same order
    QStringList failed;    // "name: why" for designs that could not be built
    int skipped = 0;       // deleted designs, unreadable files, already adopted
};

// How many live designs in `fromDir` are not in `alreadyAdopted`.
int padDesignsWaiting(const QString &fromDir, const QSet<QString> &alreadyAdopted);

// Write each live design in `fromDir` whose id is not in `alreadyAdopted` as
// a .c2d in `toDir`, named after the design. The JSON files are only read,
// never changed or removed.
Adopted adoptPadDesigns(const QString &fromDir, const QString &toDir,
                        const QSet<QString> &alreadyAdopted = {});

} // namespace c2d::incoming
