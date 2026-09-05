#pragma once
#include "gcodeexport.h"
#include "heightmodel.h"
#include "post_grbl.h"

#include <QJsonObject>
#include <QList>
#include <QPainterPath>
#include <QPointF>
#include <QVector>

// Carbide Create Pro's 3D toolpaths over a HeightModel relief:
//   * "3D Rough Machining"  (type `3d_rough_toolpath`)  — Z-level clearing with
//     a flat end mill: at each level the cells whose surface (+ stock to leave)
//     lies below the level form a region, marching-squared into polygons and
//     pocketed with the 2D ring machinery in gcodeexport.cpp.
//   * "3D Finish Machining" (type `3d_finish_toolpath`) — parallel raster
//     passes whose Z follows the tool-compensated surface (the highest tip
//     position at which the tool's footprint still touches the model), so a
//     ball, flat, bull-nose or V cutter never gouges the relief.
// CC's own type names for these are undocumented in our format notes; the two
// identifiers above are ours and are what the panel and the exporter agree on.
//
// JSON (shared): elements (closed boundary vectors; empty = model bounds),
// tool, speeds, stock_to_leave (mm), stepdown (mm), stepover (mm number, or a
// "40%" string of the tool diameter), max_depth (depth string/number, positive
// down like every other depth), boundary_offset (mm, + outward).
// Finish only: raster_angle (deg, 0 = along X, 90 = along Y), direction
// ("climb" | "conventional" | "zigzag"), both_directions (second pass set at
// raster_angle + 90).
namespace c2d {

struct Cam3dParams {
    enum Direction { Climb, Conventional, Zigzag };
    ToolGeom tool;
    double stockToLeave = 0;
    double stepdown = 1.0;
    double stepover = 1.0;        // mm, resolved from a percentage if given
    double maxDepth = -1e9;       // machine Z floor (<= 0); below the model = model floor
    double boundaryOffset = 0;    // mm, positive grows the boundary
    double rasterAngle = 0;       // degrees
    Direction direction = Zigzag;
    bool bothDirections = false;
    double safeZ = 2.54, feed = 500, plunge = 100;
};

// Read the toolpath JSON (both kinds share the parser; finish-only keys are
// simply left at their defaults for a rough toolpath).
Cam3dParams cam3dParams(const QJsonObject &toolpath, double safeZ);

// The area the toolpath may work in: `vectors` (already a filled region, may
// be empty = the model's bounds) grown/shrunk by `offset`.
QPainterPath cam3dBoundary(const HeightModel &model, const QPainterPath &vectors, double offset);

// ---- Rough ---------------------------------------------------------------
// The regions to clear, one per Z level, shallowest first; levels that clear
// nothing are omitted. Regions are in model space (not yet inset by the tool
// radius) with even-odd holes, ready for components()/insetRings().
struct RoughLevel {
    double z = 0;
    QPainterPath region;
};
QList<RoughLevel> roughLevels(const HeightModel &model, const Cam3dParams &p,
                              const QPainterPath &boundary);

// Can the flat tool travel straight from a to b with its tip at z without
// touching the model (+ stock to leave)? Used for the stay-down ring links.
bool roughLinkClear(const HeightModel &model, const Cam3dParams &p,
                    const QPointF &a, const QPointF &b, double z);

// ---- Finish --------------------------------------------------------------
// Tool tip Z at which the tool, centred on (x, y), first touches the model:
// max over the footprint of surface(q) - profile(|q - (x, y)|). Never above 0.
double compensatedZ(const HeightModel &model, const ToolGeom &tool, double x, double y);

// Raster passes as machine ops (rapids/feeds only — no tool/spindle/comment
// framing). `passes` receives the number of cutting passes emitted.
QVector<Op> finishOps(const HeightModel &model, const Cam3dParams &p,
                      const QPainterPath &boundary, int *passes = nullptr);

// ---- "New 3D toolpath" defaults for the Toolpaths panel --------------------
QJsonObject defaultRoughToolpathJson();
QJsonObject defaultFinishToolpathJson();

} // namespace c2d
