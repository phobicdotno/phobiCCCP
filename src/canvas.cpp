#include "canvas.h"
#include <QGraphicsScene>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QPen>
#include <QWheelEvent>

namespace c2d {

Canvas::Canvas(QWidget *parent)
    : QGraphicsView(parent), m_scene(new QGraphicsScene(this))
{
    setScene(m_scene);
    setRenderHint(QPainter::Antialiasing, true);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setBackgroundBrush(QColor(0x20, 0x22, 0x28));
    // Flip Y so CC's Y-up space maps to Qt's Y-down scene.
    scale(1.0, -1.0);
}

void Canvas::setDocument(const Document *doc)
{
    m_scene->clear();
    if (!doc)
        return;

    const double w = doc->boardWidth();
    const double h = doc->boardHeight();

    // Board outline.
    if (w > 0 && h > 0) {
        auto *board = m_scene->addRect(QRectF(0, 0, w, h),
                                       QPen(QColor(0x8a, 0x94, 0xa6), 0),
                                       QBrush(QColor(0x2b, 0x2e, 0x37)));
        board->setZValue(-1);
    }

    QPen elemPen(QColor(0xe6, 0xe8, 0xec));
    elemPen.setCosmetic(true);   // constant on-screen width regardless of zoom
    elemPen.setWidthF(1.2);

    for (const Element &e : doc->elements()) {
        auto *item = m_scene->addPath(e.painterPath, elemPen);
        item->setToolTip(QStringLiteral("%1  %2").arg(e.geometryType, e.id));
    }

    if (w > 0 && h > 0)
        fitInView(QRectF(-10, -10, w + 20, h + 20), Qt::KeepAspectRatio);
}

void Canvas::wheelEvent(QWheelEvent *event)
{
    const double factor = event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    scale(factor, factor);
}

} // namespace c2d
