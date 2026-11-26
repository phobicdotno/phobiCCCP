#include "strokefont.h"

#include <QHash>
#include <QPair>
#include <QStringList>
#include <QTransform>
#include <QVector>
#include <QtMath>
#include <cmath>

namespace c2d::strokefont {

// One entry per character from ' ' (32) to '~' (126): the advance and the
// strokes. Commands: "M x y" lifts the pen and moves; "L x y" draws a line;
// "A cx cy rx ry a0 a1" draws an elliptic arc about (cx, cy) from angle a0
// to a1 in degrees (counter-clockwise when a1 > a0), joined to the stroke
// in progress by a line, or starting a new one.
struct Glyph { double advance; const char *strokes; };

static const Glyph kGlyphs[95] = {
    {5, ""},                                                                  // space
    {3, "M1.5 10 L1.5 3 M1.5 0.6 L1.5 0"},                                    // !
    {4.5, "M1.5 10 L1.5 7.5 M3 10 L3 7.5"},                                   // "
    {9.5, "M3 0 L4 10 M6 0 L7 10 M1 3.5 L8.5 3.5 M1.5 6.5 L9 6.5"},           // #
    {8.5, "A4.25 7 3 2 20 270 A4.25 3 3 2 90 -160 M4.25 10.5 L4.25 -0.5"},    // $
    {10, "M1 0 L9 10 M4 8 A2.5 8 1.5 2 0 360 M9 2 A7.5 2 1.5 2 0 360"},       // %
    {9, "M8 0 L2.3 7 A3.8 8.3 1.6 1.7 210 -30 L1.3 2.5 A3.5 2.5 2.2 2.5 180 330 L8 4.5"}, // &
    {3, "M1.5 10 L1.5 7.5"},                                                  // '
    {4.5, "A5 4 3.5 7 120 240"},                                              // (
    {4.5, "A-0.5 4 3.5 7 60 -60"},                                            // )
    {7, "M3.5 10 L3.5 4 M1 8.5 L6 5.5 M6 8.5 L1 5.5"},                        // *
    {8, "M4 1 L4 8 M0.5 4.5 L7.5 4.5"},                                       // +
    {3, "M1.5 0.6 L1.5 0 L0.8 -1.5"},                                         // ,
    {7, "M1 4.5 L6 4.5"},                                                     // -
    {3, "M1.5 0.6 L1.5 0"},                                                   // .
    {6, "M0.5 -1 L5.5 10.5"},                                                 // /
    {8, "A4 5 3 5 0 360"},                                                    // 0
    {8, "M2 8 L4.5 10 L4.5 0 M2 0 L7 0"},                                     // 1
    {8, "A4 7.5 2.8 2.5 160 -40 L1 0 L7 0"},                                  // 2
    {8, "A4 7.6 2.6 2.4 150 -90 A4 2.6 3 2.6 90 -150"},                       // 3
    {8, "M5.5 0 L5.5 10 L0.8 3 L7.5 3"},                                      // 4
    {8, "M6.8 10 L1.6 10 L1.2 5.6 A4 3.3 3 3.3 135 -160"},                    // 5
    {8, "M1 3 L1 5 A4.5 5 3.5 5 180 60 M1 3 A4 3 3 3 180 540"},               // 6
    {8, "M1 10 L7 10 L3 0"},                                                  // 7
    {8, "A4 7.6 2.5 2.4 -90 270 A4 2.6 3 2.6 90 450"},                        // 8
    {8, "M7 7 A4 7 3 3 0 360 M7 7 L7 5 A3.5 5 3.5 5 0 -120"},                 // 9
    {3, "M1.5 6 L1.5 5.4 M1.5 0.6 L1.5 0"},                                   // :
    {3, "M1.5 6 L1.5 5.4 M1.5 0.6 L1.5 0 L0.8 -1.5"},                         // ;
    {7.5, "M6.5 8 L1 4.5 L6.5 1"},                                            // <
    {8, "M1 6 L7 6 M1 3 L7 3"},                                               // =
    {7.5, "M1 8 L6.5 4.5 L1 1"},                                              // >
    {7.5, "A3.75 7.5 2.75 2.5 180 -60 L3.75 3 L3.75 2.5 M3.75 0.6 L3.75 0"},  // ?
    {11.5, "M7.5 4.5 A5.5 4.5 2 2 0 360 M7.5 6.5 L7.5 3 A8.75 3 1.25 1.25 180 360 A5.5 4.5 4.5 5.5 0 310"}, // @
    {9, "M1 0 L4.5 10 L8 0 M2.2 3.5 L6.8 3.5"},                               // A
    {9, "M1 0 L1 10 L5 10 A5 7.5 2.5 2.5 90 -90 L1 5 M5 5 A5.5 2.5 2.5 2.5 90 -90 L1 0"}, // B
    {9, "A4.5 5 3.5 5 45 315"},                                               // C
    {9, "M1 0 L1 10 L3.5 10 A3.5 5 4 5 90 -90 L1 0"},                         // D
    {8, "M7 10 L1 10 L1 0 L7 0 M1 5 L5.5 5"},                                 // E
    {8, "M7 10 L1 10 L1 0 M1 5 L5.5 5"},                                      // F
    {9.5, "A4.5 5 3.5 5 45 360 L5 5"},                                        // G
    {9, "M1 0 L1 10 M8 0 L8 10 M1 5 L8 5"},                                   // H
    {3, "M1.5 0 L1.5 10"},                                                    // I
    {7, "M6 10 L6 2.5 A3.5 2.5 2.5 2.5 0 -180"},                              // J
    {8.5, "M1 0 L1 10 M7.5 10 L1 3.5 M3.3 5.7 L7.5 0"},                       // K
    {7.5, "M1 10 L1 0 L7 0"},                                                 // L
    {10.5, "M1 0 L1 10 L5.25 2 L9.5 10 L9.5 0"},                              // M
    {9, "M1 0 L1 10 L8 0 L8 10"},                                             // N
    {10, "A5 5 4 5 0 360"},                                                   // O
    {8.5, "M1 0 L1 10 L5 10 A5 7.5 2.5 2.5 90 -90 L1 5"},                     // P
    {10, "A5 5 4 5 0 360 M6 2.5 L9 -0.5"},                                    // Q
    {8.5, "M1 0 L1 10 L5 10 A5 7.5 2.5 2.5 90 -90 L1 5 M4.5 5 L7.5 0"},       // R
    {8.5, "A4.25 7.5 3 2.5 20 270 A4.25 2.5 3 2.5 90 -160"},                  // S
    {8, "M0.5 10 L7.5 10 M4 10 L4 0"},                                        // T
    {9, "M1 10 L1 3.5 A4.5 3.5 3.5 3.5 180 360 L8 10"},                       // U
    {9, "M0.5 10 L4.5 0 L8.5 10"},                                            // V
    {12, "M0.5 10 L3 0 L6 8 L9 0 L11.5 10"},                                  // W
    {8.5, "M1 10 L7.5 0 M7.5 10 L1 0"},                                       // X
    {8.5, "M0.5 10 L4.25 5 L8 10 M4.25 5 L4.25 0"},                           // Y
    {8.5, "M1 10 L7.5 10 L1 0 L7.5 0"},                                       // Z
    {4.5, "M4 10.5 L1.5 10.5 L1.5 -2 L4 -2"},                                 // [
    {6, "M0.5 10.5 L5.5 -1"},                                                 // backslash
    {4.5, "M0.5 10.5 L3 10.5 L3 -2 L0.5 -2"},                                 // ]
    {7, "M1 7 L3.5 10 L6 7"},                                                 // ^
    {8, "M0 -1.5 L8 -1.5"},                                                   // _
    {4, "M1 10 L3 8"},                                                        // `
    {7.5, "A3.75 3 2.75 3 0 360 M6.5 6 L6.5 0"},                              // a
    {7.5, "M1 10 L1 0 M1 3 A3.75 3 2.75 3 180 -180"},                         // b
    {7, "A3.75 3 2.75 3 50 310"},                                             // c
    {7.5, "A3.75 3 2.75 3 0 360 M6.5 10 L6.5 0"},                             // d
    {7.5, "M1 3 L6.5 3 A3.75 3 2.75 3 0 315"},                                // e
    {5, "M5 9.5 A4 8.5 1.5 1.5 40 180 L2.5 0 M0.5 6 L4.5 6"},                 // f
    {7.5, "A3.75 3 2.75 3 0 360 M6.5 6 L6.5 -1 A3.75 -1 2.75 2 0 -160"},      // g
    {7.5, "M1 10 L1 0 M1 3.5 A3.75 3.5 2.75 2.5 180 0 L6.5 0"},               // h
    {3, "M1.5 0 L1.5 6 M1.5 8 L1.5 8.6"},                                     // i
    {3.5, "M2.5 6 L2.5 -1.5 A1 -1.5 1.5 1.5 0 -150 M2.5 8 L2.5 8.6"},         // j
    {7, "M1 10 L1 0 M6 6 L1 2 M2.8 3.4 L6 0"},                                // k
    {3, "M1.5 10 L1.5 0"},                                                    // l
    {11, "M1 0 L1 6 M1 4 A3.25 4 2.25 2 180 0 L5.5 0 M5.5 4 A7.75 4 2.25 2 180 0 L10 0"}, // m
    {7.5, "M1 0 L1 6 M1 3.5 A3.75 3.5 2.75 2.5 180 0 L6.5 0"},                // n
    {7.5, "A3.75 3 2.75 3 0 360"},                                            // o
    {7.5, "M1 6 L1 -3 M1 3 A3.75 3 2.75 3 180 -180"},                         // p
    {7.5, "A3.75 3 2.75 3 0 360 M6.5 6 L6.5 -3"},                             // q
    {5.5, "M1 0 L1 6 M1 3.5 A3.5 3.5 2.5 2.5 180 60"},                        // r
    {6.5, "A3.25 4.5 2.25 1.5 20 270 A3.25 1.5 2.25 1.5 90 -160"},            // s
    {5, "M2 9 L2 1 A3.5 1 1.5 1 180 270 L4.5 0 M0.5 6 L4.5 6"},               // t
    {7.5, "M1 6 L1 2.5 A3.75 2.5 2.75 2.5 180 360 M6.5 6 L6.5 0"},            // u
    {7, "M0.5 6 L3.5 0 L6.5 6"},                                              // v
    {10, "M0.5 6 L2.5 0 L5 5 L7.5 0 L9.5 6"},                                 // w
    {7, "M1 6 L6 0 M6 6 L1 0"},                                               // x
    {7, "M0.5 6 L3.5 0 M6.5 6 L2.5 -3 L1 -3"},                                // y
    {7, "M1 6 L6 6 L1 0 L6 0"},                                               // z
    {5, "M4 10.5 L3 10.5 L2.5 10 L2.5 5.5 L1.2 4.25 L2.5 3 L2.5 -1.5 L3 -2 L4 -2"}, // {
    {3, "M1.5 10.5 L1.5 -2"},                                                 // |
    {5, "M1 10.5 L2 10.5 L2.5 10 L2.5 5.5 L3.8 4.25 L2.5 3 L2.5 -1.5 L2 -2 L1 -2"}, // }
    {8, "M1 4 A2.5 4 1.5 1.2 180 0 A5.5 4 1.5 1.2 180 360"},                  // ~
};

// Letters past ASCII: a base letter from the table above (or none) plus
// the strokes added to it (an accent, a ring, a bar), and the advance when
// it differs from the base's.
struct Extra { ushort code; char base; double advance; const char *strokes; };

static const Extra kExtras[] = {
    {0x00C5, 'A', 0, "M4.5 11 A4.5 12 1 1 -90 270"},                          // Å
    {0x00E5, 'a', 0, "M3.75 7.5 A3.75 8.5 1 1 -90 270"},                      // å
    {0x00D8, 'O', 0, "M1.5 -0.5 L8.5 10.5"},                                  // Ø
    {0x00F8, 'o', 0, "M1 -0.5 L6.5 6.5"},                                     // ø
    {0x00C6, 0, 13, "M1 0 L5.5 10 L12 10 M5.5 10 L5.5 0 L12 0 M5.5 5 L10.5 5 M2.5 3.5 L5.5 3.5"}, // Æ
    {0x00E6, 0, 12, "A3.75 3 2.75 3 0 360 M6.5 6 L6.5 0 M6.5 3 L11.5 3 A9 3 2.5 3 0 315"}, // æ
    {0x00C4, 'A', 0, "M3 11.5 L3 12.1 M6 11.5 L6 12.1"},                      // Ä
    {0x00D6, 'O', 0, "M3.5 11.5 L3.5 12.1 M6.5 11.5 L6.5 12.1"},              // Ö
    {0x00DC, 'U', 0, "M3 11.5 L3 12.1 M6 11.5 L6 12.1"},                      // Ü
    {0x00E4, 'a', 0, "M2.5 7.5 L2.5 8.1 M5 7.5 L5 8.1"},                      // ä
    {0x00F6, 'o', 0, "M2.5 7.5 L2.5 8.1 M5 7.5 L5 8.1"},                      // ö
    {0x00FC, 'u', 0, "M2.5 7.5 L2.5 8.1 M5 7.5 L5 8.1"},                      // ü
    {0x00E9, 'e', 0, "M3.5 7.5 L5 9"},                                        // é
    {0x00E8, 'e', 0, "M2.5 9 L4 7.5"},                                        // è
    {0x00DF, 0, 7.5, "M1 0 L1 7.5 A3.5 7.5 2.5 2.5 180 -90 A3.75 2.5 2.75 2.5 90 -90 L2.5 0"}, // ß
};

QString romanFamily() { return QStringLiteral("Single line (Roman)"); }
QString italicFamily() { return QStringLiteral("Single line (Italic)"); }
QStringList families() { return {romanFamily(), italicFamily()}; }

bool isStrokeFamily(const QString &family)
{
    return family == romanFamily() || family == italicFamily();
}

static QPainterPath parse(const char *s)
{
    QPainterPath p;
    const QStringList tok = QString::fromLatin1(s).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    bool pen = false;                    // a stroke is in progress
    QPointF last;
    auto to = [&](const QPointF &q) {
        if (!pen) { p.moveTo(q); pen = true; }
        else if (q != last) p.lineTo(q);
        last = q;
    };
    // Each command letter is glued to its first number; the rest follow.
    int i = 0;
    auto num = [&]() { return i < tok.size() ? tok.at(i++).toDouble() : 0.0; };
    while (i < tok.size()) {
        const QString t = tok.at(i++);
        const QChar cmd = t.at(0);
        const double a = t.mid(1).toDouble();
        if (cmd == QLatin1Char('M')) {
            const double y = num();
            pen = false;
            to(QPointF(a, y));
        } else if (cmd == QLatin1Char('L')) {
            const double y = num();
            to(QPointF(a, y));
        } else if (cmd == QLatin1Char('A')) {
            const double cy = num(), rx = num(), ry = num(), a0 = num(), a1 = num();
            const int n = qMax(4, int(std::ceil(std::fabs(a1 - a0) / 7.5)));
            for (int k = 0; k <= n; ++k) {
                const double ang = qDegreesToRadians(a0 + (a1 - a0) * k / n);
                QPointF q(a + rx * std::cos(ang), cy + ry * std::sin(ang));
                // Round off float noise so a full turn ends on its start.
                q = QPointF(std::round(q.x() * 1e6) / 1e6, std::round(q.y() * 1e6) / 1e6);
                to(q);
            }
        }
    }
    return p;
}

QPainterPath glyph(const QString &family, QChar c, double *advance)
{
    static const QVector<QPainterPath> upright = [] {
        QVector<QPainterPath> v;
        for (const Glyph &g : kGlyphs)
            v.append(parse(g.strokes));
        return v;
    }();
    static const QHash<ushort, QPair<double, QPainterPath>> extras = [] {
        QHash<ushort, QPair<double, QPainterPath>> h;
        for (const Extra &x : kExtras) {
            QPainterPath p = parse(x.strokes);
            double adv = x.advance;
            if (x.base) {
                const int b = x.base - 32;
                p.addPath(parse(kGlyphs[b].strokes));
                if (adv <= 0)
                    adv = kGlyphs[b].advance;
            }
            h.insert(x.code, {adv, p});
        }
        return h;
    }();
    const int idx = c.unicode() - 32;
    QPainterPath g;
    if (idx >= 0 && idx < 95) {
        g = upright.at(idx);
        if (advance)
            *advance = kGlyphs[idx].advance;
    } else if (extras.contains(c.unicode())) {
        const auto &x = extras.value(c.unicode());
        g = x.second;
        if (advance)
            *advance = x.first;
    } else {
        if (advance)
            *advance = kGlyphs[0].advance;
        return {};
    }
    if (family == italicFamily()) {
        // Slanted about the baseline (about 12 degrees), shifted so the
        // x-height sits where the upright letter's does.
        QTransform t(1, 0, 0.21, 1, -0.21 * 3, 0);
        return t.map(g);
    }
    return g;
}

} // namespace c2d::strokefont
