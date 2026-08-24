#pragma once
#include "element.h"
#include <QHash>
#include <QString>
#include <QVector>

// Reads a modern (v7/v8 SQLite) Carbide Create .c2d container: opens the
// database, pulls `params` and the `items` rows, decompresses each element
// payload and parses it into an Element. Toolpath rows are captured as raw
// JSON for now (parameter editing is a later tier).
namespace c2d {

struct Toolpath {
    QString uuid;
    QString type;
    QJsonObject json;   // full decoded J1 payload
};

class Document
{
public:
    bool load(const QString &path, QString *error = nullptr);

    // Save by cloning the currently-loaded file and rewriting its element rows
    // from the in-memory elements (the proven, round-trip-verified recipe:
    // DELETE elements, re-INSERT zlib(J1 JSON) with sz = uncompressed length,
    // blank the stale render/g-code blobs so CC regenerates them). Toolpaths,
    // layer, model and params are preserved. Requires a file previously load()ed.
    bool save(const QString &destPath, QString *error = nullptr);

    QString filePath() const { return m_path; }
    const QHash<QString, QString> &params() const { return m_params; }
    const QVector<Element> &elements() const { return m_elements; }
    const QVector<Toolpath> &toolpaths() const { return m_toolpaths; }

    // Editing: mutable element access (geometry edits), append, remove.
    QVector<Element> &elements() { return m_elements; }
    void addElement(const Element &e) { m_elements.append(e); }
    void removeElement(int index) { m_elements.remove(index); }

    double boardWidth()  const { return m_params.value("width", "0").toDouble(); }
    double boardHeight() const { return m_params.value("height", "0").toDouble(); }
    bool displayMm()     const { return m_params.value("display_mm") == "1"; }

private:
    QString m_path;
    QHash<QString, QString> m_params;
    QVector<Element> m_elements;
    QVector<Toolpath> m_toolpaths;
};

} // namespace c2d
