#include "vectoractions.h"
#include "canvas.h"

#include <QAction>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
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
            els.removeAt(m_removed.at(i).first);
            at = m_removed.at(i).first;
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
    int closed = 0;
    for (const Element &e : inputs)
        closed += vec::isClosed(e) ? 1 : 0;
    if (closed < 2) {
        emit m_canvas->statusHint(tr("Booleans need at least two closed vectors"));
        return;
    }
    const QVector<Element> results = vec::booleanElements(inputs, op);
    const QString name = op == vec::BoolOp::Union ? tr("union")
                       : op == vec::BoolOp::Subtract ? tr("subtract") : tr("intersect");
    if (results.isEmpty()) {
        emit m_canvas->statusHint(tr("%1: empty result — vectors do not overlap").arg(name));
        return;
    }
    pushReplace(inputs, results, false, tr("%1 %2 vectors").arg(name).arg(inputs.size()));
    emit m_canvas->statusHint(tr("%1: %2 vector(s) → %3 closed path(s)")
                                  .arg(name).arg(inputs.size()).arg(results.size()));
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

} // namespace c2d
