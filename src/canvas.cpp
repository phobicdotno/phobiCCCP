#include "canvas.h"
#include <QGraphicsScene>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QScrollBar>
#include <QWheelEvent>
#include <algorithm>

namespace c2d {

Canvas::Canvas(QWidget *parent)
    : QGraphicsView(parent), m_scene(new QGraphicsScene(this))
{
    setScene(m_scene);
    setRenderHint(QPainter::Antialiasing, true);
    // Left button selects (rubber band on empty space) and drags items;
    // panning is on the middle button.
    setDragMode(QGraphicsView::RubberBandDrag);
    setBackgroundBrush(QColor(0x20, 0x22, 0x28));
    // Flip Y so CC's Y-up space maps to Qt's Y-down scene.
    scale(1.0, -1.0);
}

void Canvas::setDocument(const Document *doc)
{
    m_doc = doc;
    rebuild(true);
}

void Canvas::refresh()
{
    rebuild(false);
}

void Canvas::rebuild(bool refit)
{
    m_scene->clear();
    if (!m_doc)
        return;

    const double w = m_doc->boardWidth();
    const double h = m_doc->boardHeight();

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

    const QVector<Element> &elems = m_doc->elements();
    for (int i = 0; i < elems.size(); ++i) {
        const Element &e = elems.at(i);
        auto *item = m_scene->addPath(e.painterPath, elemPen);
        item->setToolTip(QStringLiteral("%1  %2").arg(e.geometryType, e.id));
        item->setFlags(QGraphicsItem::ItemIsSelectable | QGraphicsItem::ItemIsMovable);
        item->setData(0, i);
    }

    if (refit && w > 0 && h > 0)
        fitInView(QRectF(-10, -10, w + 20, h + 20), Qt::KeepAspectRatio);
}

QList<int> Canvas::selectedElements() const
{
    QList<int> out;
    const QList<QGraphicsItem *> sel = m_scene->selectedItems();
    for (QGraphicsItem *item : sel)
        out.append(item->data(0).toInt());
    std::sort(out.begin(), out.end());
    return out;
}

void Canvas::wheelEvent(QWheelEvent *event)
{
    const double factor = event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    scale(factor, factor);
}

void Canvas::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::MiddleButton) {
        m_panning = true;
        m_panStart = event->pos();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QGraphicsView::mousePressEvent(event);
}

void Canvas::mouseMoveEvent(QMouseEvent *event)
{
    if (m_panning) {
        const QPoint d = event->pos() - m_panStart;
        m_panStart = event->pos();
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - d.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - d.y());
        event->accept();
        return;
    }
    QGraphicsView::mouseMoveEvent(event);
}

void Canvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_panning && event->button() == Qt::MiddleButton) {
        m_panning = false;
        unsetCursor();
        event->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);

    // Commit finished drags: a moved item has a nonzero pos() offset in scene
    // coordinates (mm, Y-up already thanks to the flipped view transform).
    // elementMoved() is applied to the model synchronously, so the item can be
    // re-based onto the mutated path at pos 0 (a second drag then starts clean).
    const QList<QGraphicsItem *> items = m_scene->items();
    for (QGraphicsItem *item : items) {
        if (item->data(0).isNull())
            continue;
        if (!item->flags().testFlag(QGraphicsItem::ItemIsMovable))
            continue;
        const QPointF d = item->pos();
        if (d.manhattanLength() > 1e-9) {
            const int idx = item->data(0).toInt();
            emit elementMoved(idx, d);
            if (auto *pi = qgraphicsitem_cast<QGraphicsPathItem *>(item)) {
                pi->setPath(m_doc->elements().at(idx).painterPath);
                pi->setPos(0, 0);
            }
        }
    }
}

} // namespace c2d
