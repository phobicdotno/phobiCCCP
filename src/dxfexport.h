#pragma once
#include "element.h"
#include <QByteArray>
#include <QVector>

// Vector export to ASCII DXF (R12 dialect: POLYLINE/VERTEX/SEQEND and
// CIRCLE), the lowest common denominator every CAD, CAM and laser program
// reads. Coordinates are written as they are in CC space (millimetres, Y-up,
// origin bottom-left), which is DXF's own orientation, and $INSUNITS says mm.
//
// Circles go out as true CIRCLE entities; everything else (paths, rectangles,
// polygons, text glyphs) is flattened at `tolerance` mm into polylines, closed
// where the contour is closed. Construction geometry is never written. Each
// element lands on a DXF layer named after its CC layer.
namespace c2d {

struct DxfExportResult {
    QByteArray data;
    int entities = 0;   // CIRCLE + POLYLINE entities written
    int skipped = 0;    // construction elements and empty paths left out
};

DxfExportResult exportDxf(const QVector<Element> &elements, double tolerance = 0.01);

} // namespace c2d
