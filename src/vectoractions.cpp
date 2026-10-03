#include "vectoractions.h"
#include "canvas.h"

#include <QAction>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineF>
#include <QSpinBox>
#include <QHash>
#include <QIcon>
#include <QJsonArray>
#include <QMainWindow>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QRadioButton>
#include <QSet>
#include <QToolBar>
#include <QUndoCommand>
#include <QUndoStack>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace c2d {

namespace {

// ---- undo commands -------------------------------------------------------

void refresh(Canvas *c, const QStringList &select)
{
    c->rebuild();
    emit c->documentChanged();
    c->selectIds(select);
}

// Toolpaths reference elements by uuid; when an operation consumes its
// inputs, every toolpath that pointed at one of them is pointed at the
// result elements instead (so a pocket on two circles becomes a pocket on
// their weld). Returns (before, after) pairs for the toolpaths that change.
QVector<QPair<Toolpath, Toolpath>> retargetToolpaths(Document *d, const QSet<QString> &removed,
                                                     const QVector<Element> &added)
{
    QVector<QPair<Toolpath, Toolpath>> out;
    if (removed.isEmpty())
        return out;
    for (const Toolpath &t : d->toolpaths()) {
        const QJsonArray refs = t.json.value("elements").toArray();
        bool hit = false;
        QJsonArray kept;
        QSet<QString> seen;
        for (const QJsonValue &v : refs) {
            const QString id = v.toObject().value("uuid").toString();
            if (removed.contains(id)) { hit = true; continue; }
            kept.append(v);
            seen.insert(id);
        }
        if (!hit)
            continue;
        for (const Element &e : added)
            if (!seen.contains(e.id))
                kept.append(QJsonObject{{QStringLiteral("uuid"), e.id}});
        Toolpath after = t;
        after.json.insert(QStringLiteral("elements"), kept);
        out.append({t, after});
    }
    return out;
}

// Replace `inputs` (unless kept) by `results`; results are inserted where the
// first consumed input sat so the z-order stays put.
class ReplaceCmd : public QUndoCommand
{
public:
    ReplaceCmd(Canvas *c, Document *d, const QVector<Element> &inputs,
               const QVector<Element> &results, bool keepInputs, const QString &text)
        : m_c(c), m_d(d), m_results(results)
    {
        setText(text);
        if (!keepInputs) {
            QSet<QString> removed;
            const QVector<Element> &all = d->elements();
            for (const Element &e : inputs) {
                for (int i = 0; i < all.size(); ++i)
                    if (all.at(i).id == e.id) { m_removed.append({i, all.at(i)}); break; }
                removed.insert(e.id);
            }
            std::sort(m_removed.begin(), m_removed.end(),
                      [](const auto &a, const auto &b) { return a.first < b.first; });
            m_tps = retargetToolpaths(d, removed, results);
        }
        for (const Element &e : results)
            m_resultIds << e.id;
    }
    void redo() override
    {
        QVector<Element> &els = m_d->elementsRef();
        int at = els.size();
        for (int i = m_removed.size() - 1; i >= 0; --i) {   // high index first
            const int idx = m_removed.at(i).first;
            // The document can have been replaced under the undo stack (a
            // failed Open used to leave it empty). undo() already clamps;
            // without the same here, Redo indexed off the end of the vector.
            if (idx < 0 || idx >= els.size())
                continue;
            els.removeAt(idx);
            at = idx;
        }
        for (int i = 0; i < m_results.size(); ++i)
            els.insert(at + i, m_results.at(i));
        for (const auto &p : m_tps)
            m_d->replaceToolpath(p.second);
        refresh(m_c, m_resultIds);
    }
    void undo() override
    {
        QStringList inputIds;
        for (const Element &e : m_results)
            m_d->removeElementById(e.id);
        QVector<Element> &els = m_d->elementsRef();
        for (const auto &p : m_removed) {              // low index first
            els.insert(qMin(p.first, els.size()), p.second);
            inputIds << p.second.id;
        }
        for (const auto &p : m_tps)
            m_d->replaceToolpath(p.first);
        refresh(m_c, inputIds);
    }
private:
    Canvas *m_c; Document *m_d;
    QVector<Element> m_results;
    QVector<QPair<int, Element>> m_removed;
    QVector<QPair<Toolpath, Toolpath>> m_tps;
    QStringList m_resultIds;
};

class MoveManyCmd : public QUndoCommand
{
public:
    MoveManyCmd(Canvas *c, Document *d, const QStringList &ids, const QVector<QPointF> &deltas,
                const QString &text)
        : m_c(c), m_d(d), m_ids(ids), m_deltas(deltas) { setText(text); }
    void redo() override { apply(1.0); }
    void undo() override { apply(-1.0); }
private:
    void apply(double sign)
    {
        for (int i = 0; i < m_ids.size(); ++i)
            if (Element *e = m_d->elementById(m_ids.at(i)))
                e->translate(sign * m_deltas.at(i).x(), sign * m_deltas.at(i).y());
        refresh(m_c, m_ids);
    }
    Canvas *m_c; Document *m_d; QStringList m_ids; QVector<QPointF> m_deltas;
};

// Swap elements for edited versions of themselves (same ids, same places in
// the z-order) — mirror's command.
class ReshapeCmd : public QUndoCommand
{
public:
    ReshapeCmd(Canvas *c, Document *d, const QVector<Element> &before,
               const QVector<Element> &after, const QString &text)
        : m_c(c), m_d(d), m_before(before), m_after(after) { setText(text); }
    void redo() override { apply(m_after); }
    void undo() override { apply(m_before); }
private:
    void apply(const QVector<Element> &els)
    {
        QStringList ids;
        for (const Element &e : els) {
            m_d->replaceElement(e);
            ids << e.id;
        }
        refresh(m_c, ids);
    }
    Canvas *m_c; Document *m_d; QVector<Element> m_before, m_after;
};

// Add new elements on top of the drawing (the array's copies), and point the
// toolpaths in `tps` at them as well.
class AddElementsCmd : public QUndoCommand
{
public:
    AddElementsCmd(Canvas *c, Document *d, const QVector<Element> &added,
                   const QVector<QPair<Toolpath, Toolpath>> &tps, const QStringList &before,
                   const QString &text)
        : m_c(c), m_d(d), m_added(added), m_tps(tps), m_before(before) { setText(text); }
    void redo() override
    {
        QStringList ids = m_before;
        for (const Element &e : m_added) {
            m_d->addElement(e);
            ids << e.id;
        }
        for (const auto &p : m_tps)
            m_d->replaceToolpath(p.second);
        refresh(m_c, ids);
    }
    void undo() override
    {
        for (const Element &e : m_added)
            m_d->removeElementById(e.id);
        for (const auto &p : m_tps)
            m_d->replaceToolpath(p.first);
        refresh(m_c, m_before);
    }
private:
    Canvas *m_c; Document *m_d;
    QVector<Element> m_added;
    QVector<QPair<Toolpath, Toolpath>> m_tps;
    QStringList m_before;
};

// Toolpaths that machine an original also machine its copies: for every
// toolpath referencing one of the keys of `copiesOf`, the copies' uuids are
// appended to its vectors.
QVector<QPair<Toolpath, Toolpath>> extendToolpaths(Document *d,
                                                   const QHash<QString, QStringList> &copiesOf)
{
    QVector<QPair<Toolpath, Toolpath>> out;
    for (const Toolpath &t : d->toolpaths()) {
        QJsonArray refs = t.json.value("elements").toArray();
        QSet<QString> seen;
        for (const QJsonValue &v : refs)
            seen.insert(v.toObject().value("uuid").toString());
        bool hit = false;
        const QJsonArray orig = refs;
        for (const QJsonValue &v : orig) {
            for (const QString &id : copiesOf.value(v.toObject().value("uuid").toString())) {
                if (seen.contains(id))
                    continue;
                refs.append(QJsonObject{{QStringLiteral("uuid"), id}});
                seen.insert(id);
                hit = true;
            }
        }
        if (!hit)
            continue;
        Toolpath after = t;
        after.json.insert(QStringLiteral("elements"), refs);
        out.append({t, after});
    }
    return out;
}

// ---- icons ---------------------------------------------------------------

QIcon vecIcon(const QString &kind)
{
    QPixmap pm(20, 20);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor ink(0xd8, 0xdc, 0xe4);
    QPen pen(ink, 1.5);
    p.setPen(pen);
    const QRectF a(3, 5, 10, 10), b(7, 5, 10, 10);
    if (kind == "union" || kind == "subtract" || kind == "intersect") {
        QPainterPath pa, pb;
        pa.addEllipse(a);
        pb.addEllipse(b);
        QPainterPath fill = kind == "union" ? pa.united(pb)
                          : kind == "subtract" ? pa.subtracted(pb)
                                               : pa.intersected(pb);
        p.setBrush(QColor(ink.red(), ink.green(), ink.blue(), 110));
        p.setPen(Qt::NoPen);
        p.drawPath(fill);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(a);
        p.drawEllipse(b);
    } else if (kind == "offset") {
        p.drawRoundedRect(QRectF(6, 6, 8, 8), 1.5, 1.5);
        pen.setStyle(Qt::DotLine);
        p.setPen(pen);
        p.drawRoundedRect(QRectF(2.5, 2.5, 15, 15), 4, 4);
    } else if (kind == "center") {
        p.drawRect(QRectF(2.5, 2.5, 15, 15));
        p.setBrush(ink);
        p.drawRect(QRectF(7, 7, 6, 6));
    } else if (kind == "mirrorh") {
        p.drawLine(QLineF(10, 2, 10, 18));
        p.setBrush(ink);
        p.drawPolygon(QPolygonF({QPointF(8, 4), QPointF(8, 16), QPointF(2, 16)}));
        p.setBrush(Qt::NoBrush);
        p.drawPolygon(QPolygonF({QPointF(12, 4), QPointF(12, 16), QPointF(18, 16)}));
    } else if (kind == "mirrorv") {
        p.drawLine(QLineF(2, 10, 18, 10));
        p.setBrush(ink);
        p.drawPolygon(QPolygonF({QPointF(4, 8), QPointF(16, 8), QPointF(16, 2)}));
        p.setBrush(Qt::NoBrush);
        p.drawPolygon(QPolygonF({QPointF(4, 12), QPointF(16, 12), QPointF(16, 18)}));
    } else if (kind == "grid") {
        p.setBrush(ink);
        p.drawRect(QRectF(3, 3, 5, 5));
        p.setBrush(Qt::NoBrush);
        for (const QPointF &o : {QPointF(12, 3), QPointF(3, 12), QPointF(12, 12)})
            p.drawRect(QRectF(o, QSizeF(5, 5)));
    } else if (kind == "circular") {
        p.setBrush(ink);
        p.drawEllipse(QPointF(10, 3.5), 2.2, 2.2);
        p.setBrush(Qt::NoBrush);
        for (int i = 1; i < 6; ++i) {
            const double a = M_PI / 2 + i * 2 * M_PI / 6;
            p.drawEllipse(QPointF(10 + 6.5 * std::cos(a), 10 - 6.5 * std::sin(a)), 2.2, 2.2);
        }
    } else if (kind == "alignh") {
        p.drawLine(QLineF(10, 2, 10, 18));
        p.setBrush(ink);
        p.drawRect(QRectF(4, 5, 12, 4));
        p.drawRect(QRectF(6, 11, 8, 4));
    } else if (kind == "alignv") {
        p.drawLine(QLineF(2, 10, 18, 10));
        p.setBrush(ink);
        p.drawRect(QRectF(5, 4, 4, 12));
        p.drawRect(QRectF(11, 6, 4, 8));
    }
    return QIcon(pm);
}

} // namespace

// ---- VectorActions ---------------------------------------------------------

VectorActions::VectorActions(Canvas *canvas, QMenu *editMenu, QMainWindow *window)
    : QObject(window), m_canvas(canvas)
{
    connect(m_canvas, &Canvas::selectionChangedIds, this, &VectorActions::onSelection);

    editMenu->addSeparator();
    QMenu *m = editMenu->addMenu(QStringLiteral("&Vectors"));
    auto add = [&](QMenu *menu, const QString &text, const QKeySequence &key,
                   QVector<QAction *> &group, auto slot, const QString &tip = QString()) {
        QAction *a = menu->addAction(text);
        if (!key.isEmpty())
            a->setShortcut(key);
        a->setShortcutContext(Qt::WindowShortcut);
        a->setToolTip(tip.isEmpty() ? text : tip);
        a->setStatusTip(a->toolTip());
        connect(a, &QAction::triggered, this, slot);
        group.append(a);
        return a;
    };
    const auto S = [](int k) { return QKeySequence(Qt::CTRL | Qt::SHIFT | k); };
    const auto A = [](int k) { return QKeySequence(Qt::CTRL | Qt::ALT | k); };

    QAction *unionAct = add(m, QStringLiteral("&Union (Weld)"), S(Qt::Key_U), m_needTwo,
        [this] { boolean(vec::BoolOp::Union); },
        QStringLiteral("Weld the selected closed vectors into one outline  (Ctrl+Shift+U)"));
    QAction *subAct = add(m, QStringLiteral("&Subtract"), S(Qt::Key_D), m_needTwo,
        [this] { boolean(vec::BoolOp::Subtract); },
        QStringLiteral("First-selected vector minus the others  (Ctrl+Shift+D)"));
    QAction *interAct = add(m, QStringLiteral("&Intersect"), S(Qt::Key_I), m_needTwo,
        [this] { boolean(vec::BoolOp::Intersect); },
        QStringLiteral("Keep only the area common to all selected vectors  (Ctrl+Shift+I)"));
    m->addSeparator();
    QAction *offsetAct = add(m, QStringLiteral("&Offset…"), S(Qt::Key_O), m_needOne,
        [this] { offset(); },
        QStringLiteral("Offset the selection inward or outward by a distance  (Ctrl+Shift+O)"));
    m->addSeparator();

    QMenu *al = m->addMenu(QStringLiteral("&Align"));
    add(al, QStringLiteral("&Left"), S(Qt::Key_Left), m_needTwo, [this] { align(vec::Align::Left); });
    add(al, QStringLiteral("&Horizontal center"), S(Qt::Key_H), m_needTwo,
        [this] { align(vec::Align::HCenter); });
    add(al, QStringLiteral("&Right"), S(Qt::Key_Right), m_needTwo, [this] { align(vec::Align::Right); });
    al->addSeparator();
    add(al, QStringLiteral("&Top"), S(Qt::Key_Up), m_needTwo, [this] { align(vec::Align::Top); });
    add(al, QStringLiteral("&Vertical center"), S(Qt::Key_V), m_needTwo,
        [this] { align(vec::Align::VCenter); });
    add(al, QStringLiteral("&Bottom"), S(Qt::Key_Down), m_needTwo, [this] { align(vec::Align::Bottom); });

    QMenu *cs = m->addMenu(QStringLiteral("&Center on stock"));
    QAction *centerAct = add(cs, QStringLiteral("&Both"), S(Qt::Key_C), m_needOne,
        [this] { centerOnStock(vec::Center::Both); },
        QStringLiteral("Center the selection on the stock  (Ctrl+Shift+C)"));
    add(cs, QStringLiteral("&Horizontally"), QKeySequence(), m_needOne,
        [this] { centerOnStock(vec::Center::Horizontal); });
    add(cs, QStringLiteral("&Vertically"), QKeySequence(), m_needOne,
        [this] { centerOnStock(vec::Center::Vertical); });

    QMenu *ds = m->addMenu(QStringLiteral("&Distribute"));
    add(ds, QStringLiteral("&Horizontally"), A(Qt::Key_H), m_needThree,
        [this] { distribute(vec::Axis::Horizontal); },
        QStringLiteral("Space the selected vectors' centers evenly left to right  (Ctrl+Alt+H)"));
    add(ds, QStringLiteral("&Vertically"), A(Qt::Key_V), m_needThree,
        [this] { distribute(vec::Axis::Vertical); },
        QStringLiteral("Space the selected vectors' centers evenly bottom to top  (Ctrl+Alt+V)"));

    m->addSeparator();
    QAction *mirrorH = add(m, QStringLiteral("&Mirror horizontally"), A(Qt::Key_M), m_needOne,
        [this] { mirror(vec::Axis::Horizontal); },
        QStringLiteral("Flip the selection left to right about its own center  (Ctrl+Alt+M)"));
    QAction *mirrorV = add(m, QStringLiteral("Mirror &vertically"), A(Qt::Key_F), m_needOne,
        [this] { mirror(vec::Axis::Vertical); },
        QStringLiteral("Flip the selection top to bottom about its own center  (Ctrl+Alt+F)"));
    QAction *gridAct = add(m, QStringLiteral("&Grid array…"), QKeySequence(), m_needOne,
        [this] { gridArrayDialog(); },
        QStringLiteral("Copy the selection into rows and columns"));
    QAction *circAct = add(m, QStringLiteral("&Circular array…"), QKeySequence(), m_needOne,
        [this] { circularArrayDialog(); },
        QStringLiteral("Copy the selection around a center point"));
    mirrorH->setIcon(vecIcon(QStringLiteral("mirrorh")));
    mirrorV->setIcon(vecIcon(QStringLiteral("mirrorv")));
    gridAct->setIcon(vecIcon(QStringLiteral("grid")));
    circAct->setIcon(vecIcon(QStringLiteral("circular")));

    // Icon toolbar for the everyday ones.
    unionAct->setIcon(vecIcon(QStringLiteral("union")));
    subAct->setIcon(vecIcon(QStringLiteral("subtract")));
    interAct->setIcon(vecIcon(QStringLiteral("intersect")));
    offsetAct->setIcon(vecIcon(QStringLiteral("offset")));
    centerAct->setIcon(vecIcon(QStringLiteral("center")));
    auto *tb = window->addToolBar(QStringLiteral("Vectors"));
    tb->setMovable(false);
    tb->setToolButtonStyle(Qt::ToolButtonIconOnly);
    tb->setIconSize(QSize(20, 20));
    for (QAction *a : {unionAct, subAct, interAct, offsetAct, centerAct})
        tb->addAction(a);
    tb->addSeparator();
    for (QAction *a : {mirrorH, mirrorV, gridAct, circAct})
        tb->addAction(a);

    updateEnabled();
}

void VectorActions::onSelection(const QStringList &ids)
{
    // Keep the ids still selected, in their old order; append the newcomers.
    QStringList next;
    for (const QString &id : m_order)
        if (ids.contains(id))
            next << id;
    QStringList fresh;
    for (const QString &id : ids)
        if (!next.contains(id))
            fresh << id;
    if (fresh.size() > 1) {
        // Batch selection has no click order: biggest bounding box first, so
        // Subtract on a rubber-banded base + cut-outs does the expected thing.
        Document *d = m_canvas->document();
        auto area = [d](const QString &id) {
            const Element *e = d ? d->elementById(id) : nullptr;
            const QRectF r = e ? e->painterPath.boundingRect() : QRectF();
            return r.width() * r.height();
        };
        std::stable_sort(fresh.begin(), fresh.end(),
                         [&](const QString &a, const QString &b) { return area(a) > area(b); });
    }
    m_order = next + fresh;
    updateEnabled();
}

void VectorActions::updateEnabled()
{
    const int n = m_order.size();
    for (QAction *a : m_needOne) a->setEnabled(n >= 1);
    for (QAction *a : m_needTwo) a->setEnabled(n >= 2);
    for (QAction *a : m_needThree) a->setEnabled(n >= 3);
}

QVector<Element> VectorActions::orderedSelection() const
{
    QVector<Element> out;
    Document *d = m_canvas->document();
    if (!d)
        return out;
    for (const QString &id : m_order)
        if (const Element *e = d->elementById(id))
            out.append(*e);
    return out;
}

void VectorActions::pushReplace(const QVector<Element> &inputs, const QVector<Element> &results,
                                bool keepInputs, const QString &text)
{
    m_canvas->undoStack()->push(
        new ReplaceCmd(m_canvas, m_canvas->document(), inputs, results, keepInputs, text));
}

void VectorActions::pushMoves(const QStringList &ids, const QVector<QPointF> &deltas,
                              const QString &text)
{
    bool any = false;
    for (const QPointF &d : deltas)
        if (!qFuzzyIsNull(d.x()) || !qFuzzyIsNull(d.y())) { any = true; break; }
    if (!any)
        return;
    m_canvas->undoStack()->push(new MoveManyCmd(m_canvas, m_canvas->document(), ids, deltas, text));
}

void VectorActions::boolean(vec::BoolOp op)
{
    const QVector<Element> inputs = orderedSelection();
    if (inputs.size() < 2)
        return;
    // Only closed vectors take part - booleanElements skips the rest. They
    // must also be the only ones replaced: handing the whole selection to
    // pushReplace deleted every open path in it, without it ever having
    // contributed to the result.
    QVector<Element> closedInputs;
    for (const Element &e : inputs)
        if (vec::isClosed(e))
            closedInputs.append(e);
    const QString name = op == vec::BoolOp::Union ? tr("union")
                       : op == vec::BoolOp::Subtract ? tr("subtract") : tr("intersect");
    if (closedInputs.size() < 2) {
        emit m_canvas->statusHint(tr("Booleans need at least two closed vectors"));
        return;
    }
    // Subtract is "the first selected vector minus the rest". If the first one
    // is open it is skipped entirely, every closed vector becomes a clip, and
    // the empty result got reported as "vectors do not overlap" - which was
    // never the reason.
    if (op == vec::BoolOp::Subtract && !vec::isClosed(inputs.first())) {
        emit m_canvas->statusHint(
            tr("Subtract takes the first selected vector minus the rest — "
               "select a closed vector first"));
        return;
    }
    const QVector<Element> results = vec::booleanElements(closedInputs, op);
    if (results.isEmpty()) {
        emit m_canvas->statusHint(tr("%1: empty result — vectors do not overlap").arg(name));
        return;
    }
    pushReplace(closedInputs, results, false,
                tr("%1 %2 vectors").arg(name).arg(closedInputs.size()));
    emit m_canvas->statusHint(tr("%1: %2 vector(s) → %3 closed path(s)")
                                  .arg(name).arg(closedInputs.size()).arg(results.size()));
}

void VectorActions::offset()
{
    const QVector<Element> inputs = orderedSelection();
    if (inputs.isEmpty())
        return;

    QDialog dlg(m_canvas->window());
    dlg.setWindowTitle(tr("Offset vectors"));
    auto *form = new QFormLayout;
    auto *dist = new QDoubleSpinBox(&dlg);
    dist->setRange(0.001, 1000.0);
    dist->setDecimals(3);
    dist->setSuffix(QStringLiteral(" mm"));
    dist->setValue(3.175);
    form->addRow(tr("Distance"), dist);
    auto *outside = new QRadioButton(tr("Outside (grow)"), &dlg);
    auto *inside = new QRadioButton(tr("Inside (shrink)"), &dlg);
    outside->setChecked(true);
    auto *dirRow = new QVBoxLayout;
    dirRow->addWidget(outside);
    dirRow->addWidget(inside);
    form->addRow(tr("Direction"), dirRow);
    auto *keep = new QCheckBox(tr("Keep original vectors"), &dlg);
    keep->setChecked(true);
    form->addRow(QString(), keep);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *lay = new QVBoxLayout(&dlg);
    lay->addLayout(form);
    lay->addWidget(buttons);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const double delta = inside->isChecked() ? -dist->value() : dist->value();
    const QVector<Element> results = vec::offsetElements(inputs, delta);
    if (results.isEmpty()) {
        emit m_canvas->statusHint(tr("Offset: nothing left — inset larger than the shape"));
        return;
    }
    pushReplace(inputs, results, keep->isChecked(),
                tr("offset %1 vector(s) by %2 mm").arg(inputs.size()).arg(delta));
    emit m_canvas->statusHint(tr("Offset by %1 mm: %2 closed path(s)").arg(delta).arg(results.size()));
}

static QVector<QRectF> boxesOf(const QVector<Element> &els)
{
    QVector<QRectF> b;
    for (const Element &e : els)
        b.append(e.painterPath.boundingRect());
    return b;
}

static QStringList idsOf(const QVector<Element> &els)
{
    QStringList ids;
    for (const Element &e : els)
        ids << e.id;
    return ids;
}

void VectorActions::align(vec::Align mode)
{
    const QVector<Element> els = orderedSelection();
    if (els.size() < 2)
        return;
    const QVector<QRectF> boxes = boxesOf(els);
    QRectF ref;
    for (const QRectF &b : boxes)
        ref = ref.isNull() ? b : ref.united(b);
    pushMoves(idsOf(els), vec::alignDeltas(boxes, mode, ref), tr("align %1 vectors").arg(els.size()));
}

void VectorActions::centerOnStock(vec::Center mode)
{
    const QVector<Element> els = orderedSelection();
    Document *d = m_canvas->document();
    if (els.isEmpty() || !d)
        return;
    const QRectF stock(0, 0, d->boardWidth(), d->boardHeight());
    pushMoves(idsOf(els), vec::centerDeltas(boxesOf(els), mode, stock),
              tr("center %1 vector(s) on stock").arg(els.size()));
}

void VectorActions::distribute(vec::Axis axis)
{
    const QVector<Element> els = orderedSelection();
    if (els.size() < 3)
        return;
    pushMoves(idsOf(els), vec::distributeDeltas(boxesOf(els), axis),
              tr("distribute %1 vectors").arg(els.size()));
}

// ---- mirror and array ------------------------------------------------------

static QRectF selectionBox(const QVector<Element> &els)
{
    QRectF box;
    for (const Element &e : els) {
        const QRectF b = e.painterPath.boundingRect();
        box = box.isNull() ? b : box.united(b);
    }
    return box;
}

void VectorActions::mirror(vec::Axis axis)
{
    const QVector<Element> before = orderedSelection();
    if (before.isEmpty())
        return;
    const QTransform t = vec::mirrorTransform(selectionBox(before), axis);
    QVector<Element> after;
    for (const Element &e : before)
        after.append(vec::transformElement(e, t));
    const QString dir = axis == vec::Axis::Horizontal ? tr("horizontally") : tr("vertically");
    m_canvas->undoStack()->push(new ReshapeCmd(m_canvas, m_canvas->document(), before, after,
        tr("mirror %1 vector(s) %2").arg(before.size()).arg(dir)));
    emit m_canvas->statusHint(tr("Mirrored %1 vector(s) %2").arg(before.size()).arg(dir));
}

void VectorActions::pushCopies(const QVector<Element> &originals,
                               const QVector<QTransform> &placements, bool joinToolpaths,
                               const QString &text)
{
    Document *d = m_canvas->document();
    if (!d || originals.isEmpty() || placements.isEmpty())
        return;
    QVector<Element> added;
    QHash<QString, QStringList> copiesOf;
    for (const QTransform &t : placements) {
        const QVector<Element> copy = vec::copyElements(originals, t);
        for (int i = 0; i < copy.size(); ++i)
            copiesOf[originals.at(i).id] << copy.at(i).id;
        added += copy;
    }
    const auto tps = joinToolpaths ? extendToolpaths(d, copiesOf)
                                   : QVector<QPair<Toolpath, Toolpath>>();
    m_canvas->undoStack()->push(
        new AddElementsCmd(m_canvas, d, added, tps, idsOf(originals), text));
    QString hint = tr("%1 cop(ies) of %2 vector(s)").arg(placements.size()).arg(originals.size());
    if (!tps.isEmpty())
        hint += tr("; added to %1 toolpath(s)").arg(tps.size());
    emit m_canvas->statusHint(hint);
}

void VectorActions::gridArray(int cols, int rows, double gapX, double gapY, bool joinToolpaths)
{
    const QVector<Element> els = orderedSelection();
    if (els.isEmpty())
        return;
    pushCopies(els, vec::gridTransforms(selectionBox(els), cols, rows, gapX, gapY), joinToolpaths,
               tr("grid array %1 x %2").arg(cols).arg(rows));
}

void VectorActions::circularArray(QPointF center, int count, double spanDeg, bool rotate,
                                  bool joinToolpaths)
{
    const QVector<Element> els = orderedSelection();
    if (els.isEmpty())
        return;
    pushCopies(els, vec::circularTransforms(selectionBox(els), center, count, spanDeg, rotate),
               joinToolpaths, tr("circular array of %1").arg(count));
}

// The two array dialogs share their tail: the toolpath option and the buttons.
static QCheckBox *finishArrayDialog(QDialog &dlg, QFormLayout *form)
{
    auto *join = new QCheckBox(QObject::tr("Add the copies to the originals' toolpaths"), &dlg);
    join->setChecked(true);
    join->setToolTip(QObject::tr("A toolpath that machines a selected vector machines its copies too"));
    form->addRow(QString(), join);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *lay = new QVBoxLayout(&dlg);
    lay->addLayout(form);
    lay->addWidget(buttons);
    return join;
}

static QDoubleSpinBox *mmBox(QWidget *parent, double lo, double hi, double value)
{
    auto *b = new QDoubleSpinBox(parent);
    b->setRange(lo, hi);
    b->setDecimals(3);
    b->setSuffix(QStringLiteral(" mm"));
    b->setValue(value);
    return b;
}

void VectorActions::gridArrayDialog()
{
    const QVector<Element> els = orderedSelection();
    if (els.isEmpty())
        return;
    const QRectF box = selectionBox(els);
    QDialog dlg(m_canvas->window());
    dlg.setWindowTitle(tr("Grid array"));
    auto *form = new QFormLayout;
    auto *cols = new QSpinBox(&dlg);
    cols->setRange(1, 500);
    cols->setValue(3);
    auto *rows = new QSpinBox(&dlg);
    rows->setRange(1, 500);
    rows->setValue(1);
    auto *gapX = mmBox(&dlg, -box.width(), 10000, 5.0);
    auto *gapY = mmBox(&dlg, -box.height(), 10000, 5.0);
    gapX->setToolTip(tr("Space between neighbouring copies, edge to edge"));
    gapY->setToolTip(gapX->toolTip());
    form->addRow(tr("Columns"), cols);
    form->addRow(tr("Rows"), rows);
    form->addRow(tr("Gap X"), gapX);
    form->addRow(tr("Gap Y"), gapY);
    auto *size = new QLabel(&dlg);
    auto updateSize = [=] {
        const double w = cols->value() * box.width() + (cols->value() - 1) * gapX->value();
        const double h = rows->value() * box.height() + (rows->value() - 1) * gapY->value();
        size->setText(tr("%1 × %2 mm overall").arg(w, 0, 'f', 2).arg(h, 0, 'f', 2));
    };
    for (QSpinBox *b : {cols, rows})
        connect(b, &QSpinBox::valueChanged, &dlg, updateSize);
    for (QDoubleSpinBox *b : {gapX, gapY})
        connect(b, &QDoubleSpinBox::valueChanged, &dlg, updateSize);
    updateSize();
    form->addRow(tr("Size"), size);
    QCheckBox *join = finishArrayDialog(dlg, form);
    if (dlg.exec() != QDialog::Accepted)
        return;
    if (cols->value() * rows->value() < 2)
        return;
    gridArray(cols->value(), rows->value(), gapX->value(), gapY->value(), join->isChecked());
}

void VectorActions::circularArrayDialog()
{
    const QVector<Element> els = orderedSelection();
    Document *d = m_canvas->document();
    if (els.isEmpty() || !d)
        return;
    const QRectF box = selectionBox(els);
    QDialog dlg(m_canvas->window());
    dlg.setWindowTitle(tr("Circular array"));
    auto *form = new QFormLayout;
    auto *count = new QSpinBox(&dlg);
    count->setRange(2, 1000);
    count->setValue(6);
    count->setToolTip(tr("Number of items including the original"));
    // Default center: the stock's, unless the selection sits on it, in
    // which case a ring around the selection's own center would collapse —
    // then a point one selection-height below it.
    QPointF c(d->boardWidth() / 2.0, d->boardHeight() / 2.0);
    if (d->boardWidth() <= 0 || QLineF(c, box.center()).length() < 1e-3)
        c = box.center() - QPointF(0, qMax(box.height(), 10.0) * 2);
    auto *cx = mmBox(&dlg, -100000, 100000, c.x());
    auto *cy = mmBox(&dlg, -100000, 100000, c.y());
    auto *span = new QDoubleSpinBox(&dlg);
    span->setRange(-360, 360);
    span->setDecimals(2);
    span->setSuffix(QStringLiteral(" °"));
    span->setValue(360);
    span->setToolTip(tr("360° spreads the items evenly around the circle; less places the first "
                        "and last at the ends of the arc. Positive is counter-clockwise."));
    auto *rotate = new QCheckBox(tr("Rotate the copies"), &dlg);
    rotate->setChecked(true);
    form->addRow(tr("Items"), count);
    form->addRow(tr("Center X"), cx);
    form->addRow(tr("Center Y"), cy);
    form->addRow(tr("Angle"), span);
    form->addRow(QString(), rotate);
    QCheckBox *join = finishArrayDialog(dlg, form);
    if (dlg.exec() != QDialog::Accepted)
        return;
    circularArray(QPointF(cx->value(), cy->value()), count->value(), span->value(),
                  rotate->isChecked(), join->isChecked());
}

} // namespace c2d
