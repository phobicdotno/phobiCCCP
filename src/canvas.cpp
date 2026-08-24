#include "canvas.h"
#include <QGraphicsScene>
#include <QGraphicsPathItem>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QStyleOptionGraphicsItem>
#include <QUndoCommand>
#include <QUndoStack>
#include <QWheelEvent>
#include <QtMath>

namespace c2d {

// ---- palette -------------------------------------------------------------
static const QColor kViewBg(0x15, 0x17, 0x1c);
static const QColor kBoardFill(0x21, 0x24, 0x2b);
static const QColor kBoardEdge(0x4a, 0x52, 0x60);
static const QColor kGridMinor(0x28, 0x2c, 0x35);
static const QColor kGridMajor(0x34, 0x3a, 0x46);
static const QColor kAxisX(0x8a, 0x4a, 0x4a);
static const QColor kAxisY(0x4a, 0x7a, 0x4a);
static const QColor kElement(0xd8, 0xdc, 0xe4);
static const QColor kSelected(0xf2, 0xa5, 0x36);   // amber
static const QColor kPreview(0x5c, 0xc8, 0x8a);

// Path item that suppresses Qt's default dashed selection rectangle — the
// selection is shown by swapping the pen instead (applySelectionStyle).
class ElemItem : public QGraphicsPathItem
{
public:
    using QGraphicsPathItem::QGraphicsPathItem;
    void paint(QPainter *p, const QStyleOptionGraphicsItem *opt, QWidget *w) override
    {
        QStyleOptionGraphicsItem o(*opt);
        o.state &= ~QStyle::State_Selected;
        QGraphicsPathItem::paint(p, &o, w);
    }
};

// ---- undo commands -------------------------------------------------------
// Each command mutates the Document, then re-syncs the canvas and notifies.
namespace {

void refresh(Canvas *c)
{
    c->rebuild();
    emit c->documentChanged();
}

class AddCmd : public QUndoCommand
{
public:
    AddCmd(Canvas *c, Document *d, const Element &e)
        : m_c(c), m_d(d), m_e(e) { setText(QStringLiteral("add %1").arg(e.geometryType)); }
    void redo() override { m_d->addElement(m_e); refresh(m_c); }
    void undo() override { m_d->removeElementById(m_e.id); refresh(m_c); }
private:
    Canvas *m_c; Document *m_d; Element m_e;
};

class DeleteCmd : public QUndoCommand
{
public:
    DeleteCmd(Canvas *c, Document *d, const QVector<Element> &es)
        : m_c(c), m_d(d), m_es(es) { setText(QStringLiteral("delete %1 element(s)").arg(es.size())); }
    void redo() override { for (const Element &e : m_es) m_d->removeElementById(e.id); refresh(m_c); }
    void undo() override { for (const Element &e : m_es) m_d->addElement(e); refresh(m_c); }
private:
    Canvas *m_c; Document *m_d; QVector<Element> m_es;
};

class MoveCmd : public QUndoCommand
{
public:
    MoveCmd(Canvas *c, Document *d, const QVector<QPair<QString, QPointF>> &moves)
        : m_c(c), m_d(d), m_moves(moves) { setText(QStringLiteral("move %1 element(s)").arg(moves.size())); }
    void redo() override { apply(1.0); }
    void undo() override { apply(-1.0); }
private:
    void apply(double sign)
    {
        for (const auto &m : m_moves)
            if (Element *e = m_d->elementById(m.first))
                e->translate(sign * m.second.x(), sign * m.second.y());
        refresh(m_c);
    }
    Canvas *m_c; Document *m_d; QVector<QPair<QString, QPointF>> m_moves;
};

} // namespace

// ---- canvas --------------------------------------------------------------

Canvas::Canvas(QWidget *parent)
    : QGraphicsView(parent), m_scene(new QGraphicsScene(this)),
      m_undo(new QUndoStack(this))
{
    setScene(m_scene);
    setRenderHint(QPainter::Antialiasing, true);
    setDragMode(QGraphicsView::RubberBandDrag);
    setBackgroundBrush(kViewBg);
    setMouseTracking(true);
    // Flip Y so CC's Y-up space maps to Qt's Y-down scene.
    scale(1.0, -1.0);
    connect(m_scene, &QGraphicsScene::selectionChanged,
            this, &Canvas::applySelectionStyle);
}

void Canvas::setDocument(Document *doc)
{
    m_doc = doc;
    m_fitted = false;
    m_undo->clear();
    cancelDrawing();
    rebuild();
}

double Canvas::gridSpacing() const
{
    if (!m_doc)
        return 0;
    if (m_doc->params().value("grid_enabled", "1") == "0")
        return 0;
    const double s = m_doc->params().value("grid_spacing", "5").toDouble();
    return s > 0 ? s : 0;
}

QPointF Canvas::snap(QPointF p) const
{
    const double g = m_snap ? gridSpacing() : 0;
    if (g <= 0)
        return p;
    return QPointF(qRound(p.x() / g) * g, qRound(p.y() / g) * g);
}

void Canvas::setTool(Tool t)
{
    if (m_tool == DrawPath && t != DrawPath)
        cancelDrawing();
    m_tool = t;
    m_drawing = false;
    setDragMode(t == Select ? QGraphicsView::RubberBandDrag
                            : QGraphicsView::NoDrag);
    viewport()->setCursor(t == Select ? Qt::ArrowCursor : Qt::CrossCursor);
    const bool editable = (t == Select);
    for (QGraphicsItem *it : m_scene->items()) {
        if (it->data(0).isValid()) {
            it->setFlag(QGraphicsItem::ItemIsSelectable, editable);
            it->setFlag(QGraphicsItem::ItemIsMovable, editable);
        }
    }
    switch (t) {
    case Select:      emit statusHint(tr("Select — click/drag to select, drag to move, Del to delete")); break;
    case DrawCircle:  emit statusHint(tr("Circle — press at center, drag to radius")); break;
    case DrawRect:    emit statusHint(tr("Rectangle — drag corner to corner")); break;
    case DrawPolygon: emit statusHint(tr("Polygon — press at center, drag to radius")); break;
    case DrawPath:    emit statusHint(tr("Path — click to add points; Enter finishes, click near start closes, Esc cancels")); break;
    }
}

void Canvas::zoomFit()
{
    if (!m_doc)
        return;
    const double w = m_doc->boardWidth(), h = m_doc->boardHeight();
    if (w > 0 && h > 0)
        fitInView(QRectF(-10, -10, w + 20, h + 20), Qt::KeepAspectRatio);
}

void Canvas::rebuild()
{
    m_preview = nullptr;           // owned by the scene; clear() deletes it
    m_scene->clear();
    if (!m_doc)
        return;

    const double w = m_doc->boardWidth();
    const double h = m_doc->boardHeight();
    // Margin so the rubber band / fit has somewhere to breathe.
    m_scene->setSceneRect(QRectF(-w * 0.2 - 20, -h * 0.2 - 20, w * 1.4 + 40, h * 1.4 + 40));

    QPen elemPen(kElement);
    elemPen.setCosmetic(true);   // constant on-screen width regardless of zoom
    elemPen.setWidthF(1.3);

    const bool editable = (m_tool == Select);
    for (const Element &e : m_doc->elements()) {
        auto *item = new ElemItem(e.painterPath);
        item->setPen(elemPen);
        m_scene->addItem(item);
        item->setToolTip(QStringLiteral("%1  %2").arg(e.geometryType, e.id));
        item->setData(0, e.id);                       // scene item -> element
        item->setFlag(QGraphicsItem::ItemIsSelectable, editable);
        item->setFlag(QGraphicsItem::ItemIsMovable, editable);
    }

    if (!m_fitted && w > 0 && h > 0) {
        zoomFit();
        m_fitted = true;
    }
}

void Canvas::applySelectionStyle()
{
    QPen normal(kElement);
    normal.setCosmetic(true);
    normal.setWidthF(1.3);
    QPen sel(kSelected);
    sel.setCosmetic(true);
    sel.setWidthF(2.2);
    for (QGraphicsItem *it : m_scene->items()) {
        if (!it->data(0).isValid())
            continue;
        if (auto *pi = qgraphicsitem_cast<QGraphicsPathItem *>(it))
            pi->setPen(it->isSelected() ? sel : normal);
    }
}

void Canvas::drawBackground(QPainter *p, const QRectF &rect)
{
    QGraphicsView::drawBackground(p, rect);
    if (!m_doc)
        return;
    const double w = m_doc->boardWidth(), h = m_doc->boardHeight();
    if (w <= 0 || h <= 0)
        return;

    const QRectF board(0, 0, w, h);
    p->setPen(QPen(kBoardEdge, 0));
    p->setBrush(kBoardFill);
    p->drawRect(board);

    // Grid: minor lines at grid_spacing, majors every 5th. Skip minors when
    // they would sit closer than ~6 px on screen.
    const double g = gridSpacing();
    if (g > 0) {
        const double pxPerMm = transform().m11();
        const bool minors = g * pxPerMm >= 6.0;
        QPen minorPen(kGridMinor, 0), majorPen(kGridMajor, 0);
        for (int i = 1; i * g < w; ++i) {
            const bool major = (i % 5) == 0;
            if (!major && !minors) continue;
            p->setPen(major ? majorPen : minorPen);
            p->drawLine(QLineF(i * g, 0, i * g, h));
        }
        for (int i = 1; i * g < h; ++i) {
            const bool major = (i % 5) == 0;
            if (!major && !minors) continue;
            p->setPen(major ? majorPen : minorPen);
            p->drawLine(QLineF(0, i * g, w, i * g));
        }
    }

    // Origin axes along the board's bottom/left edges.
    p->setPen(QPen(kAxisX, 0));
    p->drawLine(QLineF(0, 0, qMin(w, 15.0), 0));
    p->setPen(QPen(kAxisY, 0));
    p->drawLine(QLineF(0, 0, 0, qMin(h, 15.0)));
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
    case DrawPath:
        if (!m_pathPts.isEmpty()) {
            p.moveTo(m_pathPts.first());
            for (int i = 1; i < m_pathPts.size(); ++i)
                p.lineTo(m_pathPts.at(i));
            p.lineTo(cur);
        }
        break;
    default:
        break;
    }
    return p;
}

void Canvas::cancelDrawing()
{
    m_drawing = false;
    m_pathPts.clear();
    if (m_preview) {
        m_scene->removeItem(m_preview);
        delete m_preview;
        m_preview = nullptr;
    }
}

void Canvas::finishPath(bool closed)
{
    if (m_doc && m_pathPts.size() >= 2) {
        const Element e = Element::makePath(m_pathPts, closed && m_pathPts.size() >= 3,
                                            m_doc->defaultLayer());
        m_undo->push(new AddCmd(this, m_doc, e));
        emit statusHint(closed ? tr("Closed path added") : tr("Path added"));
    }
    cancelDrawing();
}

void Canvas::mousePressEvent(QMouseEvent *event)
{
    if (m_tool == DrawPath && m_doc && event->button() == Qt::LeftButton) {
        const QPointF pos = snap(mapToScene(event->pos()));
        // Clicking near the start point closes the path.
        if (m_pathPts.size() >= 3) {
            const double tol = 8.0 / qMax(1e-9, qAbs(transform().m11()));
            if (QLineF(pos, m_pathPts.first()).length() <= tol) {
                finishPath(true);
                event->accept();
                return;
            }
        }
        m_pathPts.append(pos);
        if (!m_preview) {
            QPen pen(kPreview);
            pen.setCosmetic(true);
            pen.setStyle(Qt::DashLine);
            m_preview = m_scene->addPath(QPainterPath(), pen);
        }
        m_preview->setPath(previewPath(pos));
        emit statusHint(tr("Path: %1 point(s) — Enter finishes, click near start closes, Esc cancels")
                            .arg(m_pathPts.size()));
        event->accept();
        return;
    }

    if (m_tool != Select && m_tool != DrawPath && m_doc
        && event->button() == Qt::LeftButton) {
        m_drawing = true;
        m_anchor = snap(mapToScene(event->pos()));
        QPen pen(kPreview);
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
    const QPointF cc = mapToScene(event->pos());
    emit cursorMoved(cc);

    if ((m_drawing || (m_tool == DrawPath && !m_pathPts.isEmpty())) && m_preview) {
        const QPointF cur = snap(cc);
        m_preview->setPath(previewPath(cur));
        switch (m_tool) {
        case DrawCircle:
        case DrawPolygon:
            emit statusHint(tr("r = %1 mm").arg(QLineF(m_anchor, cur).length(), 0, 'f', 2));
            break;
        case DrawRect: {
            const QRectF r = QRectF(m_anchor, cur).normalized();
            emit statusHint(tr("%1 × %2 mm").arg(r.width(), 0, 'f', 2).arg(r.height(), 0, 'f', 2));
            break;
        }
        default:
            break;
        }
        event->accept();
        return;
    }
    QGraphicsView::mouseMoveEvent(event);
}

void Canvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_drawing && m_doc) {
        m_drawing = false;
        const QPointF cur = snap(mapToScene(event->pos()));
        if (m_preview) { m_scene->removeItem(m_preview); delete m_preview; m_preview = nullptr; }

        const QJsonObject layer = m_doc->defaultLayer();
        const double r = QLineF(m_anchor, cur).length();
        switch (m_tool) {
        case DrawCircle:
            if (r > 0.1)
                m_undo->push(new AddCmd(this, m_doc, Element::makeCircle(m_anchor, r, layer)));
            break;
        case DrawRect: {
            const QRectF rect = QRectF(m_anchor, cur).normalized();
            if (rect.width() > 0.1 && rect.height() > 0.1)
                m_undo->push(new AddCmd(this, m_doc,
                    Element::makeRectangle(rect.center(), rect.width(), rect.height(), layer)));
            break;
        }
        case DrawPolygon:
            if (r > 0.1)
                m_undo->push(new AddCmd(this, m_doc,
                    Element::makePolygon(m_anchor, r, m_polySides, layer)));
            break;
        default:
            break;
        }
        event->accept();
        return;
    }

    QGraphicsView::mouseReleaseEvent(event);

    // Select tool: commit any moved items back into the document. An item that
    // was dragged has a non-zero pos(); scene coords are CC mm, so the delta
    // applies directly.
    if (m_tool == Select && m_doc) {
        QVector<QPair<QString, QPointF>> moves;
        for (QGraphicsItem *it : m_scene->selectedItems()) {
            QPointF d = it->pos();
            if (!d.isNull() && it->data(0).isValid()) {
                if (m_snap) {
                    // Snap the delta so shapes land on the grid.
                    d = snap(d);
                    if (d.isNull())
                        continue;
                }
                moves.append({it->data(0).toString(), d});
            }
        }
        if (!moves.isEmpty())
            m_undo->push(new MoveCmd(this, m_doc, moves));
    }
}

void Canvas::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (m_tool == DrawPath && !m_pathPts.isEmpty()) {
        finishPath(false);
        event->accept();
        return;
    }
    QGraphicsView::mouseDoubleClickEvent(event);
}

void Canvas::keyPressEvent(QKeyEvent *event)
{
    if (m_tool == DrawPath && !m_pathPts.isEmpty()) {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            finishPath(false);
            return;
        }
        if (event->key() == Qt::Key_Escape) {
            cancelDrawing();
            emit statusHint(tr("Path cancelled"));
            return;
        }
    }
    if (event->key() == Qt::Key_Escape && (m_drawing || m_preview)) {
        cancelDrawing();
        return;
    }

    if (m_doc && (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)) {
        QVector<Element> victims;
        for (QGraphicsItem *it : m_scene->selectedItems()) {
            if (it->data(0).isValid())
                if (Element *e = m_doc->elementById(it->data(0).toString()))
                    victims.append(*e);
        }
        if (!victims.isEmpty()) {
            m_undo->push(new DeleteCmd(this, m_doc, victims));
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
