#pragma once
#include "element.h"
#include <QRectF>
#include <QStringList>
#include <QTransform>

// "Rotate 90°": the whole design turned a quarter turn clockwise. The stock
// turns with it (width and height swap, in the window), so the design keeps
// its place on the stock.
// Everything that is drawn on the stock turns with it: every vector (same
// ids, same node order, a rectangle stays a rectangle), what hangs on them
// (toolpaths point at vectors by id; their direction fields turn too), the
// constraints and dimensions, the 3D model's images, meshes and textures,
// and the tracing background. The design keeps its bottom-left corner
// where it was; with a stock, the turn is about the stock's corner.
//
// Pure functions over plain data, so the window and the tests share them.
namespace c2d {

class Document;
struct BackgroundImage;
struct Model3D;

namespace jobrotate {

// What the design covers: every vector, and every placed 3D image or mesh.
// Null when there is nothing to turn.
QRectF designBounds(const Document &doc, const Model3D *model = nullptr);

// A quarter turn clockwise that keeps `bounds`' bottom-left corner where it
// is: (x, y) -> (y + x0 - y0, x0 + y0 + w - x).
QTransform quarterTurn(const QRectF &bounds);

// One vector turned by `q` (a transform from quarterTurn). Its nodes keep
// their order, so a contour starts and lays its tabs where it did; a
// rectangle swaps width and height; a regular polygon's rotation turns.
Element turnElement(const Element &e, const QTransform &q);

// A direction in degrees (Y-up, counter-clockwise) a quarter turn clockwise,
// kept in (-180, 180] so four turns give it back.
double turnAngle(double deg);

struct Report {
    int vectors = 0;
    int toolpaths = 0;        // toolpaths whose direction fields turned
    int remeasured = 0;       // plain-number dimensions from the origin that took their new value
    QStringList notHeld;      // uids of constraints tied to the origin that no longer hold
};

// The document's half: vectors, the toolpaths' direction fields (hatch,
// texture, keyhole slot, pocket and 3D raster angles, their expressions
// too), and the constraints, which follow their geometry without a solve
// (nothing moves but by `q`). A constraint tied to the board's origin
// cannot hold after the turn: a dimension of a plain number nothing else
// uses takes its new value, anything else is reported in notHeld.
Report turnDocument(Document &doc, const QTransform &q);

// The 3D model's images, meshes and textures turned with the vectors
// (components made from vectors follow them by themselves).
void turnModel(Model3D &model, const QTransform &q);

// The tracing background turned with the vectors.
void turnBackground(BackgroundImage &bg, const QTransform &q);

} // namespace jobrotate
} // namespace c2d
