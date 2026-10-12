#pragma once
// Construction math behind the Fusion-style circle and rectangle modes:
// pure geometry on CC mm, no Qt widgets, so it is tested headlessly.

#include <QLineF>
#include <QPointF>
#include <QVector>

namespace c2d {
namespace sketch {

struct Circle {
    QPointF center;
    double radius = 0;
    bool valid = false;
};

// Circle through three points (circumcircle); invalid when they are collinear.
Circle circleThrough3(const QPointF &a, const QPointF &b, const QPointF &c);

// Circle tangent to two (infinite) lines, its centre on the angle bisector
// nearest `cursor` and as far along it as the cursor's projection.
// Parallel lines give the circle midway between them, centred at the cursor.
Circle circleTangent2(const QLineF &l1, const QLineF &l2, const QPointF &cursor);

// Circle tangent to three (infinite) lines: of the in- and excircles of the
// triangle they form, the one whose centre is nearest `cursor`.
Circle circleTangent3(const QLineF &l1, const QLineF &l2, const QLineF &l3,
                      const QPointF &cursor);

// Rectangle from a first edge p1->p2 and a third point giving its height
// (signed, perpendicular to the edge). Corners in order p1, p2, p2+h, p1+h.
QVector<QPointF> rect3Point(const QPointF &p1, const QPointF &p2, const QPointF &p3);

// True when the corners form an axis-aligned rectangle (so it can stay a
// parametric CC rectangle rather than becoming a path).
bool isAxisAligned(const QVector<QPointF> &corners, double tol = 1e-6);

} // namespace sketch
} // namespace c2d
