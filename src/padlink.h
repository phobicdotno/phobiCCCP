#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

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
//       "shapes": ["s1"], "tool": 201, "depth": 6}]}               // tool: library number; depth below the top
namespace c2d::padlink {

// Limits on what a sender can make us build.
constexpr int kMaxShapes = 2000;
constexpr int kMaxPoints = 200000;   // over all paths
constexpr int kMaxToolpaths = 100;

// Empty when `design` is a well-formed version-1 design, else why not.
QString check(const QJsonObject &design);

// Write `design` as a new .c2d at `c2dPath`. `notes` gets anything that was
// left out (a toolpath without shapes, ...). False with `error` set when the
// design is malformed, a tool is unknown or has no feeds for the material, or
// the file cannot be written.
bool buildDocument(const QJsonObject &design, const QString &c2dPath,
                   QStringList *notes, QString *error);

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
    QString idFor(const QString &c2dPath) const;
    QStringList pathsFor(const QString &id) const;
    bool padOnly(const QString &c2dPath) const;
    void set(const QString &c2dPath, const QString &id, bool padOnly);
    void remove(const QString &c2dPath);

private:
    void write() const;
    QString m_file;
    QJsonObject m_links;   // canonical path -> {id, padOnly}
};

// The embedded tool library as the pad needs it: {"materials": [{id, name}],
// "tools": [{number, name, kind, diameter, angle, flutes, materials: [ids
// with feeds]}]}.
QJsonObject toolsJson();

// Designs kept on this computer so every pad (and every phobiCCCP a pad
// talks to) ends up with the same set: one <id>.json per design in `dir`.
// The newer `modified` wins; a deletion is a design with "deleted": true.
class Store
{
public:
    explicit Store(const QString &dir) : m_dir(dir) {}
    QString dir() const { return m_dir; }
    // [{id, name, modified, deleted}] for every design kept.
    QJsonArray list() const;
    QJsonObject get(const QString &id) const;   // empty when unknown
    // Keep `design` unless the stored one is as new or newer. True when it
    // was written; `error` says why not when it was refused as malformed.
    bool put(const QJsonObject &design, QString *error = nullptr);

private:
    QString path(const QString &id) const;
    QString m_dir;
};

bool validId(const QString &id);

} // namespace c2d::padlink
