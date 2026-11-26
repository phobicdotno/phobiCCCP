#pragma once
#include <QByteArray>
#include <QPointF>
#include <QPolygonF>
#include <QString>
#include <QVector>

namespace c2d {

// QR code symbols (ISO/IEC 18004), byte mode, versions 1 to 10: enough for a
// URL or a short note on a sign. Pure logic, no widgets.
enum class QrEcc { L = 0, M = 1, Q = 2, H = 3 };

struct QrCode {
    int version = 0;          // 0: nothing encoded (see error)
    int size = 0;             // modules per side: 17 + 4 * version
    int mask = -1;            // 0..7, picked by the lowest penalty
    QrEcc ecc = QrEcc::M;
    QVector<bool> modules;    // row-major, row 0 at the top, true = dark
    QString error;

    bool dark(int x, int y) const { return modules.at(y * size + x); }
    int darkCount() const;
};

// Encodes `data` as bytes in the smallest version (1..10) that holds it at
// `ecc`. On failure version is 0 and error says why.
QrCode qrEncode(const QByteArray &data, QrEcc ecc);

// The 15 format bits (with BCH and the 0x5412 mask) for an ecc level and mask.
int qrFormatBits(QrEcc ecc, int mask);

// Most bytes a version-10 symbol holds at `ecc`.
int qrMaxBytes(QrEcc ecc);

// The symbol as closed vectors, `sizeMm` across including `quiet` light
// modules on every side, centred on `centre`, Y up (design coordinates).
// merge: the dark modules joined into outlines with holes (nested rings, as
// a Pocket sees them); otherwise one square per dark module.
struct QrDrawing {
    QVector<QPolygonF> rings;
    QPolygonF border;         // the outer square at the quiet-zone edge
    double moduleMm = 0;
};
QrDrawing qrDrawing(const QrCode &code, double sizeMm, int quiet, bool merge, QPointF centre);

} // namespace c2d
