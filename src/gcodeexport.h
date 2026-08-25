#pragma once
#include <QString>
#include <QStringList>

// Tier-2 CAM: generates machine-ready GRBL g-code (plaintext .nc, same dialect
// Carbide Create hands to Carbide Motion) for:
//   - contour (ofset_dir 0 follow / -1 inside / 1 outside) and cutout
//     (flip_inside_outside, cut_depth/depth_per_pass number fields)
//   - pocket_toolpath: inward offset rings by stepover, innermost first
//   - drilling_toolpath: peck-drill at each referenced circle's center
// Offsetting runs on Clipper2 (vendored, third_party/clipper2) for robust
// polygon insets/outsets; stock_to_leave is honoured. V-carve, texture and
// keyhole still need their own engines and are skipped with a note. Depth
// strings handle both sign conventions (build 843 negative-down, build 853
// positive-down).
namespace c2d {

class Document;

struct GcodeResult {
    QString gcode;
    QStringList done;      // toolpath names emitted
    QStringList skipped;   // "name (reason)" for what was left out
    QStringList warnings;  // emitted but needs attention (e.g. tabs not cut)
};

GcodeResult exportGcode(Document &doc);

} // namespace c2d
