#pragma once
#include "post_grbl.h"
#include <QString>
#include <QStringList>
#include <QVector>

// Toolpath tiling: a job taller (in Y) than the machine is cut as several
// programs, the stock being slid forward by one tile height between them.
//
// Semantics (Carbide Create's file format only exposes the parameters —
// tiling_enabled, tile_height, tile_overlap_y, tile_margin_x — so the split
// rule below is this app's own, documented choice):
//   - tile k (k = 0, 1, ...) holds the motion whose Y lies in
//     [k·tileH, (k+1)·tileH); tile 0 also takes anything below Y = 0.
//   - every tile is translated by −k·tileH in Y, so each program runs in
//     [0, tileH) with the stock re-indexed forward by k·tileH.
//   - a move crossing a tile boundary is split at the boundary: lines by
//     parametric clipping, arcs by tessellating them into short chords first
//     (an arc that stays inside one tile is kept as G2/G3).
//   - each cut is made exactly once — tile_overlap_y is the physical
//     alignment overlap of the stock on the table, not a re-cut zone.
//   - each tile is a complete program: G90/G21 header, tool changes,
//     spindle on/off, retract to safe Z before every re-entry and at the end,
//     M02. Comments, tool changes and spindle commands are repeated in every
//     tile in their original order.
namespace c2d {

class Document;

// Number of tiles a job needs for a tile height (>= 1).
int tileCount(const QVector<Op> &ops, double tileHeight);

// Split the ops of one program into per-tile op lists (see above). safeZ is
// the retract height used for the re-entry moves the split introduces.
QVector<QVector<Op>> tileOps(const QVector<Op> &ops, double tileHeight, double safeZ);

struct TiledExport {
    QStringList files;      // paths written, tile order
    QStringList gcode;      // the programs, tile order
    QStringList done;       // toolpaths exported (from exportGcode)
    QStringList skipped;
    double tileHeight = 0;
    QString error;          // non-empty on failure
};

// Export the document and write <outBase>_tile1.nc, _tile2.nc, ... .
// tileHeight <= 0 means the document's `tile_height` param (508 mm default).
TiledExport exportTiled(Document &doc, const QString &outBase, double tileHeight = 0);

} // namespace c2d
