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

    QString filePath() const { return m_path; }
    const QHash<QString, QString> &params() const { return m_params; }
    const QVector<Element> &elements() const { return m_elements; }
    const QVector<Toolpath> &toolpaths() const { return m_toolpaths; }

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
