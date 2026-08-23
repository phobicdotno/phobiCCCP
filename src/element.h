#pragma once
#include <QJsonObject>
#include <QPainterPath>
#include <QString>

// A drawing element (items.type = 'element'). Parsed from the decompressed J1
// JSON payload into a Qt-native QPainterPath in CC's coordinate space
// (millimetres, Y-up, origin at the board's bottom-left corner).
namespace c2d {

class Element
{
public:
    enum Behavior { Path = 0, Rectangle = 1, RegularPolygon = 2, Circle = 3, Text = 99 };

    // Build from a parsed element JSON object. Returns an Element whose
    // painterPath() is ready to draw; unknown geometryTypes yield an empty path.
    static Element fromJson(const QJsonObject &obj);

    QString id;
    QString geometryType;
    Behavior behavior = Path;
    QPainterPath painterPath;   // absolute mm, Y-up
    QJsonObject raw;            // the original J1 payload, kept for lossless save

    // Serialize back to the J1 JSON payload (currently returns `raw` verbatim;
    // geometry edits will mutate `raw` before this is called).
    QByteArray toJson() const;

private:
    static QPainterPath buildPointModel(const QJsonObject &obj);
    static QPainterPath buildText(const QJsonObject &obj);
};

} // namespace c2d
