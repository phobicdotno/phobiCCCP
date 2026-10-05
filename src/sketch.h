#pragma once
#include "element.h"
#include <QPointF>

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

} // namespace sketch
} // namespace c2d
