#include "post_grbl.h"
#include <QStringList>

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

QString GrblPost::generate(const QVector<Op> &ops) const
{
    const int coordDigits = m_metric ? 3 : 4;
    Modal X{QStringLiteral("X"), coordDigits, {}};
    Modal Y{QStringLiteral("Y"), coordDigits, {}};
    Modal Z{QStringLiteral("Z"), coordDigits, {}};
    Modal F{QStringLiteral("F"), 1, {}};
    Modal G{QStringLiteral("G"), 0, {}};

    QStringList out;
    auto put = [&](const QString &s) { out << s; };

    // onOpen
    put(QStringLiteral("G90"));
    put(m_metric ? QStringLiteral("G21") : QStringLiteral("G20"));

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
            put(QStringLiteral("(") + t + QStringLiteral(")"));
            break;
        }
        case Op::Spindle: {
            if (op.ival <= 0) {
                if (spindleOn) { put(QStringLiteral("M05")); spindleOn = false; }
            } else if (!spindleOn || op.ival != lastRpm) {
                QString ln;
                if (!spindleOn) ln += QStringLiteral("M03");
                ln += QStringLiteral("S") + QString::number(op.ival);
                put(ln);
                spindleOn = true;
                lastRpm = op.ival;
            }
            break;
        }
        case Op::Tool: {
            if (op.ival != lastTool) {
                lastTool = op.ival;
                if (spindleOn) { put(QStringLiteral("M05")); spindleOn = false; }
                // GRBL post: pause for a manual tool change, tool number in comment.
                put(QStringLiteral("M0 ;T") + QString::number(op.ival));
            }
            break;
        }
        case Op::Rapid:
            put(block({G.fmt(0), X.fmt(scale(op.x)), Y.fmt(scale(op.y)), Z.fmt(scale(op.z))}));
            break;
        case Op::Feed:
            put(block({G.fmt(1), X.fmt(scale(op.x)), Y.fmt(scale(op.y)),
                        Z.fmt(scale(op.z)), F.fmt(scale(op.feed))}));
            break;
        }
    }

    // onClose
    if (spindleOn)
        put(QStringLiteral("M05"));
    put(QStringLiteral("M02"));

    return out.join(QChar('\n')) + QChar('\n');
}

} // namespace c2d
