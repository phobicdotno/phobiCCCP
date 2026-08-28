#pragma once
#include <QList>
#include <QPolygonF>
#include <QVector>

// True medial-axis extraction for v-carving, built on the segment Voronoi
// diagram (boost::polygon). The medial axis of a region is where a V-bit rides:
// at a point with clearance d to the boundary, the bit sinks to depth
// d / tan(bitAngle/2) and its cone exactly kisses both walls. The union of the
// maximal disks along the axis covers the whole region, so tracing the axis
// carves the complete shape with sharp corners — what the inset-ring
// approximation could never do.
namespace c2d {

struct VPoint {
    double x = 0, y = 0;   // mm
    double d = 0;          // clearance: distance to the region boundary (mm)
};

// Medial-axis polyline chains of the region bounded by `rings` (one connected
// component: outer ring plus holes, even-odd). Rings may be open or closed
// polygons; they are densified internally. Returns an empty list when the
// Voronoi backend is compiled out or the region is degenerate.
QVector<QVector<VPoint>> medialAxis(const QList<QPolygonF> &rings);

bool medialAxisAvailable();

} // namespace c2d
