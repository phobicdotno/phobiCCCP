#pragma once
#include <QPainterPath>
#include <QString>
#include <QStringList>

// Single-line (stroke) fonts for engraving: each letter is a few open pen
// strokes instead of an outline, so Engrave and Fluting cut one groove per
// line rather than tracing both sides of it. A built-in design on a small
// grid: cap height 10 units, x-height 6, baseline 0, descenders to -3, Y-up.
// Covers printable ASCII (32..126); anything else is left as a blank space.
namespace c2d::strokefont {

constexpr double kCapHeight = 10.0;

// The family names a text element stores in font/qtfont.
QString romanFamily();     // "Single line (Roman)": upright
QString italicFamily();    // "Single line (Italic)": the same strokes slanted
QStringList families();
bool isStrokeFamily(const QString &family);

// The glyph's strokes in font units (open subpaths, moveTo/lineTo only),
// left edge at x = 0; `advance` gets its width including spacing.
QPainterPath glyph(const QString &family, QChar c, double *advance);

} // namespace c2d::strokefont
