#include "canvas.h"
#include "backgroundimage.h"
#include <QApplication>
#include <QContextMenuEvent>
#include <QGraphicsScene>
#include <QMenu>
#include <QGraphicsPathItem>
#include <QGraphicsSceneHoverEvent>
#include <QInputDialog>
#include <QJsonArray>
#include <QLineEdit>
#include <QScrollBar>
#include <QSet>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QStyleOptionGraphicsItem>
#include <QTimer>
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

// Element item with CAD-style state rendering: closed shapes get a faint fill
// (which also makes their interior clickable), hover brightens the outline,
// selection turns it amber — no dashed Qt selection rectangle.
class ElemItem : public QGraphicsPathItem
{
public:
    ElemItem(const QPainterPath &path, bool closed)
        : QGraphicsPathItem(path), m_closed(closed)
    {
        setAcceptHoverEvents(true);
    }
    void paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) override
    {
        QPen pen;
        pen.setCosmetic(true);
        if (isSelected()) {
            pen.setColor(kSelected);
            pen.setWidthF(2.2);
        } else if (m_hover) {
            pen.setColor(QColor(0x9f, 0xc8, 0xf2));
            pen.setWidthF(1.8);
        } else {
            pen.setColor(kElement);
            pen.setWidthF(1.3);
        }
        p->setPen(pen);
        if (m_closed) {
            QColor fill = isSelected() ? QColor(kSelected) : QColor(Qt::white);
            fill.setAlpha(isSelected() ? 30 : 14);
            p->setBrush(fill);
        } else {
            p->setBrush(Qt::NoBrush);
        }
        p->drawPath(path());
    }
    QPainterPath shape() const override
    {
        // Closed shapes select from anywhere inside; open paths keep the
        // stroke-based hit area.
        return m_closed ? path() : QGraphicsPathItem::shape();
    }

protected:
    void hoverEnterEvent(QGraphicsSceneHoverEvent *) override { m_hover = true; update(); }
    void hoverLeaveEvent(QGraphicsSceneHoverEvent *) override { m_hover = false; update(); }

private:
    bool m_closed;
    bool m_hover = false;
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

class EditCmd : public QUndoCommand
{
public:
    EditCmd(Canvas *c, Document *d, const Element &before, const Element &after)
        : m_c(c), m_d(d), m_before(before), m_after(after)
    { setText(QStringLiteral("edit %1").arg(before.geometryType)); }
    void redo() override { m_d->replaceElement(m_after); refresh(m_c); }
    void undo() override { m_d->replaceElement(m_before); refresh(m_c); }
private:
    Canvas *m_c; Document *m_d; Element m_before, m_after;
};

// Swap a set of elements for another set (convert-to-path: a text becomes
// several glyph paths). Afters are appended, so the z-order may change.
class ReplaceCmd : public QUndoCommand
{
public:
    ReplaceCmd(Canvas *c, Document *d, const QVector<Element> &before,
               const QVector<Element> &after, const QString &text)
        : m_c(c), m_d(d), m_before(before), m_after(after) { setText(text); }
    void redo() override { swap(m_before, m_after); }
    void undo() override { swap(m_after, m_before); }
private:
    void swap(const QVector<Element> &out, const QVector<Element> &in)
    {
        for (const Element &e : out) m_d->removeElementById(e.id);
        for (const Element &e : in) m_d->addElement(e);
        refresh(m_c);
    }
    Canvas *m_c; Document *m_d; QVector<Element> m_before, m_after;
};

class TpEditCmd : public QUndoCommand
{
public:
    TpEditCmd(Canvas *c, Document *d, const Toolpath &before, const Toolpath &after)
        : m_c(c), m_d(d), m_before(before), m_after(after)
    { setText(QStringLiteral("edit toolpath %1").arg(before.json.value("name").toString())); }
    void redo() override { m_d->replaceToolpath(m_after); refresh(m_c); }
    void undo() override { m_d->replaceToolpath(m_before); refresh(m_c); }
private:
    Canvas *m_c; Document *m_d; Toolpath m_before, m_after;
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
            this, &Canvas::onSelectionChanged);
}

Canvas::~Canvas()
{
    // The scene is a child QObject and dies after this destructor has run;
    // its selectionChanged (emitted while its items are deleted) must not
    // reach the half-destroyed Canvas.
    disconnect(m_scene, nullptr, this, nullptr);
    m_scene->blockSignals(true);
}

void Canvas::onSelectionChanged()
{
    QStringList ids;
    for (QGraphicsItem *it : m_scene->selectedItems())
        if (it->data(0).isValid())
            ids << it->data(0).toString();
    if (m_tool == NodeEdit)
        syncEditTarget();
    emit selectionChangedIds(ids);
    viewport()->update();
}

double Canvas::pxToMm(double px) const
{
    return px / qMax(1e-9, qAbs(transform().m11()));
}

QGraphicsPathItem *Canvas::itemFor(const QString &id) const
{
    for (QGraphicsItem *it : m_scene->items())
        if (it->data(0).isValid() && it->data(0).toString() == id)
            return qgraphicsitem_cast<QGraphicsPathItem *>(it);
    return nullptr;
}

void Canvas::selectElements(const QStringList &ids)
{
    m_scene->blockSignals(true);
    m_scene->clearSelection();
    for (const QString &id : ids)
        if (QGraphicsItem *it = itemFor(id))
            it->setSelected(true);
    m_scene->blockSignals(false);
    onSelectionChanged();
}

void Canvas::editText(const QString &id, const QJsonObject &changes)
{
    if (!m_doc || changes.isEmpty())
        return;
    Element *e = m_doc->elementById(id);
    if (!e || e->geometryType != QLatin1String("text"))
        return;
    const Element after = Element::regenText(*e, changes);
    if (after.raw == e->raw)
        return;
    m_undo->push(new EditCmd(this, m_doc, *e, after));
}

void Canvas::convertToPaths(const QStringList &ids)
{
    if (!m_doc)
        return;
    QVector<Element> before, after;
    for (const QString &id : ids) {
        Element *e = m_doc->elementById(id);
        if (!e || e->geometryType == QLatin1String("path"))
            continue;
        before.append(*e);
        after += Element::toPaths(*e);
    }
    if (before.isEmpty())
        return;
    if (after.isEmpty()) {
        // Element::toPaths drops degenerate contours, so a text of spaces (or
        // a font that renders nothing) yields none: replacing here would
        // delete the element and put nothing in its place.
        emit statusHint(tr("nothing to convert: the selection has no outline"));
        return;
    }
    if (before.size() == 1 && after.size() == 1) {
        m_undo->push(new EditCmd(this, m_doc, before.first(), after.first()));
    } else {
        m_undo->push(new ReplaceCmd(this, m_doc, before, after,
                                    tr("convert %1 element(s) to paths").arg(before.size())));
    }
    QStringList newIds;
    for (const Element &e : after)
        newIds << e.id;
    selectElements(newIds);
    emit statusHint(tr("%1 path(s) — press N to edit nodes").arg(after.size()));
}

void Canvas::emitZoom()
{
    emit zoomChanged(qAbs(transform().m11()) * 100.0);
}

void Canvas::editElement(const QString &id, const QHash<QString, double> &params)
{
    if (!m_doc)
        return;
    Element *e = m_doc->elementById(id);
    if (!e)
        return;
    const Element after = Element::regen(*e, params);
    if (after.raw == e->raw)
        return;
    m_undo->push(new EditCmd(this, m_doc, *e, after));
}

void Canvas::moveElementBy(const QString &id, double dx, double dy)
{
    if (!m_doc || (qFuzzyIsNull(dx) && qFuzzyIsNull(dy)))
        return;
    if (m_doc->elementById(id))
        m_undo->push(new MoveCmd(this, m_doc, {{id, QPointF(dx, dy)}}));
}

QStringList Canvas::selectedElementIds() const
{
    QStringList ids;
    for (QGraphicsItem *it : m_scene->selectedItems())
        if (it->data(0).isValid())
            ids << it->data(0).toString();
    return ids;
}

void Canvas::selectIds(const QStringList &ids)
{
    const QSet<QString> want(ids.begin(), ids.end());
    for (QGraphicsItem *it : m_scene->items())
        if (it->data(0).isValid())
            it->setSelected(want.contains(it->data(0).toString()));
}

void Canvas::editToolpath(const QString &uuid, const QJsonObject &newJson)
{
    if (!m_doc)
        return;
    Toolpath *t = m_doc->toolpathByUuid(uuid);
    if (!t || t->json == newJson)
        return;
    Toolpath after = *t;
    after.json = newJson;
    m_undo->push(new TpEditCmd(this, m_doc, *t, after));
}

void Canvas::insertGenerated(const QVector<Element> &els, const Toolpath &tp)
{
    if (!m_doc)
        return;
    for (const Element &e : els)
        m_doc->addElement(e);
    m_doc->addToolpath(tp);
    rebuild();
    emit documentChanged();
}

// Exactly one selected, parametrically resizable element? Handle sits at the
// east point (circle/polygon) or the top-right corner (rectangle).
bool Canvas::resizeHandle(QString *id, QPointF *pos, QString *type) const
{
    if (!m_doc || m_tool != Select)
        return false;
    const auto sel = m_scene->selectedItems();
    if (sel.size() != 1 || !sel.first()->data(0).isValid())
        return false;
    Element *e = const_cast<Document *>(m_doc)->elementById(sel.first()->data(0).toString());
    if (!e)
        return false;
    const QJsonArray c = e->raw.value("center").toArray();
    if (c.size() != 2)
        return false;
    const QPointF center(c.at(0).toDouble(), c.at(1).toDouble());
    if (e->geometryType == QLatin1String("circle")
        || e->geometryType == QLatin1String("regular_polygon")) {
        *pos = center + QPointF(e->raw.value("radius").toDouble(), 0);
    } else if (e->geometryType == QLatin1String("rectangle")) {
        *pos = center + QPointF(e->raw.value("width").toDouble() / 2,
                                e->raw.value("height").toDouble() / 2);
    } else {
        return false;
    }
    *id = e->id;
    *type = e->geometryType;
    return true;
}

void Canvas::drawForeground(QPainter *p, const QRectF &rect)
{
    QGraphicsView::drawForeground(p, rect);
    if (m_tool == NodeEdit && !m_model.isEmpty()) {
        drawNodes(p);
        return;
    }
    if (m_tool == DrawPath && !m_penNodes.isEmpty()) {
        // Pen tool: anchors so far, plus the handles of the node being dragged.
        const double s = pxToMm(3.5);
        p->setPen(QPen(QColor(0x1a, 0x1a, 0x1a), 0));
        p->setBrush(kPreview);
        for (const PathNode &n : m_penNodes)
            p->drawRect(QRectF(n.p.x() - s, n.p.y() - s, 2 * s, 2 * s));
        const PathNode &last = m_penNodes.last();
        if (last.hasOut() || last.hasIn()) {
            QPen hp(QColor(0x9f, 0xc8, 0xf2));
            hp.setCosmetic(true);
            p->setPen(hp);
            p->setBrush(Qt::NoBrush);
            p->drawLine(QLineF(last.in, last.out));
            p->setBrush(QColor(0x9f, 0xc8, 0xf2));
            p->drawEllipse(last.in, s, s);
            p->drawEllipse(last.out, s, s);
        }
        // Closing hint: ring around the first anchor when it can be closed.
        if (m_penNodes.size() >= 2) {
            p->setPen(QPen(kPreview, 0));
            p->setBrush(Qt::NoBrush);
            p->drawEllipse(m_penNodes.first().p, pxToMm(8), pxToMm(8));
        }
        return;
    }
    QString id, type;
    QPointF pos;
    if (!m_resizing && !resizeHandle(&id, &pos, &type))
        return;
    if (m_resizing)
        return;   // handle hidden while dragging; the preview shows the shape
    const double s = pxToMm(4.0);
    p->setPen(QPen(QColor(0x1a, 0x1a, 0x1a), 0));
    p->setBrush(kSelected);
    p->drawRect(QRectF(pos.x() - s, pos.y() - s, 2 * s, 2 * s));
}

// Node editor overlay: tangent lines, handle circles, anchor squares. The
// selected node is amber; smooth/symmetric anchors are drawn as diamonds so
// the kind is visible without a menu.
void Canvas::drawNodes(QPainter *p) const
{
    const double s = pxToMm(3.5);
    const QColor handleCol(0x9f, 0xc8, 0xf2);
    for (int si = 0; si < m_model.subs.size(); ++si) {
        const SubPath &sub = m_model.subs.at(si);
        QPen hp(handleCol);
        hp.setCosmetic(true);
        p->setPen(hp);
        p->setBrush(Qt::NoBrush);
        for (const PathNode &n : sub.nodes) {
            if (n.hasIn())  p->drawLine(QLineF(n.p, n.in));
            if (n.hasOut()) p->drawLine(QLineF(n.p, n.out));
        }
        p->setPen(QPen(QColor(0x1a, 0x1a, 0x1a), 0));
        p->setBrush(handleCol);
        for (const PathNode &n : sub.nodes) {
            if (n.hasIn())  p->drawEllipse(n.in, s, s);
            if (n.hasOut()) p->drawEllipse(n.out, s, s);
        }
        for (int ni = 0; ni < sub.nodes.size(); ++ni) {
            const PathNode &n = sub.nodes.at(ni);
            const bool sel = (si == m_selSub && ni == m_selNode);
            p->setBrush(sel ? kSelected : QColor(0xf4, 0xf6, 0xfa));
            if (n.kind == PathNode::Corner) {
                p->drawRect(QRectF(n.p.x() - s, n.p.y() - s, 2 * s, 2 * s));
            } else {
                const double d = s * 1.35;
                QPolygonF dia;
                dia << QPointF(n.p.x(), n.p.y() + d) << QPointF(n.p.x() + d, n.p.y())
                    << QPointF(n.p.x(), n.p.y() - d) << QPointF(n.p.x() - d, n.p.y());
                p->drawPolygon(dia);
            }
        }
    }
}

void Canvas::syncEditTarget()
{
    const QStringList ids = selectedElementIds();
    m_editId.clear();
    m_model = PathModel();
    if (m_doc && ids.size() == 1) {
        if (Element *e = m_doc->elementById(ids.first())) {
            m_editId = e->id;
            m_model = Element::pathModel(*e);
            if (m_model.isEmpty())
                emit statusHint(tr("Text has no nodes — right-click → Convert to path first"));
            else if (e->geometryType != QLatin1String("path"))
                emit statusHint(tr("%1 — editing a node converts it to a path").arg(e->geometryType));
        }
    }
    m_selSub = m_selNode = -1;
    m_grab = GrabNone;
    viewport()->update();
}

bool Canvas::hitNode(const QPointF &pos, int *sub, int *node, NodeGrab *what) const
{
    const double tol = pxToMm(7.0);
    // Handles first (they can sit on top of anchors), selected node's first.
    for (int pass = 0; pass < 2; ++pass) {
        for (int si = 0; si < m_model.subs.size(); ++si) {
            const SubPath &sp = m_model.subs.at(si);
            for (int ni = 0; ni < sp.nodes.size(); ++ni) {
                const bool sel = (si == m_selSub && ni == m_selNode);
                if ((pass == 0) != sel)
                    continue;
                const PathNode &n = sp.nodes.at(ni);
                if (n.hasOut() && QLineF(pos, n.out).length() <= tol) {
                    *sub = si; *node = ni; *what = GrabOut; return true;
                }
                if (n.hasIn() && QLineF(pos, n.in).length() <= tol) {
                    *sub = si; *node = ni; *what = GrabIn; return true;
                }
            }
        }
    }
    for (int si = 0; si < m_model.subs.size(); ++si) {
        const SubPath &sp = m_model.subs.at(si);
        for (int ni = 0; ni < sp.nodes.size(); ++ni)
            if (QLineF(pos, sp.nodes.at(ni).p).length() <= tol) {
                *sub = si; *node = ni; *what = GrabAnchor; return true;
            }
    }
    return false;
}

bool Canvas::hitSegment(const QPointF &pos, int *sub, int *seg, double *t) const
{
    const double tol = pxToMm(7.0);
    double best = tol;
    bool hit = false;
    for (int si = 0; si < m_model.subs.size(); ++si) {
        int sg; double tt;
        const double d = m_model.subs.at(si).closest(pos, &sg, &tt);
        if (sg >= 0 && d < best) { best = d; *sub = si; *seg = sg; *t = tt; hit = true; }
    }
    return hit;
}

void Canvas::previewModel()
{
    if (QGraphicsPathItem *it = itemFor(m_editId))
        it->setPath(m_model.painterPath());
    viewport()->update();
}

void Canvas::commitModel(const QString &what)
{
    if (!m_doc || m_editId.isEmpty())
        return;
    Element *e = m_doc->elementById(m_editId);
    if (!e)
        return;
    const Element after = Element::withPathModel(*e, m_model);
    if (after.raw == e->raw) {
        previewModel();
        return;
    }
    const int keepSub = m_selSub, keepNode = m_selNode;
    auto *cmd = new EditCmd(this, m_doc, *e, after);
    cmd->setText(what);
    m_undo->push(cmd);               // rebuild() re-decodes the model
    m_selSub = keepSub;
    m_selNode = keepNode;
    if (m_selSub >= m_model.subs.size()
        || (m_selSub >= 0 && m_selNode >= m_model.subs.at(m_selSub).nodes.size()))
        m_selSub = m_selNode = -1;
    viewport()->update();
}

void Canvas::setBackgroundImage(const BackgroundImage *bg)
{
    m_bg = bg;
    viewport()->update();
}

void Canvas::setDocument(Document *doc)
{
    m_doc = doc;
    m_fitted = false;
    m_undo->clear();
    cancelDrawing();
    rebuild();

    // Headless screenshot hooks (`--shot`): PHOBICCCP_SHOT_SELECT=<geometryType>
    // selects the last element of that type, PHOBICCCP_SHOT_TOOL=nodes opens
    // it in the node editor.
    const QByteArray selType = qgetenv("PHOBICCCP_SHOT_SELECT");
    if (m_doc && !selType.isEmpty()) {
        QString id;
        for (const Element &e : m_doc->elements())
            if (e.geometryType == QString::fromLatin1(selType))
                id = e.id;
        const bool nodes = qgetenv("PHOBICCCP_SHOT_TOOL") == "nodes";
        // Deferred: the main window wires its panels after setDocument().
        QTimer::singleShot(300, this, [this, id, nodes] {
            if (nodes)
                setTool(NodeEdit);
            if (!id.isEmpty())
                selectElements({id});
        });
    }
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
    m_grab = GrabNone;
    setDragMode(t == Select ? QGraphicsView::RubberBandDrag
                            : QGraphicsView::NoDrag);
    viewport()->setCursor(t == Select || t == NodeEdit ? Qt::ArrowCursor : Qt::CrossCursor);
    const bool selectable = (t == Select || t == NodeEdit);
    for (QGraphicsItem *it : m_scene->items()) {
        if (it->data(0).isValid()) {
            it->setFlag(QGraphicsItem::ItemIsSelectable, selectable);
            it->setFlag(QGraphicsItem::ItemIsMovable, t == Select);
        }
    }
    switch (t) {
    case Select:      emit statusHint(tr("Select — click/drag to select, drag to move, Del to delete, right-click for Convert to path")); break;
    case DrawCircle:  emit statusHint(tr("Circle — press at center, drag to radius")); break;
    case DrawRect:    emit statusHint(tr("Rectangle — drag corner to corner")); break;
    case DrawPolygon: emit statusHint(tr("Polygon — press at center, drag to radius")); break;
    case DrawPath:    emit statusHint(tr("Path — click = corner, click-drag = curve; Enter finishes, click near start closes, Esc cancels")); break;
    case DrawText:    emit statusHint(tr("Text — click to place the baseline start")); break;
    case NodeEdit:    emit statusHint(tr("Nodes — drag anchors/handles (Alt breaks symmetry), double-click a segment to add, Del removes, right-click a node for its kind")); break;
    }
    if (t == NodeEdit)
        syncEditTarget();
    else {
        m_editId.clear();
        m_model = PathModel();
    }
    viewport()->update();
    emit toolChanged(t);
}

void Canvas::zoomFit()
{
    if (!m_doc)
        return;
    const double w = m_doc->boardWidth(), h = m_doc->boardHeight();
    if (w > 0 && h > 0)
        fitInView(QRectF(-10, -10, w + 20, h + 20), Qt::KeepAspectRatio);
    emitZoom();
}

void Canvas::rebuild()
{
    // The scene is rebuilt from scratch; keep the selection (and with it the
    // node-edit target and the properties panel) across the edit.
    const QStringList selected = selectedElementIds();
    m_scene->blockSignals(true);
    m_preview = nullptr;           // owned by the scene; clear() deletes it
    m_scene->clear();
    if (!m_doc) {
        m_scene->blockSignals(false);
        onSelectionChanged();
        return;
    }

    const double w = m_doc->boardWidth();
    const double h = m_doc->boardHeight();
    // Margin so the rubber band / fit has somewhere to breathe.
    m_scene->setSceneRect(QRectF(-w * 0.2 - 20, -h * 0.2 - 20, w * 1.4 + 40, h * 1.4 + 40));

    const bool selectable = (m_tool == Select || m_tool == NodeEdit);
    for (const Element &e : m_doc->elements()) {
        // Open paths are the only unfilled type; a path row of point_type 4
        // marks a closed one.
        bool closed = e.geometryType != QLatin1String("path");
        if (!closed)
            for (const auto &v : e.raw.value("point_type").toArray())
                if (v.toInt() == 4) { closed = true; break; }
        auto *item = new ElemItem(e.painterPath, closed);
        m_scene->addItem(item);
        item->setToolTip(QStringLiteral("%1  %2").arg(e.geometryType, e.id));
        item->setData(0, e.id);                       // scene item -> element
        item->setFlag(QGraphicsItem::ItemIsSelectable, selectable);
        item->setFlag(QGraphicsItem::ItemIsMovable, m_tool == Select);
        if (selected.contains(e.id))
            item->setSelected(true);
    }

    if (!m_previewOps.isEmpty())
        renderPreviewOps();
    m_scene->blockSignals(false);
    onSelectionChanged();

    // First-load fit. Only trust the viewport once it has been laid out;
    // otherwise fitInView() scales to a placeholder size (0% zoom). The
    // resizeEvent below picks it up when the real geometry arrives.
    if (!m_fitted && w > 0 && h > 0 && isVisible() && viewport()->width() > 100) {
        zoomFit();
        m_fitted = true;
    }
}

void Canvas::resizeEvent(QResizeEvent *event)
{
    QGraphicsView::resizeEvent(event);
    if (!m_fitted && m_doc && m_doc->boardWidth() > 0 && m_doc->boardHeight() > 0
        && viewport()->width() > 100) {
        zoomFit();
        m_fitted = true;
    }
}

void Canvas::setToolpathPreview(const QVector<Op> &ops)
{
    m_previewOps = ops;
    rebuild();
}

void Canvas::clearToolpathPreview()
{
    m_previewOps.clear();
    rebuild();
}

// Draw the generated machine motion over the vectors: rapids as dashed gray,
// cutting moves colored shallow-yellow → deep-red by Z, arcs sampled to
// segments. Overlay items are unselectable and sit above the elements.
void Canvas::renderPreviewOps()
{
    double zMin = 0;
    for (const Op &op : m_previewOps)
        if ((op.kind == Op::Feed || op.kind == Op::Arc) && op.z < zMin)
            zMin = op.z;
    const int NB = 8;
    auto bucketOf = [&](double z) {
        if (zMin >= -1e-9)
            return 0;
        return qBound(0, int((z / zMin) * NB), NB - 1);
    };
    QVector<QPainterPath> cut(NB);
    QPainterPath rapids;

    double x = 0, y = 0;
    bool have = false;
    for (const Op &op : m_previewOps) {
        switch (op.kind) {
        case Op::Rapid:
            if (have && (op.x != x || op.y != y)) {
                rapids.moveTo(x, y);
                rapids.lineTo(op.x, op.y);
            }
            x = op.x; y = op.y; have = true;
            break;
        case Op::Feed: {
            const int b = bucketOf(op.z);
            if (have) {
                if (op.x == x && op.y == y) {
                    // pure plunge: mark with a tiny diamond
                    cut[b].moveTo(x - 0.4, y);
                    cut[b].lineTo(x, y + 0.4);
                    cut[b].lineTo(x + 0.4, y);
                    cut[b].lineTo(x, y - 0.4);
                    cut[b].closeSubpath();
                } else {
                    cut[b].moveTo(x, y);
                    cut[b].lineTo(op.x, op.y);
                }
            }
            x = op.x; y = op.y; have = true;
            break;
        }
        case Op::Arc: {
            const int b = bucketOf(op.z);
            const double cx = x + op.ci, cy = y + op.cj;
            const double r = QLineF(QPointF(cx, cy), QPointF(x, y)).length();
            double a0 = qAtan2(y - cy, x - cx);
            double a1 = qAtan2(op.y - cy, op.x - cx);
            // G2 = clockwise in Y-up space = decreasing angle.
            if (op.cw) { while (a1 >= a0 - 1e-12) a1 -= 2 * M_PI; }
            else       { while (a1 <= a0 + 1e-12) a1 += 2 * M_PI; }
            const int steps = qMax(8, int(qAbs(a1 - a0) / (M_PI / 36)));
            cut[b].moveTo(x, y);
            for (int i = 1; i <= steps; ++i) {
                const double a = a0 + (a1 - a0) * i / steps;
                cut[b].lineTo(cx + r * qCos(a), cy + r * qSin(a));
            }
            x = op.x; y = op.y; have = true;
            break;
        }
        default:
            break;
        }
    }

    QPen rapidPen(QColor(0x8a, 0x94, 0xa6, 140));
    rapidPen.setCosmetic(true);
    rapidPen.setStyle(Qt::DashLine);
    auto *ri = m_scene->addPath(rapids, rapidPen);
    ri->setZValue(5);

    for (int b = 0; b < NB; ++b) {
        if (cut.at(b).isEmpty())
            continue;
        const double f = NB > 1 ? double(b) / (NB - 1) : 0;
        QColor col = QColor::fromHsvF(0.14 * (1.0 - f), 0.9, 1.0);   // yellow→red
        col.setAlpha(200);
        QPen pen(col);
        pen.setCosmetic(true);
        pen.setWidthF(1.4);
        auto *it = m_scene->addPath(cut.at(b), pen);
        it->setZValue(6);
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
    if (m_bg)
        m_bg->paint(p);   // under the grid and the vectors

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
        if (!m_penNodes.isEmpty()) {
            PathModel m;
            SubPath sp;
            sp.nodes = m_penNodes;
            if (!m_penDrag) {
                // Rubber segment to the cursor: a corner would land there.
                PathNode c;
                c.p = c.in = c.out = cur;
                sp.nodes.append(c);
            }
            m.subs.append(sp);
            p = m.painterPath();
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
    m_penNodes.clear();
    m_penDrag = false;
    if (m_preview) {
        m_scene->removeItem(m_preview);
        delete m_preview;
        m_preview = nullptr;
    }
}

void Canvas::finishPath(bool closed)
{
    // A double-click's second press leaves a duplicate of the previous node.
    while (m_penNodes.size() >= 2
           && QLineF(m_penNodes.last().p, m_penNodes.at(m_penNodes.size() - 2).p).length() < 1e-9)
        m_penNodes.removeLast();
    if (m_doc && m_penNodes.size() >= 2) {
        PathModel m;
        SubPath sp;
        sp.nodes = m_penNodes;
        sp.closed = closed && m_penNodes.size() >= 2;
        if (!sp.closed) {   // no segment arrives at the first / leaves the last node
            sp.nodes.first().in = sp.nodes.first().p;
            sp.nodes.last().out = sp.nodes.last().p;
        }
        m.subs.append(sp);
        const Element e = Element::makeBezierPath(m, m_doc->defaultLayer());
        m_undo->push(new AddCmd(this, m_doc, e));
        emit statusHint(sp.closed ? tr("Closed path added") : tr("Path added"));
    }
    cancelDrawing();
    viewport()->update();
}

void Canvas::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::MiddleButton) {
        m_panning = true;
        m_panLast = event->pos();
        viewport()->setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }

    // Resize-handle grab (Select mode, single selection).
    if (m_tool == Select && event->button() == Qt::LeftButton) {
        QString id, type;
        QPointF hpos;
        if (resizeHandle(&id, &hpos, &type)) {
            const double tol = 8.0 / qMax(1e-9, qAbs(transform().m11()));
            if (QLineF(mapToScene(event->pos()), hpos).length() <= tol) {
                Element *e = m_doc->elementById(id);
                const QJsonArray c = e->raw.value("center").toArray();
                m_resizing = true;
                m_resizeId = id;
                m_resizeType = type;
                m_resizeCenter = QPointF(c.at(0).toDouble(), c.at(1).toDouble());
                QPen pen(kSelected);
                pen.setCosmetic(true);
                pen.setStyle(Qt::DashLine);
                m_preview = m_scene->addPath(QPainterPath(), pen);
                viewport()->update();
                event->accept();
                return;
            }
        }
    }

    if (m_tool == DrawText && m_doc && event->button() == Qt::LeftButton) {
        const QPointF pos = snap(mapToScene(event->pos()));
        bool ok = false;
        const QString text = QInputDialog::getText(
            this, tr("Add text"), tr("Text:"), QLineEdit::Normal, {}, &ok);
        if (ok && !text.trimmed().isEmpty()) {
            const double h = QInputDialog::getDouble(
                this, tr("Add text"), tr("Height (mm):"), 10.0, 0.5, 500.0, 1, &ok);
            if (ok)
                m_undo->push(new AddCmd(this, m_doc,
                    Element::makeText(text, pos, h, QStringLiteral("Helvetica"),
                                      m_doc->defaultLayer())));
        }
        event->accept();
        return;
    }

    if (m_tool == DrawPath && m_doc && event->button() == Qt::LeftButton) {
        const QPointF pos = snap(mapToScene(event->pos()));
        // Clicking near the start point closes the path.
        if (m_penNodes.size() >= 2) {
            if (QLineF(mapToScene(event->pos()), m_penNodes.first().p).length() <= pxToMm(8)) {
                finishPath(true);
                event->accept();
                return;
            }
        }
        PathNode n;
        n.p = n.in = n.out = pos;
        m_penNodes.append(n);
        m_penDrag = true;                 // drag pulls out symmetric handles
        if (!m_preview) {
            QPen pen(kPreview);
            pen.setCosmetic(true);
            pen.setStyle(Qt::DashLine);
            m_preview = m_scene->addPath(QPainterPath(), pen);
        }
        m_preview->setPath(previewPath(pos));
        emit statusHint(tr("Path: %1 node(s) — drag for a curve; Enter finishes, click near start closes, Esc cancels")
                            .arg(m_penNodes.size()));
        viewport()->update();
        event->accept();
        return;
    }

    if (m_tool == NodeEdit && m_doc && event->button() == Qt::LeftButton && !m_model.isEmpty()) {
        const QPointF pos = mapToScene(event->pos());
        int sub, node;
        NodeGrab what;
        if (hitNode(pos, &sub, &node, &what)) {
            m_selSub = sub;
            m_selNode = node;
            m_grab = what;
            m_grabBreak = event->modifiers() & (Qt::AltModifier | Qt::ShiftModifier);
            viewport()->update();
            event->accept();
            return;
        }
        m_selSub = m_selNode = -1;   // click elsewhere: (re)select elements
        viewport()->update();
    }

    // NodeEdit selects and edits; it never starts a new shape, so it must not
    // reach the drawing branch (which would swallow the click before the
    // scene sees it, leaving nothing selectable with the tool).
    if (m_tool != Select && m_tool != DrawPath && m_tool != NodeEdit && m_doc
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
    if (m_panning) {
        const QPoint d = event->pos() - m_panLast;
        m_panLast = event->pos();
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - d.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - d.y());
        event->accept();
        return;
    }

    const QPointF cc = mapToScene(event->pos());
    emit cursorMoved(cc);

    if (m_resizing && m_preview && m_doc) {
        if (Element *e = m_doc->elementById(m_resizeId)) {
            const QPointF cur = snap(cc);
            QHash<QString, double> p;
            if (m_resizeType == QLatin1String("rectangle")) {
                p.insert("width",  qMax(0.2, 2 * qAbs(cur.x() - m_resizeCenter.x())));
                p.insert("height", qMax(0.2, 2 * qAbs(cur.y() - m_resizeCenter.y())));
                emit statusHint(tr("%1 × %2 mm").arg(p["width"], 0, 'f', 2).arg(p["height"], 0, 'f', 2));
            } else {
                p.insert("radius", qMax(0.1, QLineF(m_resizeCenter, cur).length()));
                emit statusHint(tr("r = %1 mm").arg(p["radius"], 0, 'f', 2));
            }
            m_preview->setPath(Element::regen(*e, p).painterPath);
        }
        event->accept();
        return;
    }

    if (m_tool == NodeEdit && m_grab != GrabNone && !m_model.isEmpty()) {
        SubPath &sp = m_model.subs[m_selSub];
        PathNode &n = sp.nodes[m_selNode];
        const bool breakSym = m_grabBreak
                              || (event->modifiers() & (Qt::AltModifier | Qt::ShiftModifier));
        if (m_grab == GrabAnchor) {
            const QPointF to = snap(cc);
            const QPointF d = to - n.p;
            n.p = to; n.in += d; n.out += d;
        } else {
            sp.moveHandle(m_selNode, m_grab == GrabOut, cc, breakSym);
        }
        previewModel();
        emit statusHint(tr("node %1: %2, %3 mm").arg(m_selNode)
                            .arg(n.p.x(), 0, 'f', 2).arg(n.p.y(), 0, 'f', 2));
        event->accept();
        return;
    }

    if (m_tool == DrawPath && m_penDrag && !m_penNodes.isEmpty() && m_preview) {
        // Pull the new node's handles out symmetrically; a tiny drag stays a corner.
        PathNode &n = m_penNodes.last();
        if (QLineF(cc, n.p).length() > pxToMm(3)) {
            n.out = cc;
            n.in = n.p - (cc - n.p);
            n.kind = PathNode::Symmetric;
        } else {
            n.in = n.out = n.p;
            n.kind = PathNode::Corner;
        }
        m_preview->setPath(previewPath(cc));
        viewport()->update();
        event->accept();
        return;
    }

    if ((m_drawing || (m_tool == DrawPath && !m_penNodes.isEmpty())) && m_preview) {
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
    if (m_panning && event->button() == Qt::MiddleButton) {
        m_panning = false;
        viewport()->setCursor(m_tool == Select ? Qt::ArrowCursor : Qt::CrossCursor);
        event->accept();
        return;
    }

    if (m_tool == NodeEdit && m_grab != GrabNone) {
        m_grab = GrabNone;
        commitModel(tr("move node"));
        event->accept();
        return;
    }

    if (m_tool == DrawPath && m_penDrag) {
        m_penDrag = false;
        if (m_preview)
            m_preview->setPath(previewPath(mapToScene(event->pos())));
        viewport()->update();
        event->accept();
        return;
    }

    if (m_resizing && m_doc) {
        m_resizing = false;
        if (m_preview) { m_scene->removeItem(m_preview); delete m_preview; m_preview = nullptr; }
        const QPointF cur = snap(mapToScene(event->pos()));
        QHash<QString, double> p;
        if (m_resizeType == QLatin1String("rectangle")) {
            p.insert("width",  qMax(0.2, 2 * qAbs(cur.x() - m_resizeCenter.x())));
            p.insert("height", qMax(0.2, 2 * qAbs(cur.y() - m_resizeCenter.y())));
        } else {
            p.insert("radius", qMax(0.1, QLineF(m_resizeCenter, cur).length()));
        }
        editElement(m_resizeId, p);
        event->accept();
        return;
    }

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
    if (m_tool == DrawPath && !m_penNodes.isEmpty()) {
        finishPath(false);
        event->accept();
        return;
    }
    if (m_tool == NodeEdit && m_doc && !m_model.isEmpty() && event->button() == Qt::LeftButton) {
        const QPointF pos = mapToScene(event->pos());
        int sub, node, seg;
        double t;
        NodeGrab what;
        if (!hitNode(pos, &sub, &node, &what) && hitSegment(pos, &sub, &seg, &t)) {
            m_grab = GrabNone;
            const int idx = m_model.subs[sub].insertNode(seg, t);
            m_selSub = sub;
            m_selNode = idx;
            commitModel(tr("insert node"));
            event->accept();
            return;
        }
    }
    QGraphicsView::mouseDoubleClickEvent(event);
}

void Canvas::contextMenuEvent(QContextMenuEvent *event)
{
    if (!m_doc)
        return;
    const QPointF pos = mapToScene(event->pos());
    QMenu menu(this);

    if (m_tool == NodeEdit && !m_model.isEmpty()) {
        int sub, node;
        NodeGrab what;
        if (hitNode(pos, &sub, &node, &what)) {
            m_selSub = sub;
            m_selNode = node;
            viewport()->update();
            const PathNode::Kind cur = m_model.subs.at(sub).nodes.at(node).kind;
            auto kindAct = [&](const QString &name, PathNode::Kind k) {
                QAction *a = menu.addAction(name);
                a->setCheckable(true);
                a->setChecked(cur == k);
                connect(a, &QAction::triggered, this, [this, sub, node, k] {
                    m_model.subs[sub].setKind(node, k);
                    commitModel(tr("node kind"));
                });
            };
            kindAct(tr("Corner"), PathNode::Corner);
            kindAct(tr("Smooth"), PathNode::Smooth);
            kindAct(tr("Symmetric"), PathNode::Symmetric);
            menu.addSeparator();
            connect(menu.addAction(tr("Straight lines (retract handles)")), &QAction::triggered,
                    this, [this, sub, node] {
                        m_model.subs[sub].retract(node);
                        commitModel(tr("retract handles"));
                    });
            connect(menu.addAction(tr("Delete node")), &QAction::triggered, this, [this, sub, node] {
                if (m_model.subs[sub].removeNode(node)) {
                    m_selSub = m_selNode = -1;
                    commitModel(tr("delete node"));
                }
            });
            menu.exec(event->globalPos());
            return;
        }
    }

    // Element menu (Select or Nodes tool): pick the item under the cursor if
    // it is not part of the selection.
    if (QGraphicsItem *it = itemAt(event->pos())) {
        if (it->data(0).isValid() && !it->isSelected()) {
            m_scene->clearSelection();
            it->setSelected(true);
        }
    }
    const QStringList ids = selectedElementIds();
    if (ids.isEmpty())
        return;
    bool convertible = false;
    for (const QString &id : ids)
        if (Element *e = m_doc->elementById(id))
            if (e->geometryType != QLatin1String("path"))
                convertible = true;
    QAction *conv = menu.addAction(tr("Convert to path"));
    conv->setEnabled(convertible);
    connect(conv, &QAction::triggered, this, [this, ids] { convertToPaths(ids); });
    if (m_tool != NodeEdit)
        connect(menu.addAction(tr("Edit nodes  (N)")), &QAction::triggered, this,
                [this] { setTool(NodeEdit); });
    connect(menu.addAction(tr("Delete")), &QAction::triggered, this, [this, ids] {
        QVector<Element> victims;
        for (const QString &id : ids)
            if (Element *e = m_doc->elementById(id))
                victims.append(*e);
        if (!victims.isEmpty())
            m_undo->push(new DeleteCmd(this, m_doc, victims));
    });
    menu.exec(event->globalPos());
}

void Canvas::keyPressEvent(QKeyEvent *event)
{
    if (m_tool == NodeEdit && m_selSub >= 0 && m_doc
        && (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)) {
        if (m_model.subs[m_selSub].removeNode(m_selNode)) {
            m_selSub = m_selNode = -1;
            commitModel(tr("delete node"));
        } else {
            emit statusHint(tr("A path keeps at least two nodes — delete the element instead"));
        }
        return;
    }
    if (m_tool == NodeEdit && event->key() == Qt::Key_Escape && m_selSub >= 0) {
        m_selSub = m_selNode = -1;
        viewport()->update();
        return;
    }
    if (m_tool == DrawPath && !m_penNodes.isEmpty()) {
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
    emitZoom();
}

} // namespace c2d
