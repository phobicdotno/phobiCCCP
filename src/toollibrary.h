#pragma once
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

// Tool library / speeds & feeds: the Carbide 3D cutter catalogue with
// per-material defaults, loaded from the embedded data/tool-library.json
// (or any file with the same schema). applyToToolpath() writes a chosen tool
// into a Carbide Create toolpath payload using CC's own key names — the
// embedded `tool` object (diameter, number, angle, type, flutes, ...), the
// `speeds` object (feedrate, plungerate, rpm) and the per-type depth/stepover
// keys — so the file still opens in CC afterwards.
namespace c2d {

struct ToolFeeds {
    double rpm = 0, feed = 0, plunge = 0;   // rpm, mm/min, mm/min
    double stepdown = 0;                    // mm per pass
    double stepoverPct = 0;                 // % of tool diameter
    bool guess = false;                     // not a remembered CC default
    QString note;
    bool valid() const { return rpm > 0 && feed > 0; }
};

struct LibraryTool {
    int number = 0;
    QString model, name, kind, url, notes, vendor;   // kind: endmill/ball/vbit/engraver
    double diameter = 0, angle = 0, cornerRadius = 0, length = 0, shank = 0;
    int flutes = 0;
    bool uncertain = false;                 // catalogue number/spec unverified
    QHash<QString, ToolFeeds> feeds;        // material id -> feeds

    int ccType() const;                     // CC tool.type: 0 flat, 1 ball, 2 V
    const ToolFeeds *feedsFor(const QString &materialId) const;
};

struct LibraryMaterial {
    QString id, name;
    QStringList ccNames;                    // CC `material` param spellings
};

class ToolLibrary
{
public:
    bool loadDefault(QString *err = nullptr);             // embedded resource
    bool loadFile(const QString &path, QString *err = nullptr);
    bool loadJson(const QByteArray &json, QString *err = nullptr);

    const QVector<LibraryTool> &tools() const { return m_tools; }
    const QVector<LibraryMaterial> &materials() const { return m_materials; }
    const LibraryTool *byNumber(int number) const;
    QString materialName(const QString &id) const;
    // Library material id for a document's `material` param ("Softwood" ->
    // "softwood"); empty when unknown.
    QString materialIdForCC(const QString &ccMaterial) const;
    QString source() const { return m_source; }

    // The CC `tool` object for this tool + feeds, starting from `base` so any
    // keys we do not know about survive. Written as a user (non read-only)
    // tool with the full definition embedded, like CC itself does.
    static QJsonObject toolObject(const LibraryTool &t, const ToolFeeds &f,
                                  const QJsonObject &base = QJsonObject());
    // Rewrite a toolpath payload's tool, speeds and (where the toolpath type
    // has them) stepdown / depth_per_pass / stepover for the material.
    static void applyToToolpath(QJsonObject &toolpath, const LibraryTool &t,
                                const ToolFeeds &f);

private:
    QVector<LibraryTool> m_tools;
    QVector<LibraryMaterial> m_materials;
    QString m_source;
};

} // namespace c2d
