#pragma once
#include "element.h"
#include <QPointF>
#include <QVector>

// Fusion 360's sketch Create shapes that Carbide Create has no element type
// for — ellipse, slot, 3-point arc — as node models (absolute mm, Y-up) that
// Element::makeBezierPath turns into `path` elements, plus the 3-point
// circle's construction. Pure geometry: the canvas tools and the tests share
// it. Circular arcs are cubics of at most 90° each (radial error < 0.03%).
namespace c2d {
namespace sketch {

// An open subpath along the circle (c, r) from angle a0 (radians, from +X,
// counter-clockwise) turning by `sweep` (negative = clockwise).
SubPath arc(QPointF c, double r, double a0, double sweep);

// Closed ellipse with semi-axes rx, ry, its rx axis turned by rotDeg.
PathModel ellipse(QPointF c, double rx, double ry, double rotDeg = 0);

// Closed stadium: the two arc centers c1, c2 and the overall width (twice
// the end radius). Coincident centers give a circle; width <= 0 nothing.
PathModel slot(QPointF c1, QPointF c2, double width);

// The circle through three points; false when they are (nearly) collinear.
bool circleThrough(QPointF a, QPointF b, QPointF c, QPointF *center, double *radius);

// Open arc from `start` to `end` passing through `through` (Fusion's
// 3-point arc). Collinear points give the straight line start -> end.
PathModel arc3(QPointF start, QPointF end, QPointF through);

// ---- trim / break / extend ---------------------------------------------------
// Fusion's sketch Trim, Break and Extend on node models. A position along a
// subpath is a global parameter g = segment index + t (0..segmentCount).
// The curves that can bound a piece are every subpath of `m` itself
// (self-crossings included) plus every subpath of `others` (the rest of the
// drawing); crossings are found on a fine polyline and polished by Newton
// steps on the true cubics, so cut points sit on both curves.

// The subpath and position on it nearest to `q`; false when nothing of
// `m` is within `tol`.
bool pick(const PathModel &m, QPointF q, double tol, int *sub, double *g);

// Every crossing of subpath `sub` with the bounding curves, as sorted,
// de-duplicated global parameters (an open subpath's own ends excluded).
QVector<double> crossings(const PathModel &m, int sub, const QVector<PathModel> &others);

// The piece of subpath `sub` around position g between the nearest
// crossings on either side, as [from, to] (to > from; on a closed subpath
// `to` may run past the end and wraps). A closed subpath with fewer than
// two crossings is one piece: the whole loop (from = to - segmentCount).
void pieceAround(const PathModel &m, int sub, double g, const QVector<double> &cuts,
                 double *from, double *to);

// The part of a subpath between two global parameters, as an open subpath
// (on a closed one, `to` may exceed segmentCount to wrap past the start).
SubPath subRange(const SubPath &s, double from, double to);

// Trim: remove the piece under `q`. Returns the model that remains (the
// subpath split into what is left of it, other subpaths untouched); it is
// empty when the element had nothing else. *hit is false when nothing of
// `m` is within `tol` of q (and `m` comes back unchanged).
PathModel trim(const PathModel &m, QPointF q, double tol, const QVector<PathModel> &others,
               bool *hit);

// Break: split the subpath under `q` at the crossings either side of it.
// Returns the pieces: [0] is `m` with that subpath replaced by the piece
// under q, the rest one model per other piece. Empty when nothing is hit or
// there is nothing to split at.
QVector<PathModel> breakAt(const PathModel &m, QPointF q, double tol,
                           const QVector<PathModel> &others);

// Extend: lengthen the open end of a subpath nearest to `q` straight along
// its end tangent until it meets a bounding curve (a straight end segment
// grows, a curved one gets a straight continuation). False, and `m`
// untouched, when the end runs into nothing.
bool extend(PathModel &m, QPointF q, double tol, const QVector<PathModel> &others);

} // namespace sketch
} // namespace c2d
