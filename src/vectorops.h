#pragma once
#include "element.h"
#include <QJsonObject>
#include <QPainterPath>
#include <QPolygonF>
#include <QRectF>
#include <QVector>

// Vector operations on elements — Carbide Create's Booleans, Offsets and
// Alignment — as pure geometry (no widgets), so the app and the headless
// tests share one implementation. Booleans and offsets run through Clipper2
// on the elements' painterPath flattened at 0.005 mm; results come back as
// closed rings that the callers turn into `path` elements (Element::makePath),
// one element per ring, so holes are separate nested closed paths exactly as
// CC treats them (even-odd).
namespace c2d {
namespace vec {

enum class BoolOp { Union, Subtract, Intersect };

// Fill-region booleans. Each input path is its own even-odd region (so a
// text glyph keeps its counters); across inputs the fills combine non-zero.
// Subtract keeps `subjects` minus every `clips`; Union/Intersect ignore
// `clips` and combine all `subjects` (Intersect: the common area of all of
// them). Open (unfilled) paths are ignored — they have no area. Returns
// closed rings (first point not repeated) in mm.
QVector<QPolygonF> booleanRings(const QVector<QPainterPath> &subjects,
                                const QVector<QPainterPath> &clips, BoolOp op,
                                double tol = 0.005);

// Offset: `closed` paths are treated as one even-odd region (nested vectors
// are holes, as CC's toolpaths see them) and inflated by `delta` (positive =
// outward, negative = inward, round joins); each `open` path becomes the
// outline of a stroke of width 2*|delta| (round ends).
QVector<QPolygonF> offsetRings(const QVector<QPainterPath> &closed,
                               const QVector<QPainterPath> &open, double delta,
                               double tol = 0.005);

// Signed-area helper (positive = CCW) for tests and ring classification.
double ringArea(const QPolygonF &ring);

// A path element is closed when one of its rows is a close row (type 4);
// every other geometry type is closed by construction.
bool isClosed(const Element &e);

// Rings -> closed `path` elements on `layer` (CC's exact JSON shape).
QVector<Element> elementsFromRings(const QVector<QPolygonF> &rings,
                                   const QJsonObject &layer);

// Boolean over elements: `inputs[0]` is the subject for Subtract, the rest
// are subtracted from it. The result elements land on inputs[0]'s layer.
QVector<Element> booleanElements(const QVector<Element> &inputs, BoolOp op,
                                 double tol = 0.005);

// Offset over elements (see offsetRings); layer taken from inputs[0].
QVector<Element> offsetElements(const QVector<Element> &inputs, double delta,
                                double tol = 0.005);

// ---- alignment (pure arithmetic on bounding boxes) -----------------------
enum class Align { Left, HCenter, Right, Top, VCenter, Bottom };
enum class Center { Horizontal, Vertical, Both };
enum class Axis { Horizontal, Vertical };

// (dx, dy) per box that aligns it to `ref` (the selection's bounding box in
// the app; CC's Y-up: Top = max y).
QVector<QPointF> alignDeltas(const QVector<QRectF> &boxes, Align mode, const QRectF &ref);
// Move the group (as one rigid block) so its bounding box is centered on `stock`.
QVector<QPointF> centerDeltas(const QVector<QRectF> &boxes, Center mode, const QRectF &stock);
// Evenly spaced centers between the two outermost boxes along `axis`
// (needs >= 3 boxes, otherwise all-zero deltas).
QVector<QPointF> distributeDeltas(const QVector<QRectF> &boxes, Axis axis);

} // namespace vec
} // namespace c2d
