#pragma once
#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>

namespace c2d {

// The phobiCCCP logo (desktop/phobicccp.svg) drawn with QPainter, so it shows
// without the Qt SVG module: a red rounded square, a faint grid, a dashed
// construction circle with a pentagram, and the gold star.
inline QPixmap brandLogo(int px, qreal dpr)
{
    QPixmap pm(QSize(px, px) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal k = px / 128.0;
    p.scale(k, k);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0xcc, 0x00, 0x00));
    p.drawRoundedRect(QRectF(0, 0, 128, 128), 22, 22);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0xa3, 0x00, 0x00), 1.0));
    for (int v : {32, 64, 96}) {
        p.drawLine(QPointF(16, v), QPointF(112, v));
        p.drawLine(QPointF(v, 16), QPointF(v, 112));
    }
    QPen dash(QColor(0xff, 0x8a, 0x80), 1.5, Qt::CustomDashLine, Qt::RoundCap);
    dash.setDashPattern({4, 3});
    p.setPen(dash);
    p.drawEllipse(QPointF(64, 66), 42, 42);
    const QPolygonF pent({QPointF(64.0, 24.0), QPointF(88.7, 100.0), QPointF(24.1, 53.0),
                          QPointF(103.9, 53.0), QPointF(39.3, 100.0)});
    QPainterPath pp;
    pp.addPolygon(pent);
    pp.closeSubpath();
    p.drawPath(pp);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0xff, 0xd7, 0x00));
    p.drawPolygon(QPolygonF({QPointF(64.0, 24.0), QPointF(74.0, 52.2), QPointF(103.9, 53.0),
                             QPointF(80.2, 71.3), QPointF(88.7, 100.0), QPointF(64.0, 83.0),
                             QPointF(39.3, 100.0), QPointF(47.8, 71.3), QPointF(24.1, 53.0),
                             QPointF(54.0, 52.2)}));
    return pm;
}

} // namespace c2d
