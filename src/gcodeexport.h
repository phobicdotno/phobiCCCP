#pragma once
#include "post_grbl.h"
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cmath>

// First slice of Tier-2 CAM: generates machine-ready GRBL g-code (plaintext
// .nc, same dialect Carbide Create hands to Carbide Motion) for the toolpath
// types that need no geometry offsetting:
//   - contour with ofset_dir == 0 ("no offset"): follow each vector at depth
//   - drilling_toolpath: peck-drill at each referenced circle's center
// Pocket / offset contour / v-carve still need Clipper2 and are skipped with a
// note in the g-code header. Depth strings handle both sign conventions
// (build 843 negative-down, build 853 positive-down).
namespace c2d {

class Document;

struct GcodeResult {
    QString gcode;
    QStringList done;      // toolpath names emitted
    QStringList skipped;   // "name (reason)" for what was left out
    QVector<Op> ops;       // the machine operations, for on-canvas preview
};

GcodeResult exportGcode(Document &doc);

// The document's retract height, as every exporter must read it. `retract`
// comes verbatim out of the params table and QString::toDouble returns 0 for
// anything it cannot parse (an empty value, a comma decimal from another
// locale). A safe Z of 0 rapids across the stock with the tip at its top
// surface, over clamps, chips and uncut material; a negative one rapids
// through it. Both the plain and the tiled exporter go through this so the
// guard cannot drift apart again.
double documentSafeZ(const Document &doc);

// Carbide Create writes some numbers as JSON strings ("0.500") and others as
// numbers, and which is which varies by key and by build. QJsonValue::toDouble
// returns the *default* for a string, so a plain toDouble(def) on file data
// silently substitutes the default instead of reading the value -- turning a
// "0.500" depth per pass into the 1.0 mm default, twice the requested cut,
// always in the unsafe direction. Every read of a number that came out of a
// document must go through this.
inline double numOr(const QJsonValue &v, double def)
{
    if (v.isDouble())
        return v.toDouble();
    if (v.isString()) {
        bool ok = false;
        const double d = v.toString().toDouble(&ok);
        if (ok && std::isfinite(d))
            return d;
    }
    return def;
}

// Cutting geometry of one tool, keyed by the tool number that Op::Tool ops
// carry — what a material-removal simulation needs and nothing else.
struct ToolGeom {
    enum Kind { Flat, Ball, VBit };
    int number = 0;
    Kind kind = Flat;
    double diameter = 3.175;   // mm
    double angle = 0;          // V-bit included angle (deg); 0 for end mills
    double cornerRadius = 0;   // bull-nose corner radius (mm); = radius for ball
    QString name;
    double radius() const { return diameter / 2.0; }
};

// Tool table built from the toolpaths' embedded tool objects (`tool` and, for
// v-carves, `tool_pocket`). First definition of a number wins.
QHash<int, ToolGeom> toolGeometry(const Document &doc);

// One embedded tool object (CC keys: number, diameter, type, angle,
// corner_radius) as cutting geometry.
ToolGeom toolGeomFromJson(const QJsonObject &tool);

} // namespace c2d
