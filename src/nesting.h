#pragma once
// Nesting: pack many parts on one sheet of stock to save material.
// Bottom-left fill: the biggest part goes first, each part (and each allowed
// quarter turn of it) is tried on a grid of positions, lowest row first, then
// left to right, and goes where it first fits: inside the stock less the
// margin, and its outline grown by half the spacing clear of every part
// already placed (grown the same way).

#include <QPolygonF>
#include <QRectF>
#include <QTransform>
#include <QVector>

namespace c2d::nest {

struct Options {
    double spacing = 6.35;      // gap between parts, mm
    double margin = 5.0;        // gap from the stock edge to any part, mm
    bool allowRotation = true;  // also try 90, 180 and 270 degrees
    double maxStep = 5.0;       // grid step, at most this (and a quarter of the part)
};

struct Placement {
    bool placed = false;
    double rotation = 0;   // degrees, counter-clockwise: 0, 90, 180 or 270
    QTransform transform;  // original outline -> placed outline (identity if not placed)
};

// One placement per outline, in input order. Outlines are closed polygons in
// mm (Y up); `stock` is the sheet (usually 0,0 to width,height).
// `obstacles` are shapes that stay where they are (other work already on the
// stock): parts keep the spacing from them too.
QVector<Placement> nestParts(const QVector<QPolygonF> &outlines, const QRectF &stock,
                             const Options &opt,
                             const QVector<QPolygonF> &obstacles = {});

} // namespace c2d::nest
