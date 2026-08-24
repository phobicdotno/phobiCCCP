#pragma once
#include <QJsonObject>
#include <QPainterPath>
#include <QString>
#include <QVector>

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

    // Factories: build new elements in the exact JSON shape CC writes (verified
    // against CC-853 specimens), so files saved with them open in Carbide Create.
    // Coordinates are CC space: mm, Y-up, origin bottom-left. `layer` is the
    // embedded layer object (copy it from an existing element, or defaultLayer()).
    static Element makeCircle(QPointF center, double radius, const QJsonObject &layer);
    static Element makeRectangle(QPointF center, double width, double height,
                                 const QJsonObject &layer);
    static Element makePolygon(QPointF center, double radius, int numSides,
                               const QJsonObject &layer);
    // Straight-segment path from clicked vertices (absolute mm coordinates,
    // position [0,0] as CC writes paths). `closed` appends the return-to-start
    // and close rows.
    static Element makePath(const QVector<QPointF> &vertices, bool closed,
                            const QJsonObject &layer);

    // Move the element by (dx, dy) mm: shifts position/center (or the text
    // transform's translation) in `raw` and rebuilds painterPath.
    void translate(double dx, double dy);

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
