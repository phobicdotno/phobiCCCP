#include "post_grbl.h"
#include <QRegularExpression>
#include <QStringList>
#include <cmath>

namespace c2d {

// Modal number formatter: emits `<name><value>` only when the value changes from
// the previous block, mirroring CC's per-axis "last output" memory.
namespace {
struct Modal {
    QString name;
    int digits;
    QString last;
    QString fmt(double v) {
        const QString s = QString::number(v, 'f', digits);
        if (s == last)
            return QString();
        last = s;
        return name + s;
    }
    void reset() { last = QStringLiteral("--"); }
};

QString block(std::initializer_list<QString> words) {
    QStringList parts;
    for (const QString &w : words)
        if (!w.isEmpty())
            parts << w;
    return parts.join(QString());
}
} // namespace

JobStats computeStats(const QVector<Op> &ops)
{
    JobStats s;
    const double kRapidFeed = 5000.0;   // mm/min, conservative for a Shapeoko
    const double kTau = 6.283185307179586;
    double px = 0, py = 0, pz = 0;
    bool have = false;
    auto grow = [&s](double x, double y, double z) {
        if (!s.hasBounds) {
            s.minX = s.maxX = x;
            s.minY = s.maxY = y;
            s.minZ = s.maxZ = z;
            s.hasBounds = true;
            return;
        }
        s.minX = qMin(s.minX, x); s.maxX = qMax(s.maxX, x);
        s.minY = qMin(s.minY, y); s.maxY = qMax(s.maxY, y);
        s.minZ = qMin(s.minZ, z); s.maxZ = qMax(s.maxZ, z);
    };
    for (const Op &op : ops) {
        if (op.kind != Op::Rapid && op.kind != Op::Feed && op.kind != Op::Arc)
            continue;
        grow(op.x, op.y, op.z);
        if (have) {
            double len = 0;
            if (op.kind == Op::Arc) {
                const double cx = px + op.ci, cy = py + op.cj;
                const double r = std::hypot(op.ci, op.cj);
                const double a0 = std::atan2(py - cy, px - cx);
                const double a1 = std::atan2(op.y - cy, op.x - cx);
                double sweep = op.cw ? a0 - a1 : a1 - a0;
                while (sweep <= 1e-9) sweep += kTau;   // coincident endpoints = full circle
                while (sweep > kTau)  sweep -= kTau;
                len = std::hypot(r * sweep, op.z - pz);
                // Sample the arc so a ring's full extents (not just its
                // endpoints) enter the bounds.
                for (int i = 1; i < 8; ++i) {
                    const double a = a0 + (op.cw ? -sweep : sweep) * i / 8.0;
                    grow(cx + r * std::cos(a), cy + r * std::sin(a), op.z);
                }
            } else {
                len = std::hypot(std::hypot(op.x - px, op.y - py), op.z - pz);
            }
            if (op.kind == Op::Rapid) {
                s.rapidLen += len;
                s.timeSec += len / kRapidFeed * 60.0;
            } else {
                s.cutLen += len;
                if (op.feed > 0)
                    s.timeSec += len / op.feed * 60.0;
            }
        }
        px = op.x; py = op.y; pz = op.z;
        have = true;
    }
    return s;
}

QString statsSummary(const JobStats &s)
{
    if (!s.hasBounds)
        return QStringLiteral("empty program");
    const int t = int(s.timeSec + 0.5);
    return QStringLiteral("X %1..%2  Y %3..%4  Z %5..%6 mm\n"
                          "cut %7 mm + rapid %8 mm, est. %9:%10 min")
        .arg(s.minX, 0, 'f', 1).arg(s.maxX, 0, 'f', 1)
        .arg(s.minY, 0, 'f', 1).arg(s.maxY, 0, 'f', 1)
        .arg(s.minZ, 0, 'f', 1).arg(s.maxZ, 0, 'f', 1)
        .arg(s.cutLen, 0, 'f', 0).arg(s.rapidLen, 0, 'f', 0)
        .arg(t / 60).arg(t % 60, 2, 10, QChar('0'));
}

QStringList airCutTransform(const QStringList &lines, double lift)
{
    static const QRegularExpression zWord(QStringLiteral("Z(-?[0-9]+\\.?[0-9]*)"));
    QStringList out;
    for (const QString &raw : lines) {
        const QString l = raw.trimmed();
        // Drop spindle control: M03/M04 starts, bare S rpm changes, M05 stops.
        // (`M0 ;T` tool-change pauses survive — they contain no motion.)
        if (l.startsWith(QLatin1String("M03")) || l.startsWith(QLatin1String("M04"))
            || l == QLatin1String("M05")
            || (l.size() > 1 && l.at(0) == QChar('S') && l.at(1).isDigit()))
            continue;
        if (l.startsWith(QChar('(')) || l.startsWith(QChar(';'))) {
            out << raw;   // comments untouched — a name could contain "Z1"
            continue;
        }
        QString shifted;
        int pos = 0;
        auto it = zWord.globalMatch(raw);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            shifted += raw.mid(pos, m.capturedStart() - pos);
            shifted += QStringLiteral("Z")
                + QString::number(m.captured(1).toDouble() + lift, 'f', 3);
            pos = m.capturedEnd();
        }
        shifted += raw.mid(pos);
        out << shifted;
    }
    return out;
}

QString GrblPost::generate(const QVector<Op> &ops) const
{
    const int coordDigits = m_metric ? 3 : 4;
    Modal X{QStringLiteral("X"), coordDigits, {}};
    Modal Y{QStringLiteral("Y"), coordDigits, {}};
    Modal Z{QStringLiteral("Z"), coordDigits, {}};
    Modal F{QStringLiteral("F"), 1, {}};
    Modal G{QStringLiteral("G"), 0, {}};

    QStringList out;
    // NB: `emit` is a Qt keyword macro (empty), so the lambda is named `line`.
    auto line = [&](const QString &s) { out << s; };

    // onOpen
    line(QStringLiteral("G90"));
    line(m_metric ? QStringLiteral("G21") : QStringLiteral("G20"));

    bool spindleOn = false;
    int lastRpm = -1;
    int lastTool = -1;

    auto scale = [&](double mm) { return m_metric ? mm : mm / 25.4; };

    for (const Op &op : ops) {
        switch (op.kind) {
        case Op::Comment: {
            QString t = op.text;
            if (t.length() > 12)
                t = t.left(9) + QStringLiteral("...");
            line(QStringLiteral("(") + t + QStringLiteral(")"));
            break;
        }
        case Op::Spindle: {
            if (op.ival <= 0) {
                if (spindleOn) { line(QStringLiteral("M05")); spindleOn = false; }
            } else if (!spindleOn || op.ival != lastRpm) {
                QString ln;
                if (!spindleOn) ln += QStringLiteral("M03");
                ln += QStringLiteral("S") + QString::number(op.ival);
                line(ln);
                spindleOn = true;
                lastRpm = op.ival;
            }
            break;
        }
        case Op::Tool: {
            if (op.ival != lastTool) {
                lastTool = op.ival;
                if (spindleOn) { line(QStringLiteral("M05")); spindleOn = false; }
                // GRBL post: pause for a manual tool change, tool number in comment.
                line(QStringLiteral("M0 ;T") + QString::number(op.ival));
            }
            break;
        }
        case Op::Rapid:
            line(block({G.fmt(0), X.fmt(scale(op.x)), Y.fmt(scale(op.y)), Z.fmt(scale(op.z))}));
            break;
        case Op::Feed:
            line(block({G.fmt(1), X.fmt(scale(op.x)), Y.fmt(scale(op.y)),
                        Z.fmt(scale(op.z)), F.fmt(scale(op.feed))}));
            break;
        case Op::Arc: {
            // I/J are relative center offsets and must always be emitted; the
            // endpoint X/Y must also always be present (a modal-suppressed X/Y
            // on an arc would leave the target ambiguous), so bypass the modal
            // memory for them but keep it updated.
            const int cd = m_metric ? 3 : 4;
            QString ln = G.fmt(op.cw ? 2 : 3)
                + QStringLiteral("X") + QString::number(scale(op.x), 'f', cd)
                + QStringLiteral("Y") + QString::number(scale(op.y), 'f', cd);
            X.last = QString::number(scale(op.x), 'f', cd);
            Y.last = QString::number(scale(op.y), 'f', cd);
            const QString zw = Z.fmt(scale(op.z));
            ln += zw;
            ln += QStringLiteral("I") + QString::number(scale(op.ci), 'f', cd)
                + QStringLiteral("J") + QString::number(scale(op.cj), 'f', cd)
                + F.fmt(scale(op.feed));
            line(ln);
            break;
        }
        }
    }

    // onClose
    if (spindleOn)
        line(QStringLiteral("M05"));
    line(QStringLiteral("M02"));

    return out.join(QChar('\n')) + QChar('\n');
}

} // namespace c2d
