#pragma once
#include "element.h"
#include <QByteArray>
#include <QJsonObject>
#include <QPainterPath>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>

// Vector import (Carbide Create's "Import DXF and SVG" equivalent).
//
// Both readers are self-contained (no QtSvg, no external DXF library): the SVG
// side is a QXmlStreamReader walk with a full `d` grammar parser, the DXF side
// a group-code reader for the ASCII R12..2018 dialects. Everything is turned
// into a QPainterPath in CC space (millimetres, Y-up, origin bottom-left) —
// beziers are kept as beziers because the CC path schema carries cubic control
// points (point_type 3 + cp1/cp2); only DXF SPLINEs are flattened (0.02 mm).
// Each closed or open contour becomes one CC `path` element in the exact JSON
// shape Element::makePath writes, so the saved file still opens in CC.
namespace c2d {

struct ImportOptions {
    double stockWidth = 0;    // mm; 0 = unknown (always relocate)
    double stockHeight = 0;
    QJsonObject layer;        // layer object for the new elements (Document::defaultLayer)
    bool autoPlace = true;    // move bbox bottom-left to (margin, margin) unless it fits the stock
    double margin = 10.0;     // mm
    double tolerance = 0.02;  // mm, spline flattening
};

struct ImportResult {
    bool ok = false;
    QString error;            // set when !ok
    QVector<Element> elements;
    QRectF bounds;            // mm, Y-up, after placement (empty when nothing imported)
    bool relocated = false;   // true when the geometry was moved to (margin, margin)
    int skipped = 0;          // unsupported entities/elements ignored
    QStringList notes;        // human-readable summary of what was skipped
    QString summary() const;  // one-line status text
};

// Parse from memory. `sourceName` is only used in messages.
ImportResult importSvgData(const QByteArray &data, const ImportOptions &opt,
                           const QString &sourceName = QString());
ImportResult importDxfData(const QByteArray &data, const ImportOptions &opt,
                           const QString &sourceName = QString());

// Read the file and dispatch on the extension (.svg / .dxf, case-insensitive).
ImportResult importFile(const QString &path, const ImportOptions &opt);
bool isImportableFile(const QString &path);

// Building blocks (exposed for tests):
// One CC `path` element per subpath of `p` (mm, Y-up). A subpath whose last
// point coincides with its first is written closed (line-back + close row).
QVector<Element> elementsFromPath(const QPainterPath &p, const QJsonObject &layer);
// Elliptical arc as cubic beziers appended to `path` (starts with moveTo when
// `path` is empty or `startNew`). Angles in radians, sweep signed (CCW > 0 in a
// Y-up frame). `rotation` rotates the ellipse axes.
void appendArc(QPainterPath &path, QPointF center, double rx, double ry, double rotation,
               double startAngle, double sweep, bool startNew);
// Parse an SVG path `d` string into a Y-down user-unit QPainterPath.
QPainterPath parseSvgPathData(const QString &d, bool *ok = nullptr);

} // namespace c2d
