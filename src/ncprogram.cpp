#include "ncprogram.h"

#include <QRegularExpression>

#include <cmath>

namespace c2d {

namespace {

// G codes this sender passes through. G28/G30 go to the controller's own
// stored spots; G53 is a machine-coordinate move; G91.1 is GRBL's default.
bool gAllowed(int g10)
{
    switch (g10) {
    case 0: case 10: case 20: case 30: case 40: case 170: case 210: case 280: case 300:
    case 400: case 530: case 540: case 610: case 800: case 900: case 911: case 940:
        return true;
    default:
        return false;
    }
}

QString gRefused(int g10)
{
    const QString g = QStringLiteral("G%1").arg(g10 % 10 ? QString::number(g10 / 10.0, 'f', 1)
                                                         : QString::number(g10 / 10));
    switch (g10) {
    case 200: return QStringLiteral("G20: the program is in inches. Post it in millimetres (G21).");
    case 180: case 190:
        return QStringLiteral("%1: arcs in the XZ or YZ plane. Only arcs flat in XY (G17) can run here.").arg(g);
    case 410: case 420:
        return QStringLiteral("%1: cutter compensation. GRBL does not have it; post the toolpath with the "
                              "offset already in it.").arg(g);
    case 910: return QStringLiteral("G91: incremental moves. Post the program in absolute (G90).");
    case 920: case 921: case 922: case 923:
        return QStringLiteral("%1: it shifts the work zero. Set the zero on the Machine page instead.").arg(g);
    case 100: return QStringLiteral("G10: it writes offsets. Set the zero on the Machine page instead.");
    case 431: case 490:
        return QStringLiteral("%1: tool length offset. The BitSetter keeps that here.").arg(g);
    case 550: case 560: case 570: case 580: case 590:
        return QStringLiteral("%1: another work zero. This page runs on G54, the zero set here.").arg(g);
    case 930: return QStringLiteral("G93: inverse-time feed. Post it with feeds in mm/min (G94).");
    case 640: return QStringLiteral("G64: path blending. GRBL does not have it.");
    case 281: case 301:
        return QStringLiteral("%1: it stores a position in the controller.").arg(g);
    default:
        if (g10 >= 382 && g10 <= 385)
            return QStringLiteral("%1: probing. Use the BitZero page for that.").arg(g);
        if (g10 >= 810 && g10 <= 890)
            return QStringLiteral("%1: a drilling cycle. GRBL does not have canned cycles; post the "
                                  "holes as plain moves.").arg(g);
        return QStringLiteral("%1: GRBL does not run it.").arg(g);
    }
}


} // namespace

NcProgram readNcProgram(const QString &text)
{
    NcProgram p;
    static const QRegularExpression paren(QStringLiteral("\\([^)]*\\)"));
    static const QRegularExpression word(QStringLiteral("^([A-Z])\\s*([-+]?(?:[0-9]+\\.?[0-9]*|\\.[0-9]+))"));
    const QStringList raw = text.split(QLatin1Char('\n'));
    int motion = 0;
    double x = 0, y = 0, z = 0, feed = 0;
    bool haveFeed = false;
    int pendingTool = -1;
    double lastS = 0;
    auto fail = [&](int n, const QString &why) {
        p.error = why;
        p.errorLine = n;
        p.lines.clear();
        p.ops.clear();
        return p;
    };
    for (int n = 0; n < raw.size(); ++n) {
        QString l = raw.at(n);
        const int semi = l.indexOf(QLatin1Char(';'));
        if (semi >= 0)
            l.truncate(semi);
        l.remove(paren);
        l = l.trimmed().toUpper();
        if (l.isEmpty() || l == QLatin1String("%"))
            continue;
        if (l.contains(QLatin1Char('(')))
            return fail(n + 1, QStringLiteral("A comment is not closed with ')'."));
        if (l.startsWith(QLatin1Char('$')))
            return fail(n + 1, QStringLiteral("'%1': a controller command, not G-code. Settings are "
                                              "changed on the Machine page.").arg(l));
        if (l.startsWith(QLatin1Char('/')))
            return fail(n + 1, QStringLiteral("Block delete ('/'): GRBL does not have it."));
        // The words, in order.
        QVector<QPair<QChar, QString>> words;
        QString rest = l;
        while (!rest.isEmpty()) {
            const QRegularExpressionMatch m = word.match(rest);
            if (!m.hasMatch())
                return fail(n + 1, QStringLiteral("'%1' cannot be read as G-code.").arg(raw.at(n).trimmed()));
            words.append({m.captured(1).at(0), m.captured(2)});
            rest = rest.mid(m.capturedLength()).trimmed();
        }
        QStringList out;
        bool m6 = false, end = false, g53 = false, home = false, dwell = false;
        int tool = -1;
        bool hasX = false, hasY = false, hasZ = false, hasI = false, hasJ = false, hasR = false;
        double nx = x, ny = y, nz = z, ci = 0, cj = 0, r = 0, pSec = 0;
        int spindle = -1;   // 1 on, 0 off
        int spindleM = 3;
        double s = -1;
        QString sText;
        for (const auto &w : words) {
            const QChar c = w.first;
            const double v = w.second.toDouble();
            // Numbers as the sender's own transforms read them: no '+', a
            // digit before the point (the air cut lifts "Z0.5", not "Z.5").
            QString val = w.second;
            if (val.startsWith(QLatin1Char('+')))
                val.remove(0, 1);
            if (val.startsWith(QLatin1Char('.')))
                val.prepend(QLatin1Char('0'));
            else if (val.startsWith(QLatin1String("-.")))
                val.insert(1, QLatin1Char('0'));
            switch (c.toLatin1()) {
            case 'N':
                continue;   // line numbers: nothing to the controller
            case 'G': {
                const int g10 = int(std::lround(v * 10));
                if (!gAllowed(g10))
                    return fail(n + 1, gRefused(g10));
                if (g10 <= 30 && g10 % 10 == 0)
                    motion = g10 / 10;
                if (g10 == 40)
                    dwell = true;
                if (g10 == 530)
                    g53 = true;
                if (g10 == 280 || g10 == 300)
                    home = true;
                out << QStringLiteral("G") + val;
                continue;
            }
            case 'M': {
                const int m = int(std::lround(v));
                if (m == 6) {
                    m6 = true;
                    continue;
                }
                if (m == 2 || m == 30) {
                    end = true;
                    continue;
                }
                if (m == 0 || m == 1)
                    return fail(n + 1, QStringLiteral("M%1: a program pause. This page pauses only for "
                                                      "tool changes (T and M6); take it out, or split "
                                                      "the file there.").arg(m));
                if (m != 3 && m != 4 && m != 5 && m != 7 && m != 8 && m != 9)
                    return fail(n + 1, QStringLiteral("M%1: GRBL does not run it.").arg(m));
                if (m == 3 || m == 4) {
                    spindle = 1;
                    spindleM = m;
                } else if (m == 5) {
                    spindle = 0;
                } else {
                    out << QStringLiteral("M") + val;   // coolant
                }
                continue;
            }
            case 'T':
                tool = int(std::lround(v));
                continue;
            case 'X': nx = v; hasX = true; break;
            case 'Y': ny = v; hasY = true; break;
            case 'Z': nz = v; hasZ = true; break;
            case 'I': ci = v; hasI = true; break;
            case 'J': cj = v; hasJ = true; break;
            case 'R': r = v; hasR = true; break;
            case 'F': feed = v; haveFeed = v > 0; break;
            case 'S': s = v; sText = val; continue;
            case 'P': pSec = v; break;
            default:
                return fail(n + 1, QStringLiteral("The %1 word: only X, Y and Z axes (and the I, J or R of an "
                                                  "arc in XY) run here.").arg(c));
            }
            out << QString(c) + val;
        }
        if (tool >= 0)
            pendingTool = tool;
        if (m6) {
            if (pendingTool < 0)
                return fail(n + 1, QStringLiteral("M6 without a tool number (T)."));
            p.lines << QStringLiteral("M0 ;T%1").arg(pendingTool)
                    << QStringLiteral("(Tool %1)").arg(pendingTool);
            p.ops << Op::tool(pendingTool);
        }
        if (s >= 0)
            lastS = s;
        if (s >= 0 && spindle < 0)
            spindle = -2;   // a speed change only
        if (spindle == 1 || spindle == -2)
            p.ops << Op::spindle(int(std::lround(lastS)));
        else if (spindle == 0)
            p.ops << Op::spindle(0);
        if (dwell)
            p.ops << Op::dwell(pSec);
        const bool move = hasX || hasY || hasZ;
        if (move && !g53) {
            if (home) {
                // To the stored spot by way of this point: the point is in
                // work coordinates, where the machine ends up is not.
                p.ops << Op::rapid(nx, ny, nz);
            } else if (motion == 0) {
                p.ops << Op::rapid(nx, ny, nz);
            } else {
                if (!haveFeed)
                    return fail(n + 1, QStringLiteral("A cutting move before any feed rate (F)."));
                if (motion == 1) {
                    p.ops << Op::feedTo(nx, ny, nz, feed);
                } else {
                    if (hasR) {
                        // The centre from the radius: left of the chord for G3
                        // with R > 0, as GRBL takes it.
                        const double dx = nx - x, dy = ny - y, d = std::hypot(dx, dy);
                        if (d < 1e-9 || std::abs(r) * 2 < d - 1e-6)
                            return fail(n + 1, QStringLiteral("An arc whose radius cannot reach its end point."));
                        double h = std::sqrt(qMax(0.0, r * r - d * d / 4)) / d;
                        if ((motion == 2) != (r < 0))
                            h = -h;
                        ci = dx / 2 - h * dy;
                        cj = dy / 2 + h * dx;
                    } else if (!hasI && !hasJ) {
                        return fail(n + 1, QStringLiteral("An arc with neither I/J nor R."));
                    }
                    p.ops << Op::arcTo(nx, ny, nz, ci, cj, motion == 2, feed);
                }
            }
            if (!home) {
                x = nx;
                y = ny;
                z = nz;
            }
        }
        // The spindle on a line of its own, the way the post writes it, so
        // the air cut finds and drops it ("M03 S12000", "S9000", "M05").
        // GRBL starts the spindle before a block's move, so before it too.
        if (spindle == 1)
            p.lines << QStringLiteral("M0%1").arg(spindleM) + (sText.isEmpty() ? QString() : QStringLiteral(" S") + sText);
        else if (spindle == 0)
            p.lines << QStringLiteral("M05");
        else if (!sText.isEmpty())
            p.lines << QStringLiteral("S") + sText;
        if (!out.isEmpty())
            p.lines << out.join(QLatin1Char(' '));
        if (end)
            break;
    }
    if (p.ops.isEmpty())
        return fail(0, QStringLiteral("The file has no moves."));
    return p;
}

} // namespace c2d
