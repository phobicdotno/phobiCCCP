#pragma once
#include "c2ddocument.h"
#include "post_grbl.h"

#include <QPainterPath>
#include <QStringList>
#include <QVector>

// CAM: computes cutter paths for contour, pocket and drilling toolpaths with
// Clipper2 polygon offsetting and emits GrblPost Ops. Depths follow the CC
// build 853 convention (positive-down strings); machine Z is negated, with
// Z 0 at the stock top and a fixed retract height above it. V-carve and
// texture toolpaths are out of scope (Tier 3).
namespace c2d {

class Cam
{
public:
    struct Result {
        QVector<Op> ops;
        QStringList notes;   // skipped toolpaths and warnings
    };

    // Ops for every enabled, supported toolpath in the document, in order.
    static Result generate(const Document &doc);

    // Ops for one toolpath over its resolved element outlines (exposed for
    // headless tests). Dispatches on tp.json["type"].
    static QVector<Op> toolpathOps(const Toolpath &tp,
                                   const QVector<QPainterPath> &shapes,
                                   QStringList *notes);

    static constexpr double kRetractMm = 3.0;   // rapid height above stock
};

} // namespace c2d
