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
    // geometry edits mutate `raw` before this is called).
    QByteArray toJson() const;

    // Geometry edits. Each mutates `raw` (so save() round-trips the edit) and
    // rebuilds painterPath. Closed shapes move via center/position and scale
    // their center-relative point model; a path (position == [0,0]) edits its
    // absolute points; text edits its 3x3 transform.
    void translate(double dx, double dy);
    void scaleBy(double factor);            // about the shape's own center

    // Factories for new elements, emitting the full J1 schema CC writes
    // (layer/group_id/smooth/tabs included). center is absolute mm, Y-up.
    static Element makeCircle(QPointF center, double radius);
    static Element makeRectangle(QPointF center, double width, double height);

private:
    static QPainterPath buildPointModel(const QJsonObject &obj);
    static QPainterPath buildText(const QJsonObject &obj);
    void rebuildPath();
};

} // namespace c2d
