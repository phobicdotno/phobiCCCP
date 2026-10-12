#include "tooldiagram.h"
#include "toollibrary.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include <QRegularExpression>

#include <cmath>

namespace c2d {

namespace {

const QColor kLine(0xc5, 0xca, 0xd3);
const QColor kFlute(0x6b, 0x73, 0x80);
const QColor kDim(0xf2, 0xa5, 0x36);
const QColor kText(0xe6, 0xe9, 0xef);
constexpr double kRoundR = 22;   // drawn round-over radius

QString mm(double v)
{
    QString s = QString::number(v, 'f', 2);
    while (s.endsWith(QLatin1Char('0')))
        s.chop(1);
    if (s.endsWith(QLatin1Char('.')))
        s.chop(1);
    return s;
}

void arrowHead(QPainter &p, const QPointF &tip, const QPointF &from)
{
    const QPointF d = tip - from;
    const double len = std::hypot(d.x(), d.y());
    if (len < 1e-6)
        return;
    const QPointF u = d / len, n(-u.y(), u.x());
    const double a = 6, w = 2.6;
    p.drawPolygon(QPolygonF({tip, tip - u * a + n * w, tip - u * a - n * w}));
}

// Dimension line from a to b with arrows both ends and the text centred
// beside it (above a horizontal one, left of a vertical one).
void dimLine(QPainter &p, const QPointF &a, const QPointF &b, const QString &text)
{
    p.setPen(QPen(kDim, 1.1));
    p.setBrush(kDim);
    p.drawLine(a, b);
    arrowHead(p, a, b);
    arrowHead(p, b, a);
    p.setPen(kText);
    const QPointF c = (a + b) / 2;
    const bool vertical = std::abs(b.x() - a.x()) < std::abs(b.y() - a.y());
    const QFontMetricsF fm(p.font());
    const double w = fm.horizontalAdvance(text);
    if (vertical) {
        p.save();
        p.translate(c.x() - 5, c.y() + w / 2);
        p.rotate(-90);
        p.drawText(QPointF(0, 0), text);
        p.restore();
    } else {
        p.drawText(QPointF(c.x() - w / 2, c.y() - 5), text);
    }
}

void extension(QPainter &p, const QPointF &a, const QPointF &b)
{
    p.setPen(QPen(kDim, 0.8, Qt::DashLine));
    p.drawLine(a, b);
}

} // namespace

// Canonical drawing space: 200 wide, 300 tall, tool axis at x = 100, shank
// from y = 20 down to y = 150, the cutting end from 150 to 270.
void paintToolDiagram(QPainter &p, const QRectF &box, const LibraryTool &t, bool dims)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const double k = std::min(box.width() / 200.0, box.height() / 300.0);
    p.translate(box.center().x() - 100 * k, box.center().y() - 150 * k);
    p.scale(k, k);
    const double lineW = dims ? 1.6 : 2.4 / std::max(k, 0.05) * 0.5;

    const QString kind = t.kind;
    const double sh = 30;                    // shank half-width
    const double top = 20, joint = 150, tip = 270;
    QPainterPath out;
    QList<QLineF> flutes;
    double cutHalf = 24;                     // drawn cutting radius
    // shank
    out.moveTo(100 - sh, top);
    out.lineTo(100 + sh, top);
    if (kind == QLatin1String("vbit") || kind == QLatin1String("engraver")) {
        cutHalf = sh;
        out.lineTo(100 + sh, joint);
        const double flat = kind == QLatin1String("engraver") ? 4 : 0;
        out.lineTo(100 + flat, tip);
        if (flat > 0)
            out.lineTo(100 - flat, tip);
        out.lineTo(100 - sh, joint);
        out.closeSubpath();
        flutes << QLineF(100 - sh * 0.55, joint + 10, 100 + 8, tip - 30)
               << QLineF(100 + sh * 0.2, joint + 6, 100 + sh * 0.7, joint + 40);
    } else if (kind == QLatin1String("roundover")) {
        // shank, the head, then the concave quarter arcs down to a small tip
        // Each side is a quarter circle centred on the head's corner, so it
        // curves inward: that hollow is what leaves the rounded edge.
        const double R = kRoundR, tipHalf = sh - R;
        out.lineTo(100 + sh, tip - R);
        out.arcTo(QRectF(100 + sh - R, tip - R, 2 * R, 2 * R), 90, 90);
        out.lineTo(100 - tipHalf, tip);
        out.arcTo(QRectF(100 - sh - R, tip - R, 2 * R, 2 * R), 0, 90);
        out.closeSubpath();
        cutHalf = tipHalf;
        flutes << QLineF(100 - sh + 6, joint + 10, 100 + sh - 10, joint + 50)
               << QLineF(100 - sh + 6, joint + 46, 100 + sh - 10, joint + 86);
    } else {
        // end mill / ball: a neck down to the cutting diameter
        out.lineTo(100 + sh, joint - 20);
        out.lineTo(100 + cutHalf, joint);
        if (kind == QLatin1String("ball")) {
            out.lineTo(100 + cutHalf, tip - cutHalf);
            out.arcTo(QRectF(100 - cutHalf, tip - 2 * cutHalf, 2 * cutHalf, 2 * cutHalf), 0, -180);
        } else {
            out.lineTo(100 + cutHalf, tip);
            out.lineTo(100 - cutHalf, tip);
        }
        out.lineTo(100 - cutHalf, joint);
        out.lineTo(100 - sh, joint - 20);
        out.closeSubpath();
        for (int i = 0; i < 3; ++i) {
            const double y = joint + 12 + i * 32;
            flutes << QLineF(100 - cutHalf, y, 100 + cutHalf, y + 26);
        }
    }

    p.setPen(QPen(kLine, lineW, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(QColor(255, 255, 255, 14));
    p.drawPath(out);
    p.setPen(QPen(kFlute, lineW * 0.8));
    for (const QLineF &l : flutes)
        p.drawLine(l);
    // the axis
    p.setPen(QPen(kFlute, 0.8, Qt::DashDotLine));
    if (dims)
        p.drawLine(QPointF(100, top - 12), QPointF(100, tip + 8));

    if (dims) {
        QFont f = p.font();
        f.setFamily(QStringLiteral("monospace"));
        f.setPixelSize(12);
        p.setFont(f);
        // shank Ø across the top
        dimLine(p, QPointF(100 - sh, top - 8), QPointF(100 + sh, top - 8),
                QStringLiteral("shank Ø%1").arg(mm(t.shank > 0 ? t.shank : t.diameter)));
        // cutting Ø under the tip
        const double y = tip + 18;
        const double half = kind == QLatin1String("vbit") ? sh : std::max(cutHalf, 10.0);
        extension(p, QPointF(100 - half, kind == QLatin1String("vbit") ? joint : tip),
                  QPointF(100 - half, y + 3));
        extension(p, QPointF(100 + half, kind == QLatin1String("vbit") ? joint : tip),
                  QPointF(100 + half, y + 3));
        dimLine(p, QPointF(100 - half, y), QPointF(100 + half, y),
                QStringLiteral("Ø%1").arg(mm(t.diameter)));
        // cut length on the right
        if (t.length > 0) {
            const double x = 100 + sh + 26;
            extension(p, QPointF(100 + sh, joint), QPointF(x + 3, joint));
            extension(p, QPointF(100 + cutHalf, tip), QPointF(x + 3, tip));
            dimLine(p, QPointF(x, joint), QPointF(x, tip),
                    QStringLiteral("L %1").arg(mm(t.length)));
        }
        // angle / radius
        p.setPen(QPen(kDim, 1.1));
        p.setBrush(Qt::NoBrush);
        if (kind == QLatin1String("vbit") || kind == QLatin1String("engraver")) {
            if (t.angle > 0) {
                const QPointF apex(100, tip);
                const double a = std::atan2(tip - joint, sh) * 180 / M_PI;   // side vs horizontal
                p.drawArc(QRectF(apex.x() - 40, apex.y() - 40, 80, 80), int((a) * 16),
                          int((180 - 2 * a) * 16));
                p.setPen(kText);
                p.drawText(QRectF(70, tip - 64, 60, 16), Qt::AlignCenter,
                           QStringLiteral("%1°").arg(mm(t.angle)));
            }
        } else if (kind == QLatin1String("ball")) {
            const QPointF c(100, tip - cutHalf);
            const QPointF onArc = c + QPointF(-cutHalf * 0.7071, cutHalf * 0.7071);
            p.drawLine(c, onArc);
            p.drawLine(onArc, onArc + QPointF(-20, 0));
            p.setPen(kText);
            const QString rText = QStringLiteral("R%1").arg(mm(t.diameter / 2));
            p.drawText(QPointF(onArc.x() - 24 - QFontMetricsF(p.font()).horizontalAdvance(rText),
                               onArc.y() + 4),
                       rText);
        } else if (kind == QLatin1String("roundover")) {
            // the round-over radius comes from the model name (R4.0) or notes
            QString r;
            const QRegularExpressionMatch m =
                QRegularExpression(QStringLiteral("R(\\d+(?:\\.\\d+)?)")).match(t.model);
            if (m.hasMatch())
                r = mm(m.captured(1).toDouble());
            // leader from the arc's centre (the head corner) to its middle
            const QPointF c(100 - sh, tip);
            const QPointF onArc = c + QPointF(kRoundR * 0.7071, -kRoundR * 0.7071);
            // from below and to the left, pointing up into the bend
            const QPointF from(100 - sh - 30, tip + 8);
            p.drawLine(from, onArc);
            arrowHead(p, onArc, from);
            p.setPen(kText);
            p.drawText(QPointF(from.x() - 26, from.y() + 12),
                       r.isEmpty() ? QStringLiteral("R") : QStringLiteral("R%1").arg(r));
        }
    }
    p.restore();
}

QIcon toolKindIcon(const LibraryTool &t)
{
    QPixmap pm(QSize(24, 36) * 3);
    pm.setDevicePixelRatio(3);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    paintToolDiagram(p, QRectF(0, 0, 24, 36), t, false);
    return QIcon(pm);
}

ToolDiagram::ToolDiagram(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(200, 260);
}

void ToolDiagram::setTool(const LibraryTool *t)
{
    m_tool = t;
    update();
}

void ToolDiagram::paintEvent(QPaintEvent *)
{
    if (!m_tool)
        return;
    QPainter p(this);
    paintToolDiagram(p, QRectF(rect()).adjusted(34, 6, -34, -6), *m_tool, true);
}

} // namespace c2d
