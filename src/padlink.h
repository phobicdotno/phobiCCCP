#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>

// Designs made on the PhobiCCCP pad app (Android/ in this repository, Flutter
// for Android tablets and iPad).
// The pad does not know .c2d: it edits a small JSON design and phobiCCCP
// turns it into a document here, with the same factories the canvas and the
// toolpath panel use, so what arrives is an ordinary design.
//
// Design, version 1 (mm; X right, Y up, X0 Y0 the stock's front-left corner,
// as Carbide Create):
//
//   {"format": "phobicccp-pad", "version": 1,
//    "id": "<letters, digits, -; up to 64>", "name": "Sign",
//    "modified": <ms since 1970, set by whoever edited it last>,
//    "base": <the "modified" of the copy the pad last got from or gave to
//             this phobiCCCP; only in a put, never stored here (Store::put)>,
//    "vv": {"<replica id>": <the "modified" of that pad's or phobiCCCP's
//           last edit of the design>, ...},   // see "Versions" below
//    "desktopOnly": "<host>",             // only in a get: a linked file there
//                                          // is not rebuilt from pad edits
//    "deleted": false,                      // a removal, kept so it syncs
//    "stock": {"width": 300, "height": 200, "thickness": 19},
//    "material": "softwood",               // a tool library material id
//    "shapes": [
//      {"id": "s1", "type": "rect", "x": 10, "y": 10, "w": 100, "h": 50},  // x, y: lower-left
//      {"id": "s2", "type": "circle", "x": 50, "y": 50, "r": 20},          // x, y: centre
//      {"id": "s3", "type": "polygon", "x": 0, "y": 0, "r": 20, "sides": 6},
//      {"id": "s4", "type": "text", "x": 0, "y": 0, "text": "Hi", "size": 20},  // x, y: baseline-left
//      {"id": "s5", "type": "path", "points": [[0, 0], [10, 0], [10, 10]], "closed": true},
//      {"id": "s6", "type": "point", "x": 0, "y": 0}],
//    "toolpaths": [
//      {"name": "Outline", "type": "profile", "side": "outside",   // profile | pocket | drill | engrave
//       "shapes": ["s1"], "tool": 201, "depth": 6,                 // tool: library number; depth below the top
//       "enabled": true}]}                                         // optional; false: kept, not cut
namespace c2d::padlink {

// Limits on what a sender can make us build.
constexpr int kMaxShapes = 2000;
constexpr int kMaxPoints = 200000;   // over all paths
constexpr int kMaxToolpaths = 100;

// Empty when `design` is a well-formed version-1 design, else why not.
QString check(const QJsonObject &design);

// A design time further ahead of this computer's clock than this is a clock
// gone wrong, not an edit: taken in, it would win every sync until then, and
// edits after it would soon run into check()'s limit and freeze the design.
constexpr double kMaxAheadMs = 366.0 * 24 * 3600 * 1000;
bool plausibleStamp(double modified, double now);
// The `modified` for an edit made at `now` to a copy stamped `was`: after it,
// even with this clock behind, but never past a plausible time.
double nextStamp(double was, double now);

// Write `design` as a new .c2d at `c2dPath`. `notes` gets anything that was
// left out (a toolpath without shapes, ...). False with `error` set when the
// design is malformed, a tool is unknown or has no feeds for the material, or
// the file cannot be written.
bool buildDocument(const QJsonObject &design, const QString &c2dPath,
                   QStringList *notes, QString *error);

// `design` written to `c2dPath` as buildDocument does, but built beside it
// and swapped in, so a failure leaves the file that is there as it was.
bool rebuildDocument(const QJsonObject &design, const QString &c2dPath, QString *error);

// The other way: a .c2d made from a pad design and edited here, as a pad
// design again, so the edit reaches the pads. `base` is the pad design it was
// made from; its id and name are kept, `modified` is left for the caller.
// What the pad cannot hold goes in `lost` (curves become straight segments,
// V-carves are left out, feeds or tabs set here, ...): with nothing lost, the
// design builds back into the same document, so a pad edit can replace it.
// Empty with `error` set when the file cannot be read.
QJsonObject designFromDocument(const QString &c2dPath, const QJsonObject &base,
                               QStringList *lost, QString *error = nullptr);

// Which .c2d files were made from which pad design, and whether the file
// still holds only what the pad can (so a pad edit may rebuild it). Kept in
// `dir`/.links.json, beside the Store's designs.
class Links
{
public:
    explicit Links(const QString &dir);
    // False when .links.json is there but could not be read (too big, not
    // JSON): then it is never written over, and set() and remove() refuse.
    bool ok() const { return m_error.isEmpty(); }
    QString error() const { return m_error; }
    QString idFor(const QString &c2dPath) const;
    QStringList pathsFor(const QString &id) const;
    bool padOnly(const QString &c2dPath) const;
    // The design's `modified` the file holds (built from it, or saved as it);
    // 0 when not known (links from before 1.1.22).
    double modified(const QString &c2dPath) const;
    // The file's size or time is not what it was when it was last written
    // here (opened and saved in Carbide Create, say). False when not known.
    bool changedOutside(const QString &c2dPath) const;
    // Also notes the file's size and time as they are now, so call it once
    // the file is written. False with error() set when the links cannot be kept.
    bool set(const QString &c2dPath, const QString &id, bool padOnly, double modified = 0);
    bool remove(const QString &c2dPath);
    // The files linked to `id` that hold its copy stamped `modified`, linked
    // to `toId` instead and otherwise left as they were (Store::put moved that
    // copy there). How many there were; -1 when the links cannot be kept.
    int moveTo(const QString &id, double modified, const QString &toId);

private:
    bool write();
    QString m_file;
    QString m_error;
    QJsonObject m_links;   // canonical path -> {id, padOnly, modified, size, mtime}
};

// Rebuild from `design` every file linked to it that a pad edit may replace
// (padOnly, not changed outside phobiCCCP, there), but `except` and those
// `leaveAlone` says to (an open document with changes), and note each as
// holding `design`'s copy. So a save of one linked file reaches the others
// as a pad edit does. The files rebuilt.
QStringList followDesign(Links &links, const QJsonObject &design, const QString &except,
                         const std::function<bool(const QString &)> &leaveAlone = {});

// The embedded tool library as the pad needs it: {"materials": [{id, name}],
// "tools": [{number, name, kind, diameter, angle, flutes, materials: [ids
// with feeds]}]}.
QJsonObject toolsJson();

// Versions. Pads and phobiCCCPs pass designs around without ever talking to
// every other one, so "the newer `modified` wins" alone would let one edit
// replace another made meanwhile elsewhere. Every edit therefore also notes,
// in "vv", the time of the latest edit each pad or phobiCCCP (by a random id
// of its own) made to the design: a copy whose vv is at least another's in
// every entry was made from it, and replaces it; two copies neither of which
// was made from the other are both kept (Store::put). A vv is only believed
// when its largest time is the copy's `modified`: an older pad or phobiCCCP
// keeps the vv it got but stamps a new time, and then the older rules apply
// (`base`, newer wins).
enum class Order { Same, After, Before, Concurrent, Unknown };
// How copy `a` stands to copy `b` of the same design: the same copy, made
// from it (After), what it was made from (Before), neither, or Unknown (one
// has no believable vv).
Order order(const QJsonObject &a, const QJsonObject &b);
// The design's vv, or empty when it has none that can be believed.
QJsonObject versionVector(const QJsonObject &design);
constexpr int kMaxReplicas = 64;

// Designs kept on this computer so every pad (and every phobiCCCP a pad
// talks to) ends up with the same set: one <id>.json per design in `dir`.
// A copy made from the one kept replaces it; a deletion is a design with
// "deleted": true.
class Store
{
public:
    explicit Store(const QString &dir) : m_dir(dir) {}
    QString dir() const { return m_dir; }
    // [{id, name, modified, deleted, vv}] for every design kept.
    QJsonArray list() const;
    QJsonObject get(const QString &id) const;   // empty when unknown
    // What a put did besides keeping the design or not.
    struct PutOutcome {
        // The two copies were edited apart (after the sender's `base`, or by
        // their vvs), so one of them was kept as this new design.
        QString forked;
        // It was the copy kept here that went to `forked`: the one put is
        // kept in its place (with vvs, the later of the two edits is).
        bool moved = false;
        // A deletion that lost to an edit made meanwhile: refused, so the
        // edit stays.
        bool conflict = false;
    };
    // Keep `design` unless the stored one is the same or made from it. True
    // when it was written (as itself or as a fork); `error` says why not when
    // it was refused as malformed. `now` is for tests.
    // When the copy kept here cannot be read (made by a newer phobiCCCP, say),
    // the put is refused with `error` set rather than written over it.
    // A fork's id comes from `forkIdFor`, so the same edit put again is
    // answered with the fork already made; with vvs it depends only on the
    // copy forked, so every phobiCCCP forks the same two copies the same way,
    // and the forks of one conflict do not multiply as pads carry it around.
    bool put(const QJsonObject &design, QString *error = nullptr, PutOutcome *outcome = nullptr,
             double now = 0);
    static QString forkIdFor(const QString &id, double modified, double base);
    static QString forkIdFor(const QJsonObject &copy);
    // `design` as edited here at `modified`: its vv notes this computer's edit
    // (a design new here, with no id of its own yet in the store, starts a vv).
    QJsonObject edited(QJsonObject design, double modified) const;
    // This computer's own id in vvs, made once and kept in `dir`.
    QString replicaId() const;

private:
    QString path(const QString &id) const;
    bool write(const QJsonObject &design, QString *error);
    QString m_dir;
};

bool validId(const QString &id);

} // namespace c2d::padlink
