#pragma once
#include "gcodeexport.h"
#include "tiling.h"

class QWidget;

namespace c2d {

class Document;

// Run a g-code export without freezing the window.
//
// A 3D finish is not a quick job: a 100 mm relief at the modeller's finest
// cell takes 83 s with a 3.2 mm ball and over four minutes with a 12.7 mm one,
// and exportGcode runs on whatever thread asks for it. Called straight from a
// menu handler that is the interface thread, so the window went unresponsive
// for the whole pass with nothing to look at and no way to stop it.
//
// These run the export on a worker behind a modal progress dialog with a
// Cancel button. The document and the composited relief are both snapshotted
// on the calling thread first, so the worker touches nothing the interface
// (or the model panel's own rebuild) can be writing at the same time.
//
// A cancelled export comes back with `cancelled` set and only the toolpaths
// that finished; callers must not write that to a file.
GcodeResult exportWithProgress(QWidget *parent, Document &doc, const QString &title);

TiledExport exportTiledWithProgress(QWidget *parent, Document &doc, const QString &outBase,
                                    double tileHeight, const QString &title);

} // namespace c2d
