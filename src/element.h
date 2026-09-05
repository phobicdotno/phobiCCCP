#pragma once
#include <QFont>
#include <QHash>
#include <QJsonObject>
#include <QPainterPath>
#include <QString>
#include <QVector>

// A drawing element (items.type = 'element'). Parsed from the decompressed J1
// JSON payload into a Qt-native QPainterPath in CC's coordinate space
// (millimetres, Y-up, origin at the board's bottom-left corner).
namespace c2d {

// ---- editable node model ---------------------------------------------------
// One anchor of a path with its two bezier handles, all in absolute mm. A
// handle sitting exactly on its anchor means "no curvature on that side": a
// segment whose two handles both sit on their anchors is a straight line
// (CC point_type 1), anything else is a cubic (point_type 3).
struct PathNode {
    enum Kind { Corner = 0, Smooth = 1, Symmetric = 2 };
    QPointF p;              // anchor
    QPointF in;             // handle of the segment arriving at p (cp2 of that row)
    QPointF out;            // handle of the segment leaving p (cp1 of the next row)
    Kind kind = Corner;

    bool hasIn() const;     // handle not on the anchor
    bool hasOut() const;
};

struct SubPath {
    QVector<PathNode> nodes;
    bool closed = false;

    // Number of drawable segments (n-1 open, n closed).
    int segmentCount() const;
    // Endpoints of segment i (node i -> node i+1, wrapping when closed).
    void segment(int i, const PathNode **a, const PathNode **b) const;
    QPointF pointAt(int seg, double t) const;
    // Closest segment/parameter to `pt`; returns the distance.
    double closest(const QPointF &pt, int *seg, double *t) const;

    // Node editing. insertNode splits segment `seg` at parameter t (de
    // Casteljau for curves, lerp for lines) — the curve shape is preserved.
    // removeNode drops a node and lets the neighbouring handles bridge the gap.
    // setKind re-aligns the handles: Smooth keeps both lengths on a common
    // tangent, Symmetric also equalizes them, Corner leaves them free.
    // retract() pulls both handles onto the anchor (straight lines).
    int insertNode(int seg, double t);
    bool removeNode(int i);
    void setKind(int i, PathNode::Kind k);
    void retract(int i);
    // Move a handle to `to`, honouring the node kind (Smooth/Symmetric drag the
    // opposite handle along) unless `breakSymmetry`, which also demotes the
    // node to Corner.
    void moveHandle(int i, bool outHandle, const QPointF &to, bool breakSymmetry);
};

struct PathModel {
    QVector<SubPath> subs;
    bool isEmpty() const { return subs.isEmpty(); }
    QPainterPath painterPath() const;
};

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
                               const QJsonObject &layer, double rotationDeg = 0);
    // Straight-segment path from clicked vertices (absolute mm coordinates,
    // position [0,0] as CC writes paths). `closed` appends the return-to-start
    // and close rows.
    static Element makePath(const QVector<QPointF> &vertices, bool closed,
                            const QJsonObject &layer);
    // Text at `pos` (baseline-left, CC mm): glyph outlines are flattened into
    // CC's `rendered` contour array; font/qtfont/text keys are kept so CC can
    // re-render if the user edits it there. `heightMm` sets the glyph size.
    static Element makeText(const QString &text, QPointF pos, double heightMm,
                            const QString &family, const QJsonObject &layer);

    // Bezier path in CC's schema from the node model: per anchor, cp1 = the
    // previous node's out-handle, cp2 = this node's in-handle; straight
    // segments are written as point_type 1 rows exactly like makePath.
    static Element makeBezierPath(const PathModel &model, const QJsonObject &layer);
    // Same encoding, but keeping `src`'s identity (id, layer, group_id, tabs).
    // The result is always a `path` element: a rectangle/circle/polygon whose
    // nodes were edited is no longer parametric.
    static Element withPathModel(const Element &src, const PathModel &model);
    // Decode the point model (any point-model geometry type) into absolute-mm
    // nodes. Text yields an empty model — convert it with toPaths() first.
    static PathModel pathModel(const Element &e);
    // "Convert to path": rectangle/circle/polygon become one path element that
    // keeps the id; text becomes one closed path per glyph contour (the first
    // keeps the id). Paths come back unchanged.
    static QVector<Element> toPaths(const Element &src);

    // Text: re-render the glyph outlines (`rendered`) from the element's own
    // keys — text, font/qtfont, font_height (= ascent in mm), spacing, and the
    // arc_* keys (glyphs placed along a circle of arc_radius about arc_center,
    // baseline on the arc, centred on the top or bottom point turned by
    // arc_angle_offset degrees). `changes` may carry any of those keys plus
    // the pseudo-keys "family", "bold", "italic" that rewrite font/qtfont.
    static Element regenText(const Element &src, const QJsonObject &changes);
    static void renderText(QJsonObject &textObj);
    // The font a text element was rendered with (from qtfont, else `font`).
    static QFont textFont(const QJsonObject &textObj);

    // Move the element by (dx, dy) mm: shifts position/center (or the text
    // transform's translation) in `raw` and rebuilds painterPath.
    void translate(double dx, double dy);

    // Rebuild a closed shape's geometry from edited parameters, keeping its
    // identity (id, layer, group_id, tabs). Recognized keys: cx, cy, radius,
    // width, height, num_sides — unknown/irrelevant keys are ignored. Returns
    // the source unchanged for geometry types without parametric regen
    // (path, text — reposition those with translate()).
    static Element regen(const Element &src, const QHash<QString, double> &p);

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
