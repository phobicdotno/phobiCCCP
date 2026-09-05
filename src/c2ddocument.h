#pragma once
#include "element.h"
#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>

// Reads a modern (v7/v8 SQLite) Carbide Create .c2d container: opens the
// database, pulls `params` and the `items` rows, decompresses each element
// payload and parses it into an Element. Toolpath rows are kept as decoded
// JSON payloads in row order (= Carbide Create's machining order); any
// `type` is accepted, including phobiCCCP's own `engrave_toolpath`.
namespace c2d {

struct Toolpath {
    QString uuid;
    QString type;
    QJsonObject json;   // full decoded J1 payload
};

// A toolpath_group row: a pure folder (name/enabled/expanded); membership
// lives on each toolpath's `toolpath_group` key.
struct ToolpathGroup {
    QString uuid;
    QJsonObject json;
};

class Document
{
public:
    bool load(const QString &path, QString *error = nullptr);

    // Save by cloning the currently-loaded file and rewriting its element rows
    // from the in-memory elements (the proven, round-trip-verified recipe:
    // DELETE elements, re-INSERT zlib(J1 JSON) with sz = uncompressed length,
    // blank the stale render/g-code blobs so CC regenerates them). Toolpath
    // rows are rewritten the same way, in vector order, so deletions, moves
    // and new toolpaths persist; layer, model and params are preserved
    // (params.num_toolpaths is kept in sync). Requires a file previously load()ed.
    bool save(const QString &destPath, QString *error = nullptr);

    QString filePath() const { return m_path; }
    const QHash<QString, QString> &params() const { return m_params; }
    const QVector<Element> &elements() const { return m_elements; }
    QVector<Element> &elementsRef() { return m_elements; }   // for editing
    const QVector<Toolpath> &toolpaths() const { return m_toolpaths; }

    void addElement(const Element &e) { m_elements.append(e); }
    bool removeElementById(const QString &id)
    {
        for (int i = 0; i < m_elements.size(); ++i)
            if (m_elements.at(i).id == id) { m_elements.removeAt(i); return true; }
        return false;
    }
    Element *elementById(const QString &id)
    {
        for (Element &e : m_elements)
            if (e.id == id) return &e;
        return nullptr;
    }
    bool replaceElement(const Element &e)
    {
        for (Element &x : m_elements)
            if (x.id == e.id) { x = e; return true; }
        return false;
    }

    // Toolpath lifecycle. The vector order IS the machining order and is
    // what save() writes back. All of these are in-memory only until save().
    void addToolpath(const Toolpath &t) { m_toolpaths.append(t); }
    // uuids of toolpath rows whose payload this build could not decode. They
    // stay out of m_toolpaths but must survive a save: rewriting the toolpath
    // rows must not delete work we merely failed to understand.
    const QSet<QString> &unreadableToolpaths() const { return m_unreadableToolpaths; }
    void insertToolpath(int index, const Toolpath &t)
    {
        m_toolpaths.insert(qBound(0, index, int(m_toolpaths.size())), t);
    }
    bool removeToolpath(const QString &uuid)
    {
        for (int i = 0; i < m_toolpaths.size(); ++i)
            if (m_toolpaths.at(i).uuid == uuid) { m_toolpaths.removeAt(i); return true; }
        return false;
    }
    int toolpathIndex(const QString &uuid) const
    {
        for (int i = 0; i < m_toolpaths.size(); ++i)
            if (m_toolpaths.at(i).uuid == uuid) return i;
        return -1;
    }
    // Move the toolpath so it sits at `index` afterwards (clamped).
    bool moveToolpath(const QString &uuid, int index)
    {
        const int from = toolpathIndex(uuid);
        if (from < 0)
            return false;
        const int to = qBound(0, index, int(m_toolpaths.size()) - 1);
        if (to != from)
            m_toolpaths.move(from, to);
        return true;
    }
    const QVector<ToolpathGroup> &toolpathGroups() const { return m_groups; }
    // Group uuid for a new toolpath: the file's first group row, else the
    // group of an existing toolpath, else empty (CC's "no group").
    QString defaultToolpathGroup() const
    {
        if (!m_groups.isEmpty())
            return m_groups.first().uuid;
        for (const Toolpath &t : m_toolpaths) {
            const QString g = t.json.value("toolpath_group").toString();
            if (!g.isEmpty())
                return g;
        }
        return QString();
    }
    Toolpath *toolpathByUuid(const QString &uuid)
    {
        for (Toolpath &t : m_toolpaths)
            if (t.uuid == uuid) return &t;
        return nullptr;
    }
    bool replaceToolpath(const Toolpath &t)
    {
        for (Toolpath &x : m_toolpaths)
            if (x.uuid == t.uuid) { x = t; return true; }
        return false;
    }

    // Layer object for newly created elements: copied from an existing element
    // so new shapes land on the same layer; falls back to CC's DEFAULT layer.
    QJsonObject defaultLayer() const
    {
        if (!m_elements.isEmpty()) {
            const QJsonObject l = m_elements.first().raw.value("layer").toObject();
            if (!l.isEmpty())
                return l;
        }
        QJsonObject l;
        l.insert("blue", 0); l.insert("green", 0); l.insert("red", 0);
        l.insert("locked", false); l.insert("name", QStringLiteral("DEFAULT"));
        l.insert("uuid", QString()); l.insert("visible", true);
        return l;
    }

    double boardWidth()  const { return m_params.value("width", "0").toDouble(); }
    double boardHeight() const { return m_params.value("height", "0").toDouble(); }
    bool displayMm()     const { return m_params.value("display_mm") == "1"; }

private:
    QString m_path;
    QHash<QString, QString> m_params;
    QSet<QString> m_unreadableToolpaths;
    QVector<Element> m_elements;
    QVector<Toolpath> m_toolpaths;
    QVector<ToolpathGroup> m_groups;
};

} // namespace c2d
