#pragma once
#include <QJsonObject>

// The pad's Preview: a pad design built here (padlink::buildDocument), its
// program exported in memory as for a job (exportGcode) and drawn from above
// over the shapes, with the time it should take.
namespace c2d::padlink {

// A design bigger than this is not previewed from a pad: the export runs on
// the computer's UI thread while the pad waits.
constexpr int kMaxPreviewShapes = 400;
constexpr int kMaxPreviewPoints = 40000;
constexpr int kMaxPreviewToolpaths = 30;

// {"ok": true, "png": base64 PNG about `longSide` px on its long side,
//  "seconds": total estimate, "perToolpath": [{"name", "seconds"}],
//  "notes": [what the build and the export left out or changed]};
// {"ok": false, "error": why} when the design is malformed, too big or does
// not build.
QJsonObject previewJson(const QJsonObject &design, int longSide = 1024);

}
