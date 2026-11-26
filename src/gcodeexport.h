#pragma once
#include "post_grbl.h"
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QPainterPath>
#include <QPointF>
#include <QVector3D>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cmath>
#include <functional>

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
struct HeightModel;

struct GcodeResult {
    QString gcode;
    QStringList done;      // toolpath names emitted
    QStringList doneIds;   // their uuids (names need not be unique)
    QStringList skipped;   // "name (reason)" for what was left out
    QStringList notes;     // "name: ..." for what was changed on the way (rest depth)
    QVector<Op> ops;       // the machine operations, for on-canvas preview
    bool cancelled = false;   // abandoned part-way: `ops` and `gcode` are partial
};

// Cooperative progress and cancellation for the long passes. A 3D finish over
// a fine relief with a large ball is minutes of work, and the export runs on
// whatever thread called it - so a caller that wants to stay responsive needs
// somewhere to say "stop". Passing nullptr keeps the old behaviour exactly:
// no callbacks, never cancelled.
struct ExportWatch {
    // Called at safe points, between toolpaths and between passes inside one.
    // `what` names the toolpath or stage. Return false to abandon the export;
    // the result then has `cancelled` set and holds only what was finished.
    std::function<bool(int done, int total, const QString &what)> step;

    // Convenience: true when the caller has asked to stop.
    bool stop(int done, int total, const QString &what) const
    {
        return step && !step(done, total, what);
    }
};

// `relief`, when given, is used instead of asking heightModelFor(doc) for the
// document's 3D model. That matters for a caller on a worker thread: the
// provider composites lazily and caches the result in state the modelling
// panel also writes, so letting a background export reach it would be a race.
// Pass a snapshot taken on the owning thread instead.
GcodeResult exportGcode(Document &doc, const ExportWatch *watch = nullptr,
                        const HeightModel *relief = nullptr);

// A tool picked up again after another one: changes that would not be
// needed with the toolpaths ordered tool by tool. Empty when the order is
// fine; otherwise one sentence for the operator.
QString toolReuseWarning(const QVector<Op> &ops);

// A toolpath that cuts deeper than its bit's flutes (the embedded tool's
// `length`): past them the smooth shank rubs the wall, burns and can snap
// the bit. Depth is the deepest the toolpath goes below the stock top: end
// depth, a cutout's cut depth plus its break-through, or a 3D toolpath's
// start plus its max depth. Empty when it fits, when the length is not
// known (0 or missing), and for V-bits and V-carve / chamfer / texture,
// whose cone has its own check. `type` is Toolpath::type.
QString fluteLengthWarning(const QString &type, const QJsonObject &toolpath);

// The document's retract height, as every exporter must read it. `retract`
// comes verbatim out of the params table and QString::toDouble returns 0 for
// anything it cannot parse (an empty value, a comma decimal from another
// locale). A safe Z of 0 rapids across the stock with the tip at its top
// surface, over clamps, chips and uncut material; a negative one rapids
// through it. Both the plain and the tiled exporter go through this so the
// guard cannot drift apart again.
double documentSafeZ(const Document &doc);

// Job zero: where the program's X0 Y0 Z0 is on the stock. Carbide Create's
// .c2d has no key for it, so these are phobiCCCP's own params (kept by CC):
//   phobic_zero_xy = lower_left (default) | centre | upper_left | upper_right | lower_right
//   phobic_zero_z  = top (default) | bottom   (bottom = the spoilboard)
// Design coordinates keep the stock's lower-left at 0,0 and its top at Z 0;
// only the posted program is moved, so with the defaults it is unchanged.
enum class ZeroXY { LowerLeft, Centre, UpperLeft, UpperRight, LowerRight };
ZeroXY jobZeroXY(const Document &doc);
bool jobZeroBottom(const Document &doc);
QString zeroXYKey(ZeroXY z);           // the param value
QString zeroXYLabel(ZeroXY z);         // "Lower left", "Centre", ...
// The chosen zero in design coordinates (on the stock rectangle), X and Y.
QPointF jobZeroPoint(const Document &doc);
// Added to every design coordinate to get the program's: (-zx, -zy, +thickness or 0).
QVector3D jobZeroShift(const Document &doc);
bool jobZeroIsDefault(const Document &doc);
// Moves Rapid / Feed / Arc targets by `shift` (arc I/J are relative and stay).
void shiftOps(QVector<Op> &ops, const QVector3D &shift);
// "centre of stock, top" - for the setup sheet and the machine notes.
QString jobZeroText(const Document &doc);

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

// Where a 2D toolpath means to leave a flat floor, for the simulation's
// "what's left" check: every enabled pocket over its whole closed region
// (less any stock to leave), and the flat clearing area of a V-carve (where
// the shape is wider than the cone reaches at its end depth). Contours and
// cutouts are left out: they only cut a band along the line. `z` is the
// floor (mm, <= 0 from the stock top).
struct IntendedFloor {
    QString name;          // toolpath name
    QPainterPath region;   // document coordinates (mm), odd-even fill
    double z = 0;
};
QVector<IntendedFloor> intendedFloors(Document &doc);

// Tool table built from the toolpaths' embedded tool objects (`tool` and, for
// v-carves, `tool_pocket`). First definition of a number wins.
QHash<int, ToolGeom> toolGeometry(const Document &doc);

// One embedded tool object (CC keys: number, diameter, type, angle,
// corner_radius) as cutting geometry.
ToolGeom toolGeomFromJson(const QJsonObject &tool);

} // namespace c2d
