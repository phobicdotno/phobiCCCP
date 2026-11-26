#pragma once
#include "element.h"
#include "parametric.h"
#include <QHash>
#include <QSet>
#include <QString>
#include <QVariant>
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
    // Drop toolpath vectors whose shape no longer exists (done on load).
    void dropDanglingRefs();

    // Save by cloning the currently-loaded file and rewriting its element rows
    // from the in-memory elements (the proven, round-trip-verified recipe:
    // DELETE elements, re-INSERT zlib(J1 JSON) with sz = uncompressed length,
    // blank the stale render/g-code blobs so CC regenerates them). Toolpath
    // rows are rewritten the same way, in vector order, so deletions, moves
    // and new toolpaths persist; layer and model are preserved, and so are the
    // params, but for those changed in memory since the load (setParam), which
    // are written (params.num_toolpaths is kept in sync; site_order is the
    // application's setting and never written). Requires a file previously load()ed.
    bool save(const QString &destPath, QString *error = nullptr);
    // Writes a minimal, empty .c2d container (stock W x H x T mm) at path,
    // so a design can be started without an existing file to clone from.
    static bool createBlank(const QString &path, double width, double height,
                            double thickness, QString *error = nullptr);
    // Sets one param row in the container at path (replacing any with that key).
    static bool writeParam(const QString &path, const QString &key, const QString &value,
                           QString *error = nullptr);

private:
    // Params and toolpath groups from memory into a blank container at path,
    // for a save whose source file is gone.
    bool restoreInto(const QString &path, QString *error) const;

public:

    QString filePath() const { return m_path; }
    // Counts loads, so a caller that ran the event loop can tell the file was
    // reopened under it (a pad rebuilds and reloads the same path).
    quint64 loadCount() const { return m_loads; }
    // The container save() starts from (and then writes to). Only for a
    // copy that must be written from another container than its own file.
    void setFilePath(const QString &path) { m_path = path; }
    const QHash<QString, QString> &params() const { return m_params; }
    // Sets one param in memory (the stock turned by Rotate 90°; the tests,
    // without a round trip through a file). save() writes it to the file.
    void setParam(const QString &key, const QString &value) { m_params.insert(key, value); }
    const QVector<Element> &elements() const { return m_elements; }
    QVector<Element> &elementsRef() { return m_elements; }   // for editing
    const QVector<Toolpath> &toolpaths() const { return m_toolpaths; }
    QVector<Toolpath> &toolpathsRef() { return m_toolpaths; }    // for editing
    // Whole-state swaps for the parametric undo steps (canvas SnapshotCmd).
    void setElements(const QVector<Element> &els) { m_elements = els; }
    void setToolpaths(const QVector<Toolpath> &tps) { m_toolpaths = tps; }

    // User parameters, constraints and driving dimensions (parametric.h).
    // Loaded from and saved to params.phobic_parametric.
    const Parametric &parametric() const { return m_parametric; }
    Parametric &parametricRef() { return m_parametric; }
    void setParametric(const Parametric &p) { m_parametric = p; }

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
    // so new shapes land on the same layer (the first one that is shown and
    // not locked, so a new shape never lands out of sight or out of reach);
    // falls back to CC's DEFAULT layer.
    QJsonObject defaultLayer() const
    {
        for (const Element &e : m_elements) {
            const QJsonObject l = e.raw.value("layer").toObject();
            if (!l.isEmpty() && l.value("visible").toBool(true) && !l.value("locked").toBool(false))
                return l;
        }
        // Every layer is hidden or locked: a name none of them has, so the
        // new shape is not taken for one of those layers.
        QString name = QStringLiteral("DEFAULT");
        for (int n = 2;; ++n) {
            bool used = false;
            for (const Element &e : m_elements) {
                const QJsonObject l = e.raw.value("layer").toObject();
                if (l.value("uuid").toString().isEmpty() && l.value("name").toString() == name) {
                    used = true;
                    break;
                }
            }
            if (!used)
                break;
            name = QStringLiteral("DEFAULT %1").arg(n);
        }
        QJsonObject l;
        l.insert("blue", 0); l.insert("green", 0); l.insert("red", 0);
        l.insert("locked", false); l.insert("name", name);
        l.insert("uuid", QString()); l.insert("visible", true);
        return l;
    }

    double boardWidth()  const { return m_params.value("width", "0").toDouble(); }
    double boardHeight() const { return m_params.value("height", "0").toDouble(); }
    bool displayMm()     const { return m_params.value("display_mm") == "1"; }

private:
    QString m_path;
    quint64 m_loads = 0;
    QHash<QString, QString> m_params;
    QSet<QString> m_unreadableToolpaths;
    // The undecodable toolpath rows themselves, as read, with how many
    // readable toolpaths came before each: save() writes them back in the
    // same place in the machining order (CC orders by row id).
    struct KeptRow { QVariant uuid, name, version, sz, data; int position = 0; };
    QVector<KeptRow> m_keptToolpaths;
    QSet<QString> m_unreadableElements;   // same, for element rows
    QVector<Element> m_elements;
    QVector<Toolpath> m_toolpaths;
    QVector<ToolpathGroup> m_groups;
    Parametric m_parametric;
};

} // namespace c2d
