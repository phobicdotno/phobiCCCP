#pragma once
#include "element.h"
#include <QImage>
#include <QJsonObject>
#include <QPointF>
#include <QPolygonF>
#include <QVector>

// Raster-to-vector tracing ("Automatically Trace Images"), pure Qt:
//   grayscale -> optional box blur -> threshold -> binary ink mask
//   -> crack-following contour extraction on the pixel lattice (outer
//      boundaries and holes as separate closed loops, 4-connected ink)
//   -> minimum-area filter (despeckle)
//   -> Douglas–Peucker simplification (tolerance in mm)
//   -> optional corner-preserving smoothing (cubic Bézier through the
//      non-corner vertices, corners stay sharp)
// Output is in CC space: millimetres, Y-up, with the image's bottom-left
// corner at `origin`. A 2000×2000 image traces in well under a second.
namespace c2d {

struct TraceOptions
{
    int threshold = 128;         // 0-255 on luminance
    bool invert = false;         // false: ink = darker than threshold
    int blurRadius = 0;          // box-blur radius in pixels (0 = off)
    double minAreaMm2 = 0.0;     // drop regions/holes smaller than this
    double mmPerPixel = 0.1;     // output scale (target width / image width)
    double simplifyTolMm = 0.1;  // Douglas–Peucker tolerance (0 = keep every corner)
    bool smooth = false;         // corner-preserving Bézier smoothing
    double cornerAngleDeg = 60;  // direction change >= this stays a sharp corner
    QPointF origin;              // CC mm of the image's bottom-left corner
};

struct TraceContour
{
    QPolygonF pts;          // vertices, mm Y-up, closed implicitly (last != first)
    QVector<bool> corner;   // per vertex, only meaningful when smoothing
    bool hole = false;      // true: inner boundary (CW); false: outer (CCW)
    double area = 0;        // enclosed area, mm² (positive)
    QRectF bounds() const { return pts.boundingRect(); }
};

struct TraceResult
{
    QVector<TraceContour> contours;
    int width = 0, height = 0;   // mask size in pixels
    int inkPixels = 0;
    qint64 elapsedMs = 0;
};

// The binary mask the tracer works from (Format_Grayscale8: 255 = ink).
// Exposed for the dialog's preview overlay.
QImage traceMask(const QImage &src, const TraceOptions &o);

TraceResult traceImage(const QImage &src, const TraceOptions &o);

// Douglas–Peucker on an implicitly closed ring. Keeps at least 3 points.
QPolygonF simplifyClosed(const QPolygonF &ring, double tol);

// Build a CC path element (exact CC J1 schema) from a traced contour:
// straight rows, or cubic rows through the smooth vertices when `smooth`.
Element traceContourElement(const TraceContour &c, bool smooth, const QJsonObject &layer);

// Convenience: painter path (mm) of what traceContourElement would produce.
QPainterPath traceContourPath(const TraceContour &c, bool smooth);

} // namespace c2d
