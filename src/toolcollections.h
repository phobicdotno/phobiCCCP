#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <QVector>

namespace c2d {

// Named sets of bits from the tool library ("My Tools", "Unused", ...), kept
// by tool number in the order they were added. Stored as JSON, both in the
// app settings and in files the user saves and loads:
//   {"collections": [{"name": "My Tools", "tools": [201, 102]}, ...]}
struct ToolCollection {
    QString name;
    QList<int> tools;
};

class ToolCollections
{
public:
    // The collections a new install starts with: My Tools and Unused, empty.
    static ToolCollections defaults();

    const QVector<ToolCollection> &all() const { return m_list; }
    int indexOf(const QString &name) const;
    const ToolCollection *byName(const QString &name) const;

    bool add(const QString &name);                       // false: empty or taken
    bool rename(const QString &from, const QString &to); // false: missing or taken
    void remove(const QString &name);
    void addTool(const QString &name, int number);       // no-op if already in
    void removeTool(const QString &name, int number);

    // Collections from `other` replace those with the same name; the rest
    // are added after the existing ones.
    void merge(const ToolCollections &other);

    QByteArray toJson() const;
    static bool fromJson(const QByteArray &json, ToolCollections *out, QString *err = nullptr);
    bool saveFile(const QString &path, QString *err = nullptr) const;
    static bool loadFile(const QString &path, ToolCollections *out, QString *err = nullptr);

    // The app's own copy, in QSettings (defaults() when nothing is stored).
    static ToolCollections loadSettings();
    void saveSettings() const;

private:
    QVector<ToolCollection> m_list;
};

} // namespace c2d
