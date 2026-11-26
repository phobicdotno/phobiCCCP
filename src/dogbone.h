#pragma once
// Corner relief for a round bit: a dogbone or T-bone bite in every inside
// corner, so a square tenon or a mating part seats fully in a pocket or
// slot. Same rule as Syntexophobic's fabtools.dogbones().

#include <QJsonValue>
#include <QList>
#include <QPainterPath>
#include <QPointF>

namespace c2d {

enum class CornerStyle { Sharp, Dogbone, TBone };

// The toolpath key "corners": "dogbone" or "tbone"; anything else (absent,
// "off") is a sharp corner.
CornerStyle cornerStyleOf(const QJsonValue &v);

struct CornerBite {
    QPointF corner;   // the shape's corner; the bite circle passes through it
    QPointF centre;   // the bite circle's centre, toolD / 2 from the corner
};

// The bites for the filled `region` (even-odd). `materialInside` says which
// side is the part: true for an outside cut (the region is the part, bites go
// into it at its reflex corners), false for a pocket or inside cut (the
// region is the hole, bites grow it outward at its convex corners). Only
// corners whose opening angle (90 in a square slot) is within [minDeg, maxDeg]
// get one. A dogbone's centre lies on the corner bisector, a T-bone's on the
// longer wall, so the shorter wall (a slot bottom) keeps its full depth.
QList<CornerBite> cornerBites(const QPainterPath &region, bool materialInside, double toolD,
                              CornerStyle style, double minDeg = 0, double maxDeg = 135);

// `region` with the bites cut into the part: material minus bites, i.e. the
// part shrunk for an outside cut, the hole grown for an inside one. Each bite
// is 0.05 mm wider in radius than the bit (still through the corner), so the
// tool centre really gets to its centre. Returns
// the region unchanged for Sharp, a zero bit, or no corner in range.
QPainterPath withCornerRelief(const QPainterPath &region, bool materialInside, double toolD,
                              CornerStyle style, double minDeg = 0, double maxDeg = 135,
                              int *count = nullptr);

} // namespace c2d
