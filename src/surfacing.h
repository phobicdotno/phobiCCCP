#pragma once
#include "post_grbl.h"

#include <QString>
#include <QVector>

namespace c2d {

// Surfacing (flattening a spoilboard or a slab) from the Machine page, with
// no design: zigzag lines across a rectangle at the stepover, in depth
// passes. The rectangle is W x H from its corner (x0, y0), in work
// coordinates, +X to the right and +Y to the back. The bit's centre runs
// from one radius before each edge to one radius past it, both along the
// lines and across them, so every edge is cut clean.
struct SurfacingParams {
    double x0 = 0, y0 = 0;         // corner (work, mm)
    double width = 100, height = 50;
    double toolDia = 20;
    double stepoverPct = 40;       // % of the diameter
    double stepdown = 0.5;         // mm per pass
    double depth = 0.5;            // total, below Z0
    double feed = 2000, plunge = 500;   // mm/min
    int rpm = 18000;
    bool alongX = true;            // lines run along X (step across Y)
    bool finishClimb = false;      // one climb pass round the edge at the full depth
    double safeZ = 5;              // rapids height above Z0
};

struct SurfacingPlan {
    QVector<Op> ops;
    int lines = 0;                 // zigzag lines per pass
    int passes = 0;                // depth passes
    double minX = 0, maxX = 0, minY = 0, maxY = 0;   // where the bit's centre goes (work)
    QString error;                 // empty when it can be made
    bool ok() const { return error.isEmpty(); }
};

// More moves than this and the plan is refused (error set, no ops).
constexpr double kSurfacingMaxOps = 2.0e6;

SurfacingPlan planSurfacing(const SurfacingParams &p);

// The plan's centre path (work) moved by the work offset (machine = work +
// wco) must stay inside [left, right] x [front, back] (machine). Empty when
// it fits, else the reason in plain words.
QString surfacingFitProblem(const SurfacingPlan &plan, double wcoX, double wcoY,
                            double left, double right, double front, double back);

} // namespace c2d
