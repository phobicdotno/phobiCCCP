#include "canvas.h"
#include <QGraphicsScene>
#include <QGraphicsPathItem>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QWheelEvent>
#include <QtMath>

namespace c2d {

Canvas::Canvas(QWidget *parent)
    : QGraphicsView(parent), m_scene(new QGraphicsScene(this))
{
    setScene(m_scene);
    setRenderHint(QPainter::Antialiasing, true);
    setDragMode(QGraphicsView::RubberBandDrag);
    setBackgroundBrush(QColor(0x20, 0x22, 0x28));
    // Flip Y so CC's Y-up space maps to Qt's Y-down scene.
    scale(1.0, -1.0);
}

void Canvas::setDocument(Document *doc)
{
    m_doc = doc;
    m_fitted = false;
    rebuild();
}

void Canvas::setTool(Tool t)
{
    m_tool = t;
    m_drawing = false;
    setDragMode(t == Select ? QGraphicsView::RubberBandDrag
                            : QGraphicsView::NoDrag);
    const bool editable = (t == Select);
    for (QGraphicsItem *it : m_scene->items()) {
        if (it->data(0).isValid()) {
            it->setFlag(QGraphicsItem::ItemIsSelectable, editable);
            it->setFlag(QGraphicsItem::ItemIsMovable, editable);
        }
    }
    switch (t) {
    case Select:      emit statusHint(tr("Select: click/drag to select, drag to move, Del to delete")); break;
    case DrawCircle:  emit statusHint(tr("Circle: press at center, drag to radius")); break;
    case DrawRect:    emit statusHint(tr("Rectangle: drag corner to corner")); break;
    case DrawPolygon: emit statusHint(tr("Polygon: press at center, drag to radius")); break;
    }
}

void Canvas::rebuild()
{
    m_preview = nullptr;           // owned by the scene; clear() deletes it
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

    const bool editable = (m_tool == Select);
    for (const Element &e : m_doc->elements()) {
        auto *item = m_scene->addPath(e.painterPath, elemPen);
        item->setToolTip(QStringLiteral("%1  %2").arg(e.geometryType, e.id));
        item->setData(0, e.id);                       // scene item -> element
        item->setFlag(QGraphicsItem::ItemIsSelectable, editable);
        item->setFlag(QGraphicsItem::ItemIsMovable, editable);
    }

    if (!m_fitted && w > 0 && h > 0) {
        fitInView(QRectF(-10, -10, w + 20, h + 20), Qt::KeepAspectRatio);
        m_fitted = true;
    }
}

QPainterPath Canvas::previewPath(const QPointF &cur) const
{
    QPainterPath p;
    switch (m_tool) {
    case DrawCircle: {
        const double r = QLineF(m_anchor, cur).length();
        p.addEllipse(m_anchor, r, r);
        break;
    }
    case DrawRect:
        p.addRect(QRectF(m_anchor, cur).normalized());
        break;
    case DrawPolygon: {
        const double r = QLineF(m_anchor, cur).length();
        for (int i = 0; i <= m_polySides; ++i) {
            const double a = 2.0 * M_PI * i / m_polySides;
            const QPointF v = m_anchor + QPointF(r * qCos(a), r * qSin(a));
            if (i == 0) p.moveTo(v); else p.lineTo(v);
        }
        break;
    }
    default:
        break;
    }
    return p;
}

void Canvas::mousePressEvent(QMouseEvent *event)
{
    if (m_tool != Select && m_doc && event->button() == Qt::LeftButton) {
        m_drawing = true;
        m_anchor = mapToScene(event->pos());
        QPen pen(QColor(0x62, 0xc4, 0x62));
        pen.setCosmetic(true);
        pen.setStyle(Qt::DashLine);
        m_preview = m_scene->addPath(QPainterPath(), pen);
        event->accept();
        return;
    }
    QGraphicsView::mousePressEvent(event);
}

void Canvas::mouseMoveEvent(QMouseEvent *event)
{
    if (m_drawing && m_preview) {
        m_preview->setPath(previewPath(mapToScene(event->pos())));
        event->accept();
        return;
    }
    QGraphicsView::mouseMoveEvent(event);
}

void Canvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_drawing && m_doc) {
        m_drawing = false;
        const QPointF cur = mapToScene(event->pos());
        if (m_preview) { m_scene->removeItem(m_preview); delete m_preview; m_preview = nullptr; }

        const QJsonObject layer = m_doc->defaultLayer();
        const double r = QLineF(m_anchor, cur).length();
        bool made = false;
        switch (m_tool) {
        case DrawCircle:
            if (r > 0.1) { m_doc->addElement(Element::makeCircle(m_anchor, r, layer)); made = true; }
            break;
        case DrawRect: {
            const QRectF rect = QRectF(m_anchor, cur).normalized();
            if (rect.width() > 0.1 && rect.height() > 0.1) {
                m_doc->addElement(Element::makeRectangle(rect.center(),
                                                         rect.width(), rect.height(), layer));
                made = true;
            }
            break;
        }
        case DrawPolygon:
            if (r > 0.1) { m_doc->addElement(Element::makePolygon(m_anchor, r, m_polySides, layer)); made = true; }
            break;
        default:
            break;
        }
        if (made) {
            rebuild();
            emit documentChanged();
        }
        event->accept();
        return;
    }

    QGraphicsView::mouseReleaseEvent(event);

    // Select tool: commit any moved items back into the document. An item that
    // was dragged has a non-zero pos(); scene coords are CC mm, so the delta
    // applies directly.
    if (m_tool == Select && m_doc) {
        bool moved = false;
        for (QGraphicsItem *it : m_scene->selectedItems()) {
            const QPointF d = it->pos();
            if (!d.isNull() && it->data(0).isValid()) {
                if (Element *e = m_doc->elementById(it->data(0).toString())) {
                    e->translate(d.x(), d.y());
                    moved = true;
                }
            }
        }
        if (moved) {
            rebuild();
            emit documentChanged();
        }
    }
}

void Canvas::keyPressEvent(QKeyEvent *event)
{
    if (m_doc && (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)) {
        QStringList ids;
        for (QGraphicsItem *it : m_scene->selectedItems())
            if (it->data(0).isValid())
                ids << it->data(0).toString();
        if (!ids.isEmpty()) {
            for (const QString &id : ids)
                m_doc->removeElementById(id);
            rebuild();
            emit documentChanged();
            emit statusHint(tr("Deleted %1 element(s)").arg(ids.size()));
            return;
        }
    }
    QGraphicsView::keyPressEvent(event);
}

void Canvas::wheelEvent(QWheelEvent *event)
{
    const double factor = event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    scale(factor, factor);
}

} // namespace c2d
