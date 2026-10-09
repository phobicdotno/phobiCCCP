#include "canvas.h"
#include "sketchgeom.h"
#include "backgroundimage.h"
#include "sketch.h"
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
static const QColor kConstruction(0x8a, 0xa4, 0xc8);   // dashed reference lines

// Element item with CAD-style state rendering: closed shapes get a faint fill
// (which also makes their interior clickable), hover brightens the outline,
// selection turns it amber — no dashed Qt selection rectangle.
class ElemItem : public QGraphicsPathItem
{
public:
    ElemItem(const QPainterPath &path, bool closed, bool construction = false,
             bool point = false)
        : QGraphicsPathItem(path), m_closed(closed && !construction && !point),
          m_construction(construction), m_point(point)
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
            pen.setColor(m_construction ? kConstruction : kElement);
            pen.setWidthF(1.3);
        }
        // Construction geometry is dashed (and never filled); a sketch
        // point is a cross of fixed screen size over its tiny circle.
        if (m_construction)
            pen.setStyle(Qt::DashLine);
        p->setPen(pen);
        if (m_point) {
            const double k = 5.0 / qMax(1e-9, std::fabs(p->worldTransform().m11()));
            const QPointF c = path().boundingRect().center();
            p->drawLine(c - QPointF(k, k), c + QPointF(k, k));
            p->drawLine(c - QPointF(k, -k), c + QPointF(k, -k));
            return;
        }
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
        if (m_point) {
            QPainterPath hit;
            hit.addEllipse(path().boundingRect().center(), 1.5, 1.5);
            return hit;
        }
        return m_closed ? path() : QGraphicsPathItem::shape();
    }

protected:
    void hoverEnterEvent(QGraphicsSceneHoverEvent *) override { m_hover = true; update(); }
    void hoverLeaveEvent(QGraphicsSceneHoverEvent *) override { m_hover = false; update(); }

private:
    bool m_closed;
    bool m_construction, m_point;
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

    // Toolpath -> shapes. Drawn first, and before every early return below, so
    // the halo stays visible whatever tool is active. A wide translucent pass
    // makes it readable over dark stock, a thin solid pass keeps the outline
    // crisp; both cosmetic, so the halo does not thicken as you zoom in.
    if (!m_highlightIds.isEmpty()) {
        p->save();
        p->setBrush(Qt::NoBrush);
        for (int pass = 0; pass < 2; ++pass) {
            QPen hp(pass == 0 ? QColor(0xff, 0xa5, 0x2a, 0x60)
                              : QColor(0xff, 0xa5, 0x2a));
            hp.setCosmetic(true);
            hp.setWidth(pass == 0 ? 9 : 2);
            hp.setJoinStyle(Qt::RoundJoin);
            hp.setCapStyle(Qt::RoundCap);
            p->setPen(hp);
            for (const QString &id : m_highlightIds)
                if (const QGraphicsPathItem *it = itemFor(id))
                    p->drawPath(it->mapToScene(it->path()));
        }
        p->restore();
    }
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
    if ((m_tool == DrawPath && t != DrawPath) || !m_clicks.isEmpty() || isModifyTool(m_tool)
        || !m_splinePts.isEmpty())
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
    case DrawCircle:
    case DrawRect:    resetSketch(); sketchHint(); break;
    case DrawEllipse: emit statusHint(tr("Ellipse — drag across its bounding box; hold Ctrl to drag from the center")); break;
    case DrawSlot:
    case DrawConic:
    case DrawArc:     emit statusHint(clickHint()); break;
    case DrawPoint:   emit statusHint(tr("Point — click to place a sketch point (drilling toolpaths drill at it)")); break;
    case DrawSpline:  emit statusHint(tr("Spline — click the points it passes through; Enter or double-click finishes, click near the start closes, Esc cancels")); break;
    case Trim:        emit statusHint(tr("Trim — click the piece of a curve to cut away, up to where other curves cross it")); break;
    case Extend:      emit statusHint(tr("Extend — click near the end of an open curve to run it on to the next curve")); break;
    case Break:       emit statusHint(tr("Break — click a curve to split it where other curves cross it")); break;
    case Measure:     emit statusHint(tr("Measure — drag from one point to another (snaps to the grid when Snap is on)")); break;
    case DrawPolygon: emit statusHint(m_polyMode == PolyEdge ? clickHint()
                                      : m_polyMode == PolyCircumscribed
                                          ? tr("Polygon (circumscribed) — press at the center, drag to the middle of an edge")
                                          : tr("Polygon (inscribed) — press at the center, drag to a corner")); break;
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
        auto *item = new ElemItem(e.painterPath, closed, Element::isConstruction(e),
                                  Element::isPoint(e));
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

void Canvas::setVectorHighlight(const QStringList &ids)
{
    if (m_highlightIds == ids)
        return;
    m_highlightIds = ids;
    viewport()->update();
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

// The box a rectangle / ellipse drag spans: corner to corner, or (Ctrl)
// centered on the press point with the cursor at a corner.
static QRectF dragBox(const QPointF &anchor, const QPointF &cur, bool fromCenter)
{
    if (!fromCenter)
        return QRectF(anchor, cur).normalized();
    const QPointF d(std::fabs(cur.x() - anchor.x()), std::fabs(cur.y() - anchor.y()));
    return QRectF(anchor - d, anchor + d);
}

// "Measure" readout: length, the X / Y components and the angle (CCW from +X).
static QString measureText(const QPointF &a, const QPointF &b)
{
    const QPointF d = b - a;
    return QObject::tr("%1 mm   ΔX %2   ΔY %3   %4°")
        .arg(std::hypot(d.x(), d.y()), 0, 'f', 3)
        .arg(d.x(), 0, 'f', 3)
        .arg(d.y(), 0, 'f', 3)
        .arg(qRadiansToDegrees(std::atan2(d.y(), d.x())), 0, 'f', 2);
}

// Distance from p to the infinite line through a and b (to a when a == b).
static double lineDistance(const QPointF &p, const QPointF &a, const QPointF &b)
{
    const QPointF d = b - a;
    const double len = std::hypot(d.x(), d.y());
    if (len < 1e-12)
        return QLineF(p, a).length();
    return std::fabs(d.x() * (p.y() - a.y()) - d.y() * (p.x() - a.x())) / len;
}

bool Canvas::clickTool() const
{
    return isClickTool(m_tool) || (m_tool == DrawPolygon && m_polyMode == PolyEdge);
}

int Canvas::clicksNeeded() const
{
    if (m_tool == DrawArc)
        return m_arcMode == ArcTangent ? 2 : 3;
    if (m_tool == DrawSlot)
        return m_slotMode == SlotArc3Point ? 4 : 3;
    return 3;   // edge polygon
}

QString Canvas::clickHint() const
{
    const int n = m_clicks.size();
    if (m_tool == DrawArc) {
        switch (m_arcMode) {
        case Arc3Point:
            return n == 0 ? tr("Arc (3-point) — click the start")
                 : n == 1 ? tr("Arc: click the end point")
                          : tr("Arc: click a point the arc passes through");
        case ArcCenter:
            return n == 0 ? tr("Arc (center point) — click the center")
                 : n == 1 ? tr("Arc: click the start (sets the radius)")
                          : tr("Arc: move round the way it should go, click the end");
        case ArcTangent:
            return n == 0 ? tr("Arc (tangent) — click near the end of an open line or curve")
                          : tr("Arc: click the end point");
        }
    }
    if (m_tool == DrawSlot) {
        switch (m_slotMode) {
        case SlotCenterToCenter:
            return n == 0 ? tr("Slot (center to center) — click the first end's center")
                 : n == 1 ? tr("Slot: click the second center") : tr("Slot: click to set the width");
        case SlotOverall:
            return n == 0 ? tr("Slot (overall) — click one end of the slot")
                 : n == 1 ? tr("Slot: click the other end") : tr("Slot: click to set the width");
        case SlotCenterPoint:
            return n == 0 ? tr("Slot (center point) — click the slot's middle")
                 : n == 1 ? tr("Slot: click one end's center") : tr("Slot: click to set the width");
        case SlotArc3Point:
            return n == 0 ? tr("Arc slot — click the start of its centerline")
                 : n == 1 ? tr("Arc slot: click the end of the centerline")
                 : n == 2 ? tr("Arc slot: click a point the centerline passes through")
                          : tr("Arc slot: click to set the width");
        }
    }
    if (m_tool == DrawConic)
        return n == 0 ? tr("Conic — click the start")
             : n == 1 ? tr("Conic: click the end")
                      : tr("Conic: click the apex its end tangents meet at (rho sets the fullness)");
    return n == 0 ? tr("Polygon (edge) — click one end of an edge")
         : n == 1 ? tr("Polygon: click the other end of the edge")
                  : tr("Polygon: click the side it goes on");
}

bool Canvas::clickShape(const QVector<QPointF> &pts, Element *out) const
{
    if (!m_doc || pts.size() < clicksNeeded())
        return false;
    const QJsonObject layer = m_doc->defaultLayer();
    const QPointF a = pts.at(0), b = pts.at(1);
    PathModel m;
    if (m_tool == DrawSlot) {
        const QPointF c = pts.at(2);
        if (QLineF(a, b).length() < 1e-6)
            return false;
        if (m_slotMode == SlotArc3Point) {
            QPointF o;
            double r = 0;
            if (!sketch::circleThrough(a, c, b, &o, &r))
                return false;
            m = sketch::arcSlot(a, b, c, 2 * std::fabs(QLineF(o, pts.at(3)).length() - r));
        } else {
            const double w = 2 * lineDistance(c, a, b);
            if (w <= 0.1)
                return false;
            m = m_slotMode == SlotOverall     ? sketch::slotOverall(a, b, w)
              : m_slotMode == SlotCenterPoint ? sketch::slotCenterPoint(a, b, w)
                                              : sketch::slot(a, b, w);
        }
    } else if (m_tool == DrawConic) {
        if (QLineF(a, b).length() < 1e-6)
            return false;
        m = sketch::conic(a, b, pts.at(2), m_conicRho);
    } else if (m_tool == DrawArc) {
        m = m_arcMode == ArcCenter  ? sketch::arcCenter(a, b, pts.at(2), m_arcTurn >= 0)
          : m_arcMode == ArcTangent ? sketch::arcTangent(a, m_tanDir, b)
                                    : sketch::arc3(a, b, pts.at(2));
    } else if (m_tool == DrawPolygon) {
        QPointF ctr;
        double r = 0, rot = 0;
        if (sketch::polygonEdge(a, b, pts.at(2), m_polySides, &ctr, &r, &rot).isEmpty()
            || lineDistance(pts.at(2), a, b) < 1e-9)
            return false;
        *out = Element::makePolygon(ctr, r, m_polySides, layer, rot);
        return true;
    }
    if (m.isEmpty())
        return false;
    *out = Element::makeBezierPath(m, layer);
    return true;
}

QPainterPath Canvas::clickPreview(const QPointF &cur) const
{
    QPainterPath p;
    if (m_clicks.isEmpty())
        return p;
    QVector<QPointF> pts = m_clicks;
    pts.append(cur);
    Element e;
    if (pts.size() >= clicksNeeded() && clickShape(pts, &e)) {
        p = e.painterPath;
        if (m_tool == DrawSlot && m_slotMode != SlotArc3Point) {
            p.moveTo(pts.at(0));   // the line clicked, for reference
            p.lineTo(pts.at(1));
        }
        if (m_tool == DrawConic) {   // the two end tangents through the apex
            p.moveTo(pts.at(0));
            p.lineTo(pts.at(2));
            p.lineTo(pts.at(1));
        }
        return p;
    }
    if (m_tool == DrawSlot && m_slotMode == SlotArc3Point && pts.size() == 3)
        return sketch::arc3(pts.at(0), pts.at(1), pts.at(2)).painterPath();
    if (m_tool == DrawArc && m_arcMode == ArcCenter && pts.size() == 2)
        p.addEllipse(pts.at(0), QLineF(pts.at(0), cur).length(), QLineF(pts.at(0), cur).length());
    p.moveTo(pts.first());
    for (int i = 1; i < pts.size(); ++i)
        p.lineTo(pts.at(i));
    return p;
}

void Canvas::trackArcTurn(const QPointF &cur)
{
    if (m_tool != DrawArc || m_arcMode != ArcCenter || m_clicks.size() != 2)
        return;
    const QPointF c = m_clicks.first();
    if (QLineF(c, cur).length() < 1e-9)
        return;
    const double ang = std::atan2(cur.y() - c.y(), cur.x() - c.x());
    double d = ang - m_arcLastAng;
    while (d > M_PI) d -= 2 * M_PI;
    while (d <= -M_PI) d += 2 * M_PI;
    m_arcTurn += d;
    m_arcLastAng = ang;
}

bool Canvas::pickOpenEnd(const QPointF &q, QPointF *end, QPointF *dir) const
{
    if (!m_doc)
        return false;
    double best = pxToMm(10);
    bool found = false;
    for (const Element &e : m_doc->elements()) {
        if (Element::isPoint(e))
            continue;
        const PathModel m = Element::pathModel(e);
        for (const SubPath &sp : m.subs) {
            if (sp.closed || sp.nodes.size() < 2)
                continue;
            const int n = sp.nodes.size();
            const PathNode &f = sp.nodes.first(), &l = sp.nodes.last();
            // Leaving an end means carrying on the way the curve arrives there.
            const QPointF outFirst = f.hasOut() ? f.p - f.out : f.p - sp.nodes.at(1).p;
            const QPointF outLast = l.hasIn() ? l.p - l.in : l.p - sp.nodes.at(n - 2).p;
            for (const auto &cand : {qMakePair(f.p, outFirst), qMakePair(l.p, outLast)}) {
                const double d = QLineF(cand.first, q).length();
                if (d <= best && std::hypot(cand.second.x(), cand.second.y()) > 1e-9) {
                    best = d;
                    *end = cand.first;
                    *dir = cand.second;
                    found = true;
                }
            }
        }
    }
    return found;
}

void Canvas::clickToolPress(const QPointF &posIn)
{
    QPointF pos = posIn;
    if (!m_clicks.isEmpty() && QLineF(pos, m_clicks.last()).length() < 1e-6)
        return;   // a double-click's second press, or a click on the same spot
    if (m_tool == DrawArc && m_arcMode == ArcTangent && m_clicks.isEmpty()) {
        QPointF end, dir;
        if (!pickOpenEnd(pos, &end, &dir)) {
            emit statusHint(tr("No open end there — click near the end of a line or curve"));
            return;
        }
        pos = end;
        m_tanDir = dir;
    }
    m_clicks.append(pos);
    if (m_tool == DrawArc && m_arcMode == ArcCenter && m_clicks.size() == 2) {
        m_arcTurn = 0;
        m_arcLastAng = std::atan2(pos.y() - m_clicks.first().y(), pos.x() - m_clicks.first().x());
    }
    if (m_clicks.size() < clicksNeeded()) {
        if (!m_preview) {
            QPen pen(kPreview);
            pen.setCosmetic(true);
            pen.setStyle(Qt::DashLine);
            m_preview = m_scene->addPath(QPainterPath(), pen);
        }
        m_preview->setPath(clickPreview(pos));
        emit statusHint(clickHint());
        viewport()->update();
        return;
    }
    trackArcTurn(pos);
    Element e;
    if (clickShape(m_clicks, &e))
        m_undo->push(new AddCmd(this, m_doc, e));
    cancelDrawing();
    emit statusHint(clickHint());
    viewport()->update();
}

void Canvas::finishSpline(bool closed)
{
    while (m_splinePts.size() >= 2
           && QLineF(m_splinePts.last(), m_splinePts.at(m_splinePts.size() - 2)).length() < 1e-9)
        m_splinePts.removeLast();
    if (m_doc && m_splinePts.size() >= 2) {
        const PathModel m = sketch::fitSpline(m_splinePts, closed && m_splinePts.size() >= 3);
        if (!m.isEmpty()) {
            m_undo->push(new AddCmd(this, m_doc, Element::makeBezierPath(m, m_doc->defaultLayer())));
            emit statusHint(m.subs.first().closed ? tr("Closed spline added") : tr("Spline added"));
        }
    }
    cancelDrawing();
    viewport()->update();
}

void Canvas::toggleConstruction(const QStringList &ids)
{
    if (!m_doc)
        return;
    QVector<Element> els;
    bool allOn = true;
    for (const QString &id : ids)
        if (const Element *e = m_doc->elementById(id)) {
            els.append(*e);
            allOn = allOn && Element::isConstruction(*e);
        }
    if (els.isEmpty())
        return;
    const bool on = !allOn;
    m_undo->beginMacro(on ? tr("make %1 vector(s) construction").arg(els.size())
                          : tr("make %1 vector(s) normal").arg(els.size()));
    for (const Element &e : els)
        if (Element::isConstruction(e) != on)
            m_undo->push(new EditCmd(this, m_doc, e, Element::withConstruction(e, on)));
    m_undo->endMacro();
    emit statusHint(on ? tr("%1 vector(s) are construction geometry: drawn dashed, never machined")
                             .arg(els.size())
                       : tr("%1 vector(s) are normal geometry again").arg(els.size()));
}

// ---- trim / extend / break ----------------------------------------------------

bool Canvas::modifyTarget(const QPointF &q, QString *id, PathModel *model,
                          QVector<PathModel> *others) const
{
    if (!m_doc)
        return false;
    const double tol = pxToMm(8);
    double best = tol;
    int bestIdx = -1;
    QVector<PathModel> models;
    const QVector<Element> &els = m_doc->elements();
    for (int i = 0; i < els.size(); ++i) {
        // Text and sketch points: empty, never a target nor a bound.
        models.append(Element::isPoint(els.at(i)) ? PathModel() : Element::pathModel(els.at(i)));
        int sub;
        double g;
        if (!els.at(i).painterPath.boundingRect().adjusted(-tol, -tol, tol, tol).contains(q))
            continue;
        if (sketch::pick(models.last(), q, best, &sub, &g)) {
            const SubPath &sp = models.last().subs.at(sub);
            const int seg = qMin(int(g), sp.segmentCount() - 1);
            best = QLineF(sp.pointAt(seg, g - seg), q).length();
            bestIdx = i;
        }
    }
    if (bestIdx < 0)
        return false;
    *id = els.at(bestIdx).id;
    *model = models.at(bestIdx);
    others->clear();
    for (int i = 0; i < models.size(); ++i)
        if (i != bestIdx && !models.at(i).isEmpty())
            others->append(models.at(i));
    return true;
}

QPainterPath Canvas::modifyPreview(const QPointF &q) const
{
    QString id;
    PathModel m;
    QVector<PathModel> others;
    if (!modifyTarget(q, &id, &m, &others))
        return QPainterPath();
    const double tol = pxToMm(8);
    int sub;
    double g;
    if (!sketch::pick(m, q, tol, &sub, &g))
        return QPainterPath();
    if (m_tool == Extend) {
        PathModel grown = m;
        if (!sketch::extend(grown, q, tol, others))
            return QPainterPath();
        // Just the new stretch: from the old end to the new one.
        const SubPath &a = m.subs.at(sub), &b = grown.subs.at(sub);
        const bool atEnd = QLineF(q, a.nodes.last().p).length() < QLineF(q, a.nodes.first().p).length();
        QPainterPath p(atEnd ? a.nodes.last().p : a.nodes.first().p);
        p.lineTo(atEnd ? b.nodes.last().p : b.nodes.first().p);
        return p;
    }
    double from, to;
    sketch::pieceAround(m, sub, g, sketch::crossings(m, sub, others), &from, &to);
    PathModel piece;
    piece.subs.append(sketch::subRange(m.subs.at(sub), from, to));
    return piece.painterPath();
}

void Canvas::modifyAt(const QPointF &q)
{
    QString id;
    PathModel m;
    QVector<PathModel> others;
    if (!modifyTarget(q, &id, &m, &others)) {
        emit statusHint(tr("No curve there"));
        return;
    }
    const Element before = *m_doc->elementById(id);
    const double tol = pxToMm(8);
    switch (m_tool) {
    case Trim: {
        bool hit = false;
        const PathModel rest = sketch::trim(m, q, tol, others, &hit);
        if (!hit)
            return;
        if (rest.isEmpty())
            m_undo->push(new DeleteCmd(this, m_doc, {before}));
        else
            m_undo->push(new EditCmd(this, m_doc, before, Element::withPathModel(before, rest)));
        emit statusHint(rest.isEmpty() ? tr("Trimmed: nothing crossed it, so the whole curve went")
                                       : tr("Trimmed"));
        break;
    }
    case Extend: {
        if (!sketch::extend(m, q, tol, others)) {
            emit statusHint(tr("Nothing ahead to extend to (closed curves have no ends)"));
            return;
        }
        m_undo->push(new EditCmd(this, m_doc, before, Element::withPathModel(before, m)));
        emit statusHint(tr("Extended"));
        break;
    }
    case Break: {
        const QVector<PathModel> parts = sketch::breakAt(m, q, tol, others);
        if (parts.isEmpty()) {
            emit statusHint(tr("Nothing crosses this curve: nothing to break at"));
            return;
        }
        QVector<Element> after;
        after.append(Element::withPathModel(before, parts.first()));
        const QJsonObject layer = before.raw.value("layer").toObject();
        for (int i = 1; i < parts.size(); ++i)
            after.append(Element::makeBezierPath(parts.at(i), layer));
        m_undo->push(new ReplaceCmd(this, m_doc, {before}, after,
                                    tr("break into %1 pieces").arg(after.size())));
        emit statusHint(tr("Broken into %1 pieces").arg(after.size()));
        break;
    }
    default:
        break;
    }
    if (m_preview)
        m_preview->setPath(modifyPreview(q));
}

// Polygon by dragging from the center: inscribed puts a corner under the
// cursor, circumscribed the middle of an edge (Fusion's two center modes).
static void polygonDrag(QPointF c, QPointF cur, int sides, bool circumscribed,
                        double *radius, double *rotationDeg)
{
    const double d = QLineF(c, cur).length();
    const double ang = d > 1e-12 ? std::atan2(cur.y() - c.y(), cur.x() - c.x()) : 0.0;
    const double half = M_PI / qMax(3, sides);
    *radius = circumscribed ? d / std::cos(half) : d;
    *rotationDeg = qRadiansToDegrees(circumscribed ? ang + half : ang);
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
        p.addRect(dragBox(m_anchor, cur, m_fromCenter));
        break;
    case DrawEllipse: {
        const QRectF b = dragBox(m_anchor, cur, m_fromCenter);
        p.addEllipse(b);
        break;
    }
    case Measure: {
        p.moveTo(m_anchor);
        p.lineTo(cur);
        // Small crosses on both ends.
        const double k = pxToMm(5);
        for (const QPointF &e : {m_anchor, cur}) {
            p.moveTo(e - QPointF(k, k)); p.lineTo(e + QPointF(k, k));
            p.moveTo(e - QPointF(k, -k)); p.lineTo(e + QPointF(k, -k));
        }
        break;
    }
    case DrawPolygon: {
        double r = 0, rot = 0;
        polygonDrag(m_anchor, cur, m_polySides, m_polyMode == PolyCircumscribed, &r, &rot);
        for (int i = 0; i <= m_polySides; ++i) {
            const double a = 2.0 * M_PI * i / m_polySides + qDegreesToRadians(rot);
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
    m_clicks.clear();
    m_tanLines.clear();
    m_penNodes.clear();
    m_penDrag = false;
    m_splinePts.clear();
    m_arcTurn = 0;
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

    if (m_tool == DrawPoint && m_doc && event->button() == Qt::LeftButton) {
        const QPointF pos = snap(mapToScene(event->pos()));
        m_undo->push(new AddCmd(this, m_doc, Element::makePoint(pos, m_doc->defaultLayer())));
        emit statusHint(tr("Point at %1, %2 mm").arg(pos.x(), 0, 'f', 2).arg(pos.y(), 0, 'f', 2));
        event->accept();
        return;
    }

    if (m_tool == DrawSpline && m_doc && event->button() == Qt::LeftButton) {
        const QPointF raw = mapToScene(event->pos());
        const QPointF pos = snap(raw);
        if (m_splinePts.size() >= 3 && QLineF(raw, m_splinePts.first()).length() <= pxToMm(8)) {
            finishSpline(true);
            event->accept();
            return;
        }
        if (m_splinePts.isEmpty() || QLineF(pos, m_splinePts.last()).length() > 1e-9)
            m_splinePts.append(pos);
        if (!m_preview) {
            QPen pen(kPreview);
            pen.setCosmetic(true);
            pen.setStyle(Qt::DashLine);
            m_preview = m_scene->addPath(QPainterPath(), pen);
        }
        m_preview->setPath(sketch::fitSpline(m_splinePts, false).painterPath());
        emit statusHint(tr("Spline: %1 point(s) — Enter or double-click finishes, click near the start closes, Esc cancels")
                            .arg(m_splinePts.size()));
        viewport()->update();
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

    if (clickTool() && m_doc && event->button() == Qt::LeftButton) {
        // The tangent arc's first click picks a curve end: never snapped.
        const bool picks = m_tool == DrawArc && m_arcMode == ArcTangent && m_clicks.isEmpty();
        const QPointF at = mapToScene(event->pos());
        clickToolPress(picks ? at : snap(at));
        event->accept();
        return;
    }

    if (isModifyTool(m_tool) && m_doc && event->button() == Qt::LeftButton) {
        modifyAt(mapToScene(event->pos()));   // never snapped: it picks a curve
        event->accept();
        return;
    }

    // NodeEdit selects and edits; it never starts a new shape, so it must not
    // reach the drawing branch (which would swallow the click before the
    // scene sees it, leaving nothing selectable with the tool).
    if (sketchTool() && m_doc && event->button() == Qt::LeftButton) {
        if (!m_preview) {
            QPen pen(kPreview);
            pen.setCosmetic(true);
            pen.setStyle(Qt::DashLine);
            m_preview = m_scene->addPath(QPainterPath(), pen);
        }
        const QPointF at = mapToScene(event->pos());
        if (m_tanLines.size() < sketchLinesNeeded()) {
            QLineF edge;
            if (pickEdge(at, &edge))
                m_tanLines.append(edge);
            else
                emit statusHint(tr("No edge there — click on an existing line"));
            m_preview->setPath(sketchPath(snap(at)));
            if (m_tanLines.size() < sketchLinesNeeded() || !m_clicks.isEmpty())
                sketchHint();
            else
                emit statusHint(tr("Move to choose the circle, click to place it"));
        } else {
            m_drawing = true;
            m_clicks.append(snap(at));
            if (m_clicks.size() >= sketchPointsNeeded())
                finishSketch(m_clicks.takeLast());
            else
                sketchHint();
        }
        event->accept();
        return;
    }

    if (m_tool != Select && m_tool != DrawPath && m_tool != NodeEdit && m_doc
        && event->button() == Qt::LeftButton) {
        m_drawing = true;
        m_anchor = snap(mapToScene(event->pos()));
        m_fromCenter = event->modifiers() & Qt::ControlModifier;
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

    if (isModifyTool(m_tool) && m_doc) {
        if (!m_preview) {
            QPen pen(QColor(0xe0, 0x50, 0x50), 2.5);
            pen.setCosmetic(true);
            if (m_tool == Extend)
                pen.setStyle(Qt::DashLine);
            m_preview = m_scene->addPath(QPainterPath(), pen);
        }
        m_preview->setPath(modifyPreview(cc));
        event->accept();
        return;
    }

    if (m_tool == DrawSpline && !m_splinePts.isEmpty() && m_preview) {
        m_preview->setPath(sketch::fitSpline(QVector<QPointF>(m_splinePts) << snap(cc), false)
                               .painterPath());
        viewport()->update();
        event->accept();
        return;
    }

    if (clickTool() && !m_clicks.isEmpty() && m_preview) {
        const QPointF cur = snap(cc);
        trackArcTurn(cur);
        m_preview->setPath(clickPreview(cur));
        if (m_tool == DrawSlot && m_slotMode == SlotCenterToCenter && m_clicks.size() == 2)
            emit statusHint(tr("slot %1 mm long, %2 mm wide")
                                .arg(QLineF(m_clicks.at(0), m_clicks.at(1)).length() + 2 * lineDistance(cur, m_clicks.at(0), m_clicks.at(1)), 0, 'f', 2)
                                .arg(2 * lineDistance(cur, m_clicks.at(0), m_clicks.at(1)), 0, 'f', 2));
        else
            emit statusHint(tr("%1 mm from the last point")
                                .arg(QLineF(m_clicks.last(), cur).length(), 0, 'f', 2));
        event->accept();
        return;
    }

    if (sketchTool() && m_preview) {
        m_preview->setPath(sketchPath(snap(cc)));
        viewport()->update();
        event->accept();
        return;
    }

    if ((m_drawing || (m_tool == DrawPath && !m_penNodes.isEmpty())) && m_preview) {
        const QPointF cur = snap(cc);
        if (m_drawing)
            m_fromCenter = event->modifiers() & Qt::ControlModifier;
        m_preview->setPath(previewPath(cur));
        switch (m_tool) {
        case DrawCircle:
        case DrawPolygon:
            emit statusHint(tr("r = %1 mm").arg(QLineF(m_anchor, cur).length(), 0, 'f', 2));
            break;
        case Measure:
            emit statusHint(measureText(m_anchor, cur));
            break;
        case DrawRect:
        case DrawEllipse: {
            const QRectF r = dragBox(m_anchor, cur, m_fromCenter);
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

    // Sketch tools take a point per click; a press-drag-release counts the
    // release as the next point too, so the old drag gesture still works.
    if (sketchTool() && event->button() == Qt::LeftButton) {
        if (m_drawing && !m_clicks.isEmpty() && m_doc) {
            const QPointF cur = snap(mapToScene(event->pos()));
            if (QLineF(cur, m_clicks.last()).length() > pxToMm(4)) {
                if (m_clicks.size() + 1 >= sketchPointsNeeded())
                    finishSketch(cur);
                else {
                    m_clicks.append(cur);
                    sketchHint();
                }
            }
        }
        event->accept();
        return;
    }

    if (m_drawing && m_doc) {
        m_drawing = false;
        m_fromCenter = event->modifiers() & Qt::ControlModifier;
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
            const QRectF rect = dragBox(m_anchor, cur, m_fromCenter);
            if (rect.width() > 0.1 && rect.height() > 0.1)
                m_undo->push(new AddCmd(this, m_doc,
                    Element::makeRectangle(rect.center(), rect.width(), rect.height(), layer)));
            break;
        }
        case Measure:
            emit statusHint(measureText(m_anchor, cur));
            break;
        case DrawEllipse: {
            const QRectF b = dragBox(m_anchor, cur, m_fromCenter);
            if (b.width() > 0.1 && b.height() > 0.1)
                m_undo->push(new AddCmd(this, m_doc, Element::makeBezierPath(
                    sketch::ellipse(b.center(), b.width() / 2, b.height() / 2), layer)));
            break;
        }
        case DrawPolygon: {
            double pr = 0, rot = 0;
            polygonDrag(m_anchor, cur, m_polySides, m_polyMode == PolyCircumscribed, &pr, &rot);
            if (r > 0.1)
                m_undo->push(new AddCmd(this, m_doc,
                    Element::makePolygon(m_anchor, pr, m_polySides, layer, rot)));
            break;
        }
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
        bool dragged = false;
        for (QGraphicsItem *it : m_scene->selectedItems()) {
            QPointF d = it->pos();
            if (!d.isNull() && it->data(0).isValid()) {
                dragged = true;
                if (m_snap) {
                    // The delta is snapped, not the destination, so a drag
                    // shorter than half a cell rounds away to nothing.
                    d = snap(d);
                    if (d.isNull())
                        continue;
                }
                moves.append({it->data(0).toString(), d});
            }
        }
        if (!moves.isEmpty())
            m_undo->push(new MoveCmd(this, m_doc, moves));
        else if (dragged)
            // Every delta snapped away. Without this the items keep the pos()
            // the drag gave them: the shape sits in its new place on screen
            // while the document, the properties panel and the g-code all
            // still have the old one, until some unrelated edit rebuilds.
            rebuild();
    }
}

void Canvas::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (m_tool == DrawPath && !m_penNodes.isEmpty()) {
        finishPath(false);
        event->accept();
        return;
    }
    if (m_tool == DrawSpline && !m_splinePts.isEmpty()) {
        finishSpline(false);
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
    if (m_tool == DrawSpline && !m_splinePts.isEmpty()) {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            finishSpline(false);
            return;
        }
        if (event->key() == Qt::Key_Escape) {
            cancelDrawing();
            emit statusHint(tr("Spline cancelled"));
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


// ---- Fusion-style sketch circles and rectangles ---------------------------

void Canvas::setCircleMode(CircleMode m)
{
    m_circleMode = m;
    if (m_tool == DrawCircle) {
        cancelDrawing();
        sketchHint();
    }
}

void Canvas::setRectMode(RectMode m)
{
    m_rectMode = m;
    if (m_tool == DrawRect) {
        cancelDrawing();
        sketchHint();
    }
}

void Canvas::setArcMode(ArcMode m)
{
    m_arcMode = m;
    if (m_tool == DrawArc) {
        cancelDrawing();
        emit statusHint(clickHint());
    }
}

void Canvas::setPolygonMode(PolygonMode m)
{
    m_polyMode = m;
    if (m_tool == DrawPolygon)
        setTool(DrawPolygon);   // cancels and shows the mode's hint
}

void Canvas::setSlotMode(SlotMode m)
{
    m_slotMode = m;
    if (m_tool == DrawSlot) {
        cancelDrawing();
        emit statusHint(clickHint());
    }
}

void Canvas::resetSketch()
{
    m_clicks.clear();
    m_tanLines.clear();
    if (m_preview) {
        m_scene->removeItem(m_preview);
        delete m_preview;
        m_preview = nullptr;
    }
}

int Canvas::sketchLinesNeeded() const
{
    if (m_tool != DrawCircle)
        return 0;
    if (m_circleMode == Circle2Tangent) return 2;
    if (m_circleMode == Circle3Tangent) return 3;
    return 0;
}

int Canvas::sketchPointsNeeded() const
{
    if (m_tool == DrawCircle) {
        switch (m_circleMode) {
        case CircleCenterDiameter:
        case Circle2Point:   return 2;
        case Circle3Point:   return 3;
        case Circle2Tangent:
        case Circle3Tangent: return 1;   // where to put it
        }
    }
    return m_rectMode == Rect3Point ? 3 : 2;
}

void Canvas::sketchHint()
{
    const int n = m_clicks.size(), l = m_tanLines.size();
    QString h;
    if (m_tool == DrawCircle) {
        switch (m_circleMode) {
        case CircleCenterDiameter:
            h = n == 0 ? tr("Circle (center) — click the center") : tr("Click or drag to the radius");
            break;
        case Circle2Point:
            h = n == 0 ? tr("Circle (2-point) — click one end of the diameter")
                       : tr("Click the other end of the diameter");
            break;
        case Circle3Point:
            h = n == 0 ? tr("Circle (3-point) — click the first point on the circle")
                : n == 1 ? tr("Click the second point") : tr("Click the third point");
            break;
        case Circle2Tangent:
            h = l < 2 ? tr("Circle (2-tangent) — click line %1 of 2").arg(l + 1)
                      : tr("Move to choose the circle, click to place it");
            break;
        case Circle3Tangent:
            h = l < 3 ? tr("Circle (3-tangent) — click line %1 of 3").arg(l + 1)
                      : tr("Move to choose the circle, click to place it");
            break;
        }
    } else {
        switch (m_rectMode) {
        case Rect2Point:
            h = n == 0 ? tr("Rectangle (2-point) — click the first corner")
                       : tr("Click or drag to the opposite corner");
            break;
        case Rect3Point:
            h = n == 0 ? tr("Rectangle (3-point) — click the start of the first edge")
                : n == 1 ? tr("Click the end of the first edge") : tr("Click to set the height");
            break;
        case RectCenter:
            h = n == 0 ? tr("Rectangle (center) — click the center") : tr("Click or drag to a corner");
            break;
        }
    }
    emit statusHint(h + tr("  ·  Esc cancels"));
}

// Nearest straight piece of any element's outline within a few pixels.
// Curves are flattened, so picking one gives its tangent at that spot.
bool Canvas::pickEdge(const QPointF &at, QLineF *edge) const
{
    if (!m_doc)
        return false;
    double best = pxToMm(8);
    bool found = false;
    for (const Element &e : m_doc->elements()) {
        if (!e.painterPath.boundingRect().adjusted(-best, -best, best, best).contains(at))
            continue;
        for (const QPolygonF &poly : e.painterPath.toSubpathPolygons()) {
            for (int i = 0; i + 1 < poly.size(); ++i) {
                const QLineF seg(poly.at(i), poly.at(i + 1));
                const double len2 = seg.dx() * seg.dx() + seg.dy() * seg.dy();
                if (len2 < 1e-12)
                    continue;
                double t = ((at.x() - seg.x1()) * seg.dx() + (at.y() - seg.y1()) * seg.dy()) / len2;
                t = qBound(0.0, t, 1.0);
                const double d = QLineF(seg.pointAt(t), at).length();
                if (d < best) {
                    best = d;
                    *edge = seg;
                    found = true;
                }
            }
        }
    }
    return found;
}

namespace {

struct SketchShape {
    enum Kind { None, Circle, Polygon } kind = None;
    QPointF center;
    double radius = 0;
    QVector<QPointF> corners;
};

QVector<QPointF> boxCorners(const QPointF &a, const QPointF &b)
{
    const QRectF r = QRectF(a, b).normalized();
    return {r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()};
}

} // namespace

static SketchShape sketchShape(Canvas::Tool tool, Canvas::CircleMode cm, Canvas::RectMode rm,
                               const QVector<QPointF> &pts, const QVector<QLineF> &lines,
                               const QPointF &cur)
{
    SketchShape s;
    const int n = pts.size();
    auto circle = [&](const sketch::Circle &c) {
        if (c.valid) { s.kind = SketchShape::Circle; s.center = c.center; s.radius = c.radius; }
    };
    if (tool == Canvas::DrawCircle) {
        switch (cm) {
        case Canvas::CircleCenterDiameter:
            if (n >= 1) circle({pts[0], QLineF(pts[0], cur).length(), true});
            break;
        case Canvas::Circle2Point:
            if (n >= 1) circle({(pts[0] + cur) / 2.0, QLineF(pts[0], cur).length() / 2.0, true});
            break;
        case Canvas::Circle3Point:
            if (n >= 2) circle(sketch::circleThrough3(pts[0], pts[1], cur));
            break;
        case Canvas::Circle2Tangent:
            if (lines.size() >= 2) circle(sketch::circleTangent2(lines[0], lines[1], cur));
            break;
        case Canvas::Circle3Tangent:
            if (lines.size() >= 3) circle(sketch::circleTangent3(lines[0], lines[1], lines[2], cur));
            break;
        }
    } else if (n >= 1) {
        QVector<QPointF> c;
        switch (rm) {
        case Canvas::Rect2Point: c = boxCorners(pts[0], cur); break;
        case Canvas::RectCenter: c = boxCorners(pts[0] * 2.0 - cur, cur); break;
        case Canvas::Rect3Point: if (n >= 2) c = sketch::rect3Point(pts[0], pts[1], cur); break;
        }
        if (c.size() == 4) { s.kind = SketchShape::Polygon; s.corners = c; }
    }
    if (s.kind == SketchShape::Circle && s.radius < 1e-9)
        s.kind = SketchShape::None;
    return s;
}

QPainterPath Canvas::sketchPath(const QPointF &cur) const
{
    QPainterPath p;
    for (const QLineF &l : m_tanLines) {           // picked edges, drawn long
        const QPointF d = (l.p2() - l.p1()) / qMax(1e-9, l.length()) * pxToMm(30);
        p.moveTo(l.p1() - d);
        p.lineTo(l.p2() + d);
    }
    if (m_tool == DrawRect && m_rectMode == Rect3Point && m_clicks.size() == 1) {
        p.moveTo(m_clicks[0]);                     // first edge rubber band
        p.lineTo(cur);
    }
    if (m_tool == DrawCircle && m_circleMode == Circle3Point && m_clicks.size() == 1) {
        p.moveTo(m_clicks[0]);
        p.lineTo(cur);
    }
    if (m_tanLines.size() < sketchLinesNeeded())
        return p;
    const SketchShape s = sketchShape(m_tool, m_circleMode, m_rectMode, m_clicks, m_tanLines, cur);
    if (s.kind == SketchShape::Circle)
        p.addEllipse(s.center, s.radius, s.radius);
    else if (s.kind == SketchShape::Polygon)
        p.addPolygon(QPolygonF(s.corners) << s.corners.first());
    return p;
}

void Canvas::finishSketch(const QPointF &cur)
{
    const SketchShape s = sketchShape(m_tool, m_circleMode, m_rectMode, m_clicks, m_tanLines, cur);
    resetSketch();
    m_drawing = false;
    if (m_doc) {
        const QJsonObject layer = m_doc->defaultLayer();
        if (s.kind == SketchShape::Circle && s.radius > 0.05) {
            m_undo->push(new AddCmd(this, m_doc, Element::makeCircle(s.center, s.radius, layer)));
        } else if (s.kind == SketchShape::Polygon) {
            const QRectF box = QPolygonF(s.corners).boundingRect();
            if (QLineF(s.corners[0], s.corners[1]).length() > 0.05
                && QLineF(s.corners[1], s.corners[2]).length() > 0.05) {
                // A tilted rectangle has no CC rectangle form: it becomes a path.
                if (sketch::isAxisAligned(s.corners))
                    m_undo->push(new AddCmd(this, m_doc, Element::makeRectangle(
                        box.center(), box.width(), box.height(), layer)));
                else
                    m_undo->push(new AddCmd(this, m_doc,
                        Element::makePath(s.corners, true, layer)));
            }
        }
    }
    sketchHint();
}

} // namespace c2d
