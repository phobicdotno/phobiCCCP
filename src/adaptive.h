#pragma once
#include <QPointF>
#include <QPolygonF>
#include <QVector>
#include <functional>

// Adaptive clearing (Fusion 360's 2D Adaptive): pocket a region without the
// cutter ever taking much more than a chosen radial bite, so a pocket can be
// cut at a deep stepdown with a light stepover instead of slotting.
//
// How: the tool enters each connected part of the region on a small helix
// at its widest point. From there the cleared area grows outward: the next
// pass is the boundary of the visited centre area grown by the stepover and
// clipped to where the tool may go (the region inset by tool radius + stock
// to leave). A raster of the material already removed measures how much of
// the cutter's front half each point of a candidate pass would bury; where
// that exceeds the limit (at the junctions between the growing front and the
// walls), the step is halved and tried again, down to an eighth of the
// stepover. Stretches of a pass that would cut nothing (walls already
// finished) are dropped, so every pass is only the new cut. Passes run climb
// (counter-clockwise outward; reversed with climb = false).
//
// Pure geometry: the plan is a list of 2D moves the exporter repeats at
// every depth level.
namespace c2d {
namespace adaptive {

struct Params {
    double toolR = 3.175;       // mm
    double stepover = 0.635;    // mm, the radial bite on a straight front
    double leave = 0;           // stock to leave on the walls, mm
    bool climb = true;
    double maxEngageFactor = 1.5;   // allowed engagement = factor x the straight-front angle
};

struct Move {
    enum Kind { Helix, Cut } kind = Cut;
    QPointF center;      // Helix: the helix center
    double radius = 0;   // Helix: the helix radius
    QPolygonF path;      // Cut: tool-center polyline, mm
    // Cut: how to get to its start from wherever the last move ended.
    // StayDown: feed straight there at depth, through cleared space only.
    // Lift: a short hop just above the level being cut (the straight line
    // stays over the pocket, where everything above this level is gone).
    // Retract: up to safe Z (the straight line would cross a wall or island).
    enum Link { StayDown, Lift, Retract } link = Retract;
    QPolygonF via;       // Lift: corners of the hop between the last end and this start
                         // (a dog-leg round a corner of the pocket), may be empty
};

struct Plan {
    QVector<Move> moves;
    double straightEngageDeg = 0;   // engagement of a straight front at the stepover
    double limitDeg = 0;            // the most a pass was allowed to take
    double maxEngageDeg = 0;        // the most any planned pass takes (the helix entries,
                                    // which slot by nature, not counted)
    int unreached = 0;              // parts too narrow for even a small helix
    bool available = true;          // false: built without Clipper2
};

// `rings`: closed boundary loops of the region (even-odd, so holes and
// islands are just more loops), mm. `cancel`, when set, is polled between
// passes; returning true abandons the plan (moves so far are kept).
Plan plan(const QVector<QPolygonF> &rings, const Params &p,
          const std::function<bool()> &cancel = {});

} // namespace adaptive
} // namespace c2d
