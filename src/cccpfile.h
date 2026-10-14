#pragma once
#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

// Syntexophobic's .cccp project file, read as its final product only.
//
// A .cccp is a zip whose first entry is `mimetype` (stored, not compressed)
// holding "application/x-syntexophobic-project", followed by the whole
// project folder and its git history. Syntexophobic also writes the finished
// part into `final/` on every saved version:
//   final/part.stl     the part as it will be machined, mm, Z up
//   final/drawing.svg  every sketch, one <g id="name"> each, mm
//   final/info.json    {"name", "units", "size": [x, y, z], "version": {...}}
// phobiCCCP reads only those: the drawing as vectors and the part as an STL
// model component. The history and the design stay Syntexophobic's.
namespace c2d {

constexpr const char *kCccpMimetype = "application/x-syntexophobic-project";

struct CccpProduct {
    bool ok = false;
    QString error;            // set when !ok
    QString name;             // the part's name (info.json), else the file's
    QByteArray svg;           // final/drawing.svg, empty when the project has no sketches
    QByteArray stl;           // final/part.stl, empty when the part did not build
    QJsonObject info;         // final/info.json
};

bool isCccpFile(const QString &path);
CccpProduct readCccp(const QString &path);

// Building block (exposed for tests): every entry of a zip archive held in
// memory, name -> uncompressed bytes, for the names `want` accepts (all when
// empty). Stored and deflated entries only; returns false with `error` set
// on anything else, a truncated archive, or an entry over kMaxInflateBytes.
bool readZip(const QByteArray &zip, const QStringList &want, QHash<QString, QByteArray> *out,
             QString *error);

} // namespace c2d
