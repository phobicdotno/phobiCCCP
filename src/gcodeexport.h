#pragma once
#include "post_grbl.h"
#include <QString>
#include <QStringList>
#include <QVector>

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

} // namespace c2d
