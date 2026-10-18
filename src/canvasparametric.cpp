// Canvas: the parametric layer - constraint glyphs and driving dimensions
// drawn over the drawing, the Constrain and Dimension tools, the inline
// value editor, and the undo steps that re-solve the sketch after an edit.
#include "canvas.h"
#include <QContextMenuEvent>
#include <QFocusEvent>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QUndoCommand>
#include <QUndoStack>
#include <QtMath>
#include <cmath>
#include <functional>

namespace c2d {

// ---- palette -------------------------------------------------------------
static const QColor kDimLine(0x8f, 0xb8, 0xe8);      // dimension lines and frames
static const QColor kDimText(0xe8, 0xec, 0xf2);
static const QColor kExpr(0xc3, 0xa6, 0xff);         // "ƒx": driven by a parameter
static const QColor kGlyph(0x7c, 0xd6, 0xc2);        // constraint symbols
static const QColor kGlyphBg(0x22, 0x26, 0x2e);
static const QColor kGlyphEdge(0x3c, 0x44, 0x52);
static const QColor kFail(0xe8, 0x5c, 0x5c);
static const QColor kAmber(0xf2, 0xa5, 0x36);
static const QColor kPick(0x5c, 0xc8, 0x8a);         // picked / hovered geometry

using param::RefShape;

namespace {

struct ParamState {
    QVector<Element> els;
    QVector<Toolpath> tps;
    Parametric par;
};

ParamState stateOf(const Document *d)
{
    return {d->elements(), d->toolpaths(), d->parametric()};
}

void restore(Document *d, const ParamState &s)
{
    d->setElements(s.els);
    d->setToolpaths(s.tps);
    d->setParametric(s.par);
}

// The whole sketch before and after a parametric change: geometry the
// solver moved, toolpath fields re-evaluated, and the constraints.
class SnapshotCmd : public QUndoCommand
{
public:
    SnapshotCmd(Canvas *c, Document *d, const ParamState &before, const ParamState &after,
                const QString &text)
        : m_c(c), m_d(d), m_before(before), m_after(after) { setText(text); }
    void redo() override { apply(m_after); }
    void undo() override { apply(m_before); }

private:
    void apply(const ParamState &s)
    {
        restore(m_d, s);
        m_c->rebuild();
        emit m_c->documentChanged();
    }
    Canvas *m_c;
    Document *m_d;
    ParamState m_before, m_after;
};

// The inline dimension editor: Enter commits, Esc gives up (a new dimension
// keeps the measured value), leaving the field commits.
class DimEdit : public QLineEdit
{
public:
    using QLineEdit::QLineEdit;
    std::function<void(bool escape)> done;

protected:
    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Escape) {
            if (done) done(true);
            return;
        }
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
            if (done) done(false);
            return;
        }
        QLineEdit::keyPressEvent(e);
    }
    void focusOutEvent(QFocusEvent *e) override
    {
        QLineEdit::focusOutEvent(e);
        if (isVisible() && done)
            done(false);
    }
};

double segDist(QPointF p, QPointF a, QPointF b)
{
    const QPointF d = b - a;
    const double l2 = d.x() * d.x() + d.y() * d.y();
    if (l2 < 1e-18)
        return QLineF(p, a).length();
    const double t = qBound(0.0, ((p.x() - a.x()) * d.x() + (p.y() - a.y()) * d.y()) / l2, 1.0);
    return QLineF(p, a + d * t).length();
}

QPointF foot(QPointF p, QPointF a, QPointF b)
{
    const QPointF d = b - a;
    const double l2 = d.x() * d.x() + d.y() * d.y();
    if (l2 < 1e-18)
        return a;
    const double t = ((p.x() - a.x()) * d.x() + (p.y() - a.y()) * d.y()) / l2;
    return a + d * t;
}

QPointF unit(QPointF v)
{
    const double l = std::hypot(v.x(), v.y());
    return l < 1e-12 ? QPointF(1, 0) : v / l;
}

constexpr unsigned kP = 1u << GeoRef::Point, kL = 1u << GeoRef::Line, kC = 1u << GeoRef::Circle;

QString hintFor(Constraint::Type t, const QVector<RefShape> &picks)
{
    using T = Constraint;
    const int n = picks.size();
    const bool firstLine = n > 0 && picks.first().ref.kind == GeoRef::Line;
    switch (t) {
    case T::Coincident:
        return n == 0 ? QObject::tr("Coincident — click a point, then the point, line or circle it should sit on")
                      : QObject::tr("Coincident — now the point, line or circle");
    case T::Horizontal:
    case T::Vertical:
        return n == 0 ? QObject::tr("%1 — click a line, or two points").arg(T::typeName(t))
                      : QObject::tr("%1 — now the second point").arg(T::typeName(t));
    case T::Parallel:
    case T::Perpendicular:
    case T::Collinear:
        return n == 0 ? QObject::tr("%1 — click the first line").arg(T::typeName(t))
                      : QObject::tr("%1 — now the second line").arg(T::typeName(t));
    case T::Tangent:
        return n == 0 ? QObject::tr("Tangent — click a line or a circle")
                      : QObject::tr("Tangent — now the circle");
    case T::Equal:
        return n == 0 ? QObject::tr("Equal — click a line or a circle")
                      : QObject::tr("Equal — now the %1 to match").arg(firstLine ? QObject::tr("line") : QObject::tr("circle"));
    case T::Concentric:
        return n == 0 ? QObject::tr("Concentric — click the first circle")
                      : QObject::tr("Concentric — now the second circle");
    case T::Midpoint:
        return n == 0 ? QObject::tr("Midpoint — click a point and a line")
                      : QObject::tr("Midpoint — now the %1").arg(firstLine ? QObject::tr("point") : QObject::tr("line"));
    case T::Fix:
        return QObject::tr("Fix — click a point to pin it, or a line or circle to lock the whole shape");
    case T::Symmetric:
        return n == 0 ? QObject::tr("Symmetric — click the first point")
               : n == 1 ? QObject::tr("Symmetric — now the second point")
                        : QObject::tr("Symmetric — now the line of symmetry");
    default:
        return QString();
    }
}

} // namespace

struct Canvas::DimDraw {
    QPointF mid;                            // what the label offset is measured from
    QPointF text;                           // label position (scene mm)
    QVector<QLineF> ext;                    // extension lines
    QVector<QLineF> dim;                    // dimension lines
    QVector<QPair<QPointF, QPointF>> arrows;   // tip, unit direction it points to
    QPainterPath arc;                       // angle dimensions
    QString label;
    bool expr = false;                      // driven by a parameter
};

// ---- state -----------------------------------------------------------------------

void Canvas::refreshParametricState()
{
    m_shapeCache.clear();
    m_full.clear();
    m_dof = 0;
    m_failing.clear();
    if (!m_doc || m_doc->parametric().constraints.isEmpty()) {
        if (!m_selConstraint.isEmpty())
            selectConstraint(QString());
        return;
    }
    const param::Freedom f = param::freedom(*m_doc);
    m_full = f.fullyConstrained;
    m_dof = f.dof;
    m_failing = param::unsatisfied(*m_doc);
    if (!m_selConstraint.isEmpty() && m_doc->parametric().indexOfUid(m_selConstraint) < 0)
        selectConstraint(QString());
}

void Canvas::setConstraintType(Constraint::Type t)
{
    m_conType = t;
    if (m_tool == Constrain)
        resetPicks();
}

void Canvas::setShowConstraints(bool on)
{
    m_showConstraints = on;
    viewport()->update();
}

void Canvas::setParametricVisible(bool on)
{
    m_parametricVisible = on;
    if (!on)
        closeDimEditor();
    viewport()->update();
}

void Canvas::selectConstraint(const QString &uid)
{
    if (m_selConstraint == uid)
        return;
    m_selConstraint = uid;
    emit constraintSelected(uid);
    viewport()->update();
}

void Canvas::resetPicks()
{
    m_picks.clear();
    m_dimPlacing = false;
    m_hasHover = false;
    if (m_tool == Constrain)
        emit statusHint(hintFor(m_conType, m_picks));
    else if (m_tool == Dimension)
        emit statusHint(tr("Dimension — click a line for its length, two points or a point and a line "
                           "for a distance, two lines for an angle, a circle for its diameter (Shift: radius)"));
    viewport()->update();
}

// ---- undo steps --------------------------------------------------------------------

void Canvas::pushEdit(QUndoCommand *cmd, const QStringList &touched)
{
    if (!m_doc || m_doc->parametric().constraints.isEmpty()) {
        m_undo->push(cmd);
        return;
    }
    const QString text = cmd->text();
    const Document before = *m_doc;     // cheap: implicitly shared
    m_undo->beginMacro(text);
    m_undo->push(cmd);
    const ParamState s0 = stateOf(m_doc);
    // Constraints follow their geometry through node deletes and inserts,
    // fillets and conversions; those on geometry that is gone go too.
    QStringList dropped;
    bool renumbered = false;
    param::reconcile(before, *m_doc, &dropped, &renumbered);
    if (!dropped.isEmpty() && dropped.contains(m_selConstraint))
        selectConstraint(QString());
    const param::SolveResult r = param::solve(*m_doc, QSet<QString>(touched.begin(), touched.end()));
    if (!r.changed.isEmpty() || renumbered)
        m_undo->push(new SnapshotCmd(this, m_doc, s0, stateOf(m_doc), text));
    m_undo->endMacro();
    if (!r.ok) {
        rebuild();
        emit statusHint(r.failing.size() == 1
            ? tr("1 constraint can't hold after this edit — shown in red")
            : tr("%1 constraints can't hold after this edit — shown in red").arg(r.failing.size()));
    } else if (!dropped.isEmpty()) {
        emit statusHint(dropped.size() == 1
            ? tr("1 constraint went with the geometry it was on")
            : tr("%1 constraints went with the geometry they were on").arg(dropped.size()));
    }
}

bool Canvas::applyParametric(const Parametric &after, const QString &text, bool allowFailing,
                             const QSet<QString> &keep,
                             const std::function<void(Document &)> &mutate)
{
    if (!m_doc)
        return false;
    const ParamState s0 = stateOf(m_doc);
    m_doc->setParametric(after);
    if (mutate)
        mutate(*m_doc);
    param::SolveResult r = param::solve(*m_doc, keep, allowFailing);
    if (!r.ok && !allowFailing) {
        // Constraints that were red before don't block an unrelated edit;
        // only one that makes something else fail is refused.
        Document was = *m_doc;
        was.setElements(s0.els);
        was.setParametric(s0.par);
        const QStringList red = param::unsatisfied(was);
        bool news = false;
        for (const QString &uid : r.failing)
            news = news || !red.contains(uid);
        if (news) {
            restore(m_doc, s0);
            return false;
        }
        r = param::solve(*m_doc, keep, true);
    }
    param::applyToolpathBindings(*m_doc);
    const ParamState s1 = stateOf(m_doc);
    restore(m_doc, s0);         // the command's redo puts s1 back
    m_undo->push(new SnapshotCmd(this, m_doc, s0, s1, text));
    if (!r.ok)
        emit statusHint(r.failing.size() == 1
            ? tr("1 constraint can't hold with these values — shown in red")
            : tr("%1 constraints can't hold with these values — shown in red").arg(r.failing.size()));
    return true;
}

void Canvas::deleteConstraint(const QString &uid)
{
    if (!m_doc)
        return;
    Parametric after = m_doc->parametric();
    const int i = after.indexOfUid(uid);
    if (i < 0)
        return;
    const QString what = after.constraints.at(i).isDimension()
        ? tr("delete dimension %1").arg(after.constraints.at(i).name)
        : tr("delete %1 constraint").arg(Constraint::typeName(after.constraints.at(i).type).toLower());
    const Constraint gone = after.constraints.takeAt(i);
    // Expressions that use the dimension keep its value as a parameter.
    if (gone.isDimension() && param::usedNames(*m_doc, after).contains(gone.name))
        after.params.append({gone.name, gone.expr.trimmed().isEmpty() ? param::formatNumber(gone.value) : gone.expr,
                             tr("was dimension %1").arg(gone.name)});
    selectConstraint(QString());
    applyParametric(after, what, true);
    emit statusHint(what);
}

// ---- geometry lookups ------------------------------------------------------------------

bool Canvas::refShape(const GeoRef &r, RefShape *out) const
{
    if (!m_doc)
        return false;
    if (r.isOrigin()) {
        out->ref = r;
        out->a = out->b = QPointF(0, 0);
        out->r = 0;
        return true;
    }
    auto it = m_shapeCache.find(r.id);
    if (it == m_shapeCache.end())
        it = m_shapeCache.insert(r.id, param::shapesOf(*m_doc, r.id));
    for (const RefShape &s : it.value())
        if (s.ref == r) {
            *out = s;
            return true;
        }
    // A circle asked for as a point is its center.
    if (r.kind == GeoRef::Point)
        for (const RefShape &s : it.value())
            if (s.ref.kind == GeoRef::Circle) {
                *out = s;
                out->ref = r;
                return true;
            }
    return false;
}

bool Canvas::pickGeo(const QPointF &pos, unsigned mask, RefShape *out) const
{
    if (!m_doc || !mask)
        return false;
    const double tolP = pxToMm(9), tolL = pxToMm(7);
    double best = 1e300;
    bool hit = false;
    auto consider = [&](const RefShape &s, double d) {
        // Points win ties: a corner is where people aim.
        const double score = s.ref.kind == GeoRef::Point ? d - tolP : d;
        if (score < best) {
            best = score;
            *out = s;
            hit = true;
        }
    };
    if ((mask & kP) && QLineF(pos, QPointF(0, 0)).length() <= tolP) {
        RefShape o;
        o.ref = GeoRef::origin();
        consider(o, QLineF(pos, QPointF(0, 0)).length());
    }
    const QRectF near(pos - QPointF(tolP, tolP), QSizeF(2 * tolP, 2 * tolP));
    QSet<QString> seen;
    for (QGraphicsItem *it : m_scene->items(near, Qt::IntersectsItemBoundingRect)) {
        if (!it->data(0).isValid())
            continue;
        const QString id = it->data(0).toString();
        if (seen.contains(id))
            continue;
        seen.insert(id);
        auto cached = m_shapeCache.find(id);
        if (cached == m_shapeCache.end())
            cached = m_shapeCache.insert(id, param::shapesOf(*m_doc, id));
        for (const RefShape &s : cached.value()) {
            if (!(mask & (1u << s.ref.kind)))
                continue;
            double d = 1e300;
            if (s.ref.kind == GeoRef::Point) {
                d = QLineF(pos, s.a).length();
                if (d > tolP) continue;
            } else if (s.ref.kind == GeoRef::Line) {
                d = segDist(pos, s.a, s.b);
                if (d > tolL) continue;
            } else {
                d = std::fabs(QLineF(pos, s.a).length() - s.r);
                if (d > tolL) continue;
            }
            consider(s, d);
        }
    }
    return hit;
}

unsigned Canvas::pickMask() const
{
    using T = Constraint;
    const int n = m_picks.size();
    const GeoRef::Kind k0 = n > 0 ? m_picks.first().ref.kind : GeoRef::Point;
    if (m_tool == Dimension) {
        if (m_dimPlacing)
            return 0;
        if (n == 0)
            return kP | kL | kC;
        return k0 == GeoRef::Line ? (kP | kL) : (kP | kL | kC);
    }
    if (m_tool != Constrain)
        return 0;
    switch (m_conType) {
    case T::Coincident:
        return n == 0 || k0 == GeoRef::Point ? (kP | kL | kC) : kP;
    case T::Horizontal:
    case T::Vertical:
        return n == 0 ? (kP | kL) : kP;
    case T::Parallel:
    case T::Perpendicular:
    case T::Collinear:
        return kL;
    case T::Tangent:
        return n == 0 ? (kL | kC) : (k0 == GeoRef::Line ? kC : (kL | kC));
    case T::Equal:
        return n == 0 ? (kL | kC) : (1u << k0);
    case T::Concentric:
        return kC;
    case T::Midpoint:
        return n == 0 ? (kP | kL) : (k0 == GeoRef::Line ? kP : kL);
    case T::Fix:
        return kP | kL | kC;
    case T::Symmetric:
        return n < 2 ? kP : kL;
    default:
        return 0;
    }
}

// ---- the Constrain tool -----------------------------------------------------------------

void Canvas::constrainClick()
{
    using T = Constraint;
    if (!m_doc)
        return;
    if (!m_hasHover) {
        emit statusHint(hintFor(m_conType, m_picks) + tr("  (nothing to pick there)"));
        return;
    }
    for (const RefShape &p : m_picks)
        if (p.ref == m_hover.ref) {
            emit statusHint(tr("Already picked — pick something else"));
            return;
        }
    m_picks.append(m_hover);
    const int n = m_picks.size();
    auto ref = [&](int i) { return m_picks.at(i).ref; };
    auto isK = [&](int i, GeoRef::Kind k) { return m_picks.at(i).ref.kind == k; };

    QVector<GeoRef> refs;
    switch (m_conType) {
    case T::Coincident:
        if (n == 2) {
            if (isK(0, GeoRef::Point)) refs = {ref(0), ref(1)};
            else if (isK(1, GeoRef::Point)) refs = {ref(1), ref(0)};
        }
        break;
    case T::Horizontal:
    case T::Vertical:
        if (n == 1 && isK(0, GeoRef::Line)) refs = {ref(0)};
        else if (n == 2) refs = {ref(0), ref(1)};
        break;
    case T::Parallel:
    case T::Perpendicular:
    case T::Collinear:
    case T::Concentric:
        if (n == 2) refs = {ref(0), ref(1)};
        break;
    case T::Tangent:
        if (n == 2) refs = isK(1, GeoRef::Line) ? QVector<GeoRef>{ref(1), ref(0)}
                                               : QVector<GeoRef>{ref(0), ref(1)};
        break;
    case T::Equal:
        if (n == 2) refs = {ref(0), ref(1)};
        break;
    case T::Midpoint:
        if (n == 2) refs = isK(0, GeoRef::Point) ? QVector<GeoRef>{ref(0), ref(1)}
                                                : QVector<GeoRef>{ref(1), ref(0)};
        break;
    case T::Fix:
        refs = {ref(0)};
        break;
    case T::Symmetric:
        if (n == 3) refs = {ref(0), ref(1), ref(2)};
        break;
    default:
        break;
    }
    if (refs.isEmpty()) {
        emit statusHint(hintFor(m_conType, m_picks));
        viewport()->update();
        return;
    }
    m_picks.clear();
    Constraint c;
    c.type = m_conType;
    c.refs = refs;
    for (const Constraint &o : m_doc->parametric().constraints)
        if (o.type == c.type && o.refs == c.refs) {
            emit statusHint(tr("That %1 constraint is already there").arg(T::typeName(c.type).toLower()));
            viewport()->update();
            return;
        }
    Parametric after = m_doc->parametric();
    c.uid = after.nextUid();
    param::prime(*m_doc, c);
    after.constraints.append(c);
    if (applyParametric(after, tr("add %1 constraint").arg(T::typeName(c.type).toLower())))
        emit statusHint(tr("%1 added — %2").arg(T::typeName(c.type), hintFor(m_conType, m_picks)));
    else
        emit statusHint(tr("%1 can't hold together with the constraints already there — not added")
                            .arg(T::typeName(c.type)));
    viewport()->update();
}

// ---- the Dimension tool --------------------------------------------------------------------

void Canvas::dimensionClick(const QPointF &pos, Qt::KeyboardModifiers mods)
{
    using T = Constraint;
    if (!m_doc)
        return;
    if (m_dimPlacing) {
        updateDimDraft(pos);
        m_dimPlacing = false;
        m_picks.clear();
        Constraint c = m_dimDraft;
        c.uid = m_doc->parametric().nextUid();
        c.name = m_doc->parametric().nextDimensionName();
        c.expr.clear();
        param::prime(*m_doc, c);
        openDimEditor(c, true);
        return;
    }
    auto startPlacing = [&](T::Type t, const QVector<GeoRef> &refs) {
        m_dimDraft = Constraint();
        m_dimDraft.type = t;
        m_dimDraft.refs = refs;
        param::prime(*m_doc, m_dimDraft);
        m_dimPlacing = true;
        updateDimDraft(pos);
        emit statusHint(tr("Click where the dimension goes, then type its value or an expression"));
        viewport()->update();
    };
    if (!m_hasHover) {
        // A single line, then a click in the open: its length, placed there.
        if (m_picks.size() == 1 && m_picks.first().ref.kind == GeoRef::Line) {
            startPlacing(T::Distance, {m_picks.first().ref});
            dimensionClick(pos, mods);
        }
        return;
    }
    const RefShape h = m_hover;
    if (m_picks.isEmpty()) {
        if (h.ref.kind == GeoRef::Circle) {
            startPlacing((mods & Qt::ShiftModifier) ? T::Radius : T::Diameter, {h.ref});
            return;
        }
        m_picks.append(h);
        emit statusHint(h.ref.kind == GeoRef::Line
            ? tr("Line picked — click empty space to place its length, or a second line (angle) or a point (distance)")
            : tr("Point picked — click a second point or a line"));
        viewport()->update();
        return;
    }
    const RefShape first = m_picks.first();
    if (first.ref == h.ref)
        return;
    GeoRef second = h.ref;
    if (second.kind == GeoRef::Circle)
        second.kind = GeoRef::Point;            // its center
    if (first.ref.kind == GeoRef::Line && second.kind == GeoRef::Line) {
        const QPointF u = unit(first.b - first.a), v = unit(h.b - h.a);
        const bool parallel = std::fabs(u.x() * v.y() - u.y() * v.x()) < 0.01;
        startPlacing(parallel ? T::Distance : T::Angle, {first.ref, second});
    } else if (first.ref.kind == GeoRef::Line) {
        startPlacing(T::Distance, {second, first.ref});
    } else if (second.kind == GeoRef::Line) {
        startPlacing(T::Distance, {first.ref, second});
    } else {
        startPlacing(T::Distance, {first.ref, second});
    }
}

void Canvas::updateDimDraft(const QPointF &pos)
{
    using T = Constraint;
    if (!m_doc)
        return;
    // Two points (or one line): the cursor picks aligned, horizontal or
    // vertical, the way Fusion does - above or below the pair is horizontal,
    // beside it vertical, anywhere else along the pair aligned.
    const bool pair = (m_dimDraft.refs.size() == 2 && m_dimDraft.refs[0].kind == GeoRef::Point
                       && m_dimDraft.refs[1].kind == GeoRef::Point)
                      || (m_dimDraft.refs.size() == 1 && m_dimDraft.refs[0].kind == GeoRef::Line);
    if (pair) {
        RefShape s0, s1;
        QPointF p, q;
        if (m_dimDraft.refs.size() == 1) {
            if (!refShape(m_dimDraft.refs[0], &s0)) return;
            p = s0.a; q = s0.b;
        } else {
            if (!refShape(m_dimDraft.refs[0], &s0) || !refShape(m_dimDraft.refs[1], &s1)) return;
            p = s0.a; q = s1.a;
        }
        const QRectF box = QRectF(p, q).normalized();
        const bool inX = pos.x() >= box.left() && pos.x() <= box.right();
        const bool inY = pos.y() >= box.top() && pos.y() <= box.bottom();
        T::Type t = T::Distance;
        if (box.width() > 1e-6 && box.height() > 1e-6) {
            if (inX && !inY) t = T::HDistance;
            else if (inY && !inX) t = T::VDistance;
        }
        if (t != m_dimDraft.type) {
            m_dimDraft.type = t;
            param::prime(*m_doc, m_dimDraft);
        }
    }
    m_dimDraft.label = QPointF();
    DimDraw d;
    if (dimGeometry(m_dimDraft, &d))
        m_dimDraft.label = pos - d.mid;
    viewport()->update();
}

// ---- dimension geometry --------------------------------------------------------------------

bool Canvas::dimGeometry(const Constraint &c, DimDraw *d) const
{
    using T = Constraint;
    RefShape s[3];
    for (int i = 0; i < c.refs.size() && i < 3; ++i)
        if (!refShape(c.refs[i], &s[i]))
            return false;
    const double v = c.value;
    const bool fx = !param::namesIn(c.expr).isEmpty();
    d->expr = fx;
    const double ext = pxToMm(6), gap = pxToMm(3);

    auto linear = [&](QPointF p, QPointF q, T::Type t) {
        const QPointF mid = (p + q) / 2;
        d->mid = mid;
        QPointF p2, q2, along;
        if (t == T::HDistance || t == T::VDistance) {
            const bool h = t == T::HDistance;
            if (h) {
                const double y = mid.y() + c.label.y();
                p2 = QPointF(p.x(), y); q2 = QPointF(q.x(), y);
                d->text = QPointF(mid.x() + c.label.x(), y);
            } else {
                const double x = mid.x() + c.label.x();
                p2 = QPointF(x, p.y()); q2 = QPointF(x, q.y());
                d->text = QPointF(x, mid.y() + c.label.y());
            }
            for (const auto &pr : {qMakePair(p, p2), qMakePair(q, q2)}) {
                const QPointF dir = unit(pr.second - pr.first);
                if (QLineF(pr.first, pr.second).length() > gap)
                    d->ext << QLineF(pr.first + dir * gap, pr.second + dir * ext);
            }
        } else {
            const QPointF u = unit(q - p), n(-u.y(), u.x());
            const double off = QPointF::dotProduct(c.label, n);
            const double al = QPointF::dotProduct(c.label, u);
            p2 = p + n * off; q2 = q + n * off;
            d->text = (p2 + q2) / 2 + u * al;
            if (std::fabs(off) > gap) {
                const QPointF sn = n * (off < 0 ? -1 : 1);
                d->ext << QLineF(p + sn * gap, p2 + sn * ext) << QLineF(q + sn * gap, q2 + sn * ext);
            }
        }
        d->dim << QLineF(p2, q2);
        // The text sits beyond the arrows: carry the line out to it.
        const QPointF dl = unit(q2 - p2);
        const double tp = QPointF::dotProduct(d->text - p2, dl), span = QLineF(p2, q2).length();
        if (tp < 0) d->dim << QLineF(p2, p2 + dl * tp);
        if (tp > span) d->dim << QLineF(q2, p2 + dl * tp);
        d->arrows << qMakePair(p2, unit(p2 - q2)) << qMakePair(q2, unit(q2 - p2));
        d->label = QString::number(v, 'f', 2);
    };

    switch (c.type) {
    case T::Distance:
    case T::HDistance:
    case T::VDistance:
        if (c.refs.size() == 1) {
            linear(s[0].a, s[0].b, c.type);
        } else if (c.refs[0].kind == GeoRef::Point && c.refs[1].kind == GeoRef::Point) {
            linear(s[0].a, s[1].a, c.type);
        } else if (c.refs[0].kind == GeoRef::Point) {
            linear(s[0].a, foot(s[0].a, s[1].a, s[1].b), T::Distance);
        } else {
            const QPointF m = (s[1].a + s[1].b) / 2;
            linear(foot(m, s[0].a, s[0].b), m, T::Distance);
        }
        break;
    case T::Radius:
    case T::Diameter: {
        const QPointF ctr = s[0].a;
        const double r = std::fabs(s[0].r);
        d->mid = ctr;
        d->text = ctr + c.label;
        const QPointF u = c.label.isNull() ? QPointF(M_SQRT1_2, M_SQRT1_2) : unit(c.label);
        const QPointF tip = ctr + u * r;
        if (c.type == T::Radius) {
            d->dim << QLineF(ctr, QLineF(ctr, d->text).length() > r ? d->text : tip);
            d->arrows << qMakePair(tip, u);
            d->label = QStringLiteral("R%1").arg(v, 0, 'f', 2);
        } else {
            const QPointF back = ctr - u * r;
            d->dim << QLineF(back, QLineF(ctr, d->text).length() > r ? d->text : tip);
            d->arrows << qMakePair(tip, u) << qMakePair(back, -u);
            d->label = QStringLiteral("⌀%1").arg(v, 0, 'f', 2);
        }
        break;
    }
    case T::Angle: {
        const QPointF a1 = s[0].a, b1 = s[0].b, a2 = s[1].a, b2 = s[1].b;
        const QPointF r1 = b1 - a1, r2 = b2 - a2;
        const double den = r1.x() * r2.y() - r1.y() * r2.x();
        QPointF x = a1;
        if (std::fabs(den) > 1e-12) {
            const double t = ((a2.x() - a1.x()) * r2.y() - (a2.y() - a1.y()) * r2.x()) / den;
            x = a1 + r1 * t;
        }
        d->mid = x;
        d->text = x + c.label;
        // Each ray points from the vertex to the far end of its line.
        auto rayOf = [&](QPointF a, QPointF b) {
            return unit(QLineF(x, a).length() > QLineF(x, b).length() ? a - x : b - x);
        };
        const QPointF u1 = rayOf(a1, b1), u2 = rayOf(a2, b2);
        const double rad = qMax(pxToMm(24), QLineF(x, d->text).length());
        double t1 = std::atan2(u1.y(), u1.x()), t2 = std::atan2(u2.y(), u2.x());
        double sweep = t2 - t1;
        while (sweep > M_PI) sweep -= 2 * M_PI;
        while (sweep < -M_PI) sweep += 2 * M_PI;
        QPainterPath arc;
        const int steps = 32;
        for (int i = 0; i <= steps; ++i) {
            const double t = t1 + sweep * i / steps;
            const QPointF pt = x + QPointF(std::cos(t), std::sin(t)) * rad;
            if (i == 0) arc.moveTo(pt); else arc.lineTo(pt);
        }
        d->arc = arc;
        const QPointF e1 = x + u1 * rad, e2 = x + u2 * rad;
        // Extension along each line when the arc runs past its end.
        for (const auto &pr : {qMakePair(e1, QLineF(a1, b1)), qMakePair(e2, QLineF(a2, b2))}) {
            const double reach = qMax(QLineF(x, pr.second.p1()).length(), QLineF(x, pr.second.p2()).length());
            if (rad > reach + gap)
                d->ext << QLineF(x + unit(pr.first - x) * (reach + gap), pr.first + unit(pr.first - x) * ext);
        }
        const double s = sweep < 0 ? -1 : 1;
        d->arrows << qMakePair(e1, QPointF(std::sin(t1), -std::cos(t1)) * s)
                  << qMakePair(e2, QPointF(-std::sin(t2), std::cos(t2)) * s);
        d->label = QStringLiteral("%1°").arg(v, 0, 'f', 1);
        break;
    }
    default:
        return false;
    }
    return true;
}

// ---- drawing ---------------------------------------------------------------------------------

static void drawGlyph(QPainter *p, const QRectF &r, Constraint::Type t, const QColor &ink)
{
    using T = Constraint;
    p->save();
    QPen pen(ink, 1.4);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p->setPen(pen);
    p->setBrush(Qt::NoBrush);
    const QPointF c = r.center();
    const double k = r.width() / 16.0;
    auto P = [&](double x, double y) { return c + QPointF(x * k, y * k); };
    switch (t) {
    case T::Horizontal:
    case T::Vertical: {
        // The line with a dot at each end, so it never reads as a minus.
        const bool h = t == T::Horizontal;
        const QPointF a = h ? P(-4.5, 0) : P(0, -4.5), b = h ? P(4.5, 0) : P(0, 4.5);
        p->drawLine(a, b);
        p->setBrush(ink);
        p->drawEllipse(a, 1.3 * k, 1.3 * k);
        p->drawEllipse(b, 1.3 * k, 1.3 * k);
        break;
    }
    case T::Parallel:
        p->drawLine(P(-4, 4), P(0, -4));
        p->drawLine(P(0, 4), P(4, -4));
        break;
    case T::Perpendicular:
        p->drawLine(P(-5, 4), P(5, 4));
        p->drawLine(P(0, 4), P(0, -5));
        break;
    case T::Tangent:
        p->drawEllipse(P(0, 1.5), 3.5 * k, 3.5 * k);
        p->drawLine(P(-5, -2.5), P(5, -2.5));
        break;
    case T::Equal:
        p->drawLine(P(-4, -2), P(4, -2));
        p->drawLine(P(-4, 2), P(4, 2));
        break;
    case T::Concentric:
        p->drawEllipse(c, 5 * k, 5 * k);
        p->drawEllipse(c, 2 * k, 2 * k);
        break;
    case T::Midpoint:
        p->drawPolygon(QPolygonF({P(0, -4.5), P(4.5, 3.5), P(-4.5, 3.5)}));
        break;
    case T::Collinear:
        p->drawLine(P(-5, 3), P(-1.5, 0.9));
        p->drawLine(P(1.5, -0.9), P(5, -3));
        p->setBrush(ink);
        p->drawEllipse(c, 1.1 * k, 1.1 * k);
        break;
    case T::Fix: {
        // A padlock.
        p->drawArc(QRectF(P(-3, -6), P(3, 0)), 0, 180 * 16);
        p->setBrush(ink);
        p->setPen(Qt::NoPen);
        p->drawRoundedRect(QRectF(P(-4.5, -2.5), P(4.5, 5)), 1.2 * k, 1.2 * k);
        break;
    }
    case T::Symmetric:
        p->drawLine(P(0, -5.5), P(0, 5.5));
        p->drawPolyline(QPolygonF({P(-2.5, -3.5), P(-5, 0), P(-2.5, 3.5)}));
        p->drawPolyline(QPolygonF({P(2.5, -3.5), P(5, 0), P(2.5, 3.5)}));
        break;
    case T::Coincident:
        // Two lines ending on one shared point.
        p->drawLine(P(-5, 4), c);
        p->drawLine(c, P(5, 2.5));
        p->setBrush(ink);
        p->drawEllipse(c, 2.2 * k, 2.2 * k);
        break;
    default:
        break;
    }
    p->restore();
}

QPixmap constraintPixmap(Constraint::Type t, const QColor &ink, int size, qreal dpr)
{
    QPixmap pm(int(size * dpr), int(size * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r(0, 0, size, size);
    if (t >= Constraint::Distance) {
        // A dimension: extension lines, the dimension line with its arrows.
        const double k = size / 20.0;
        QPen pen(ink, 1.4 * k);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.drawLine(QPointF(3 * k, 16 * k), QPointF(17 * k, 16 * k));
        p.drawLine(QPointF(3 * k, 14 * k), QPointF(3 * k, 4 * k));
        p.drawLine(QPointF(17 * k, 14 * k), QPointF(17 * k, 4 * k));
        p.drawLine(QPointF(4 * k, 7 * k), QPointF(16 * k, 7 * k));
        p.setPen(Qt::NoPen);
        p.setBrush(ink);
        p.drawPolygon(QPolygonF({QPointF(3.6 * k, 7 * k), QPointF(7.5 * k, 5 * k), QPointF(7.5 * k, 9 * k)}));
        p.drawPolygon(QPolygonF({QPointF(16.4 * k, 7 * k), QPointF(12.5 * k, 5 * k), QPointF(12.5 * k, 9 * k)}));
        return pm;
    }
    drawGlyph(&p, r.adjusted(size * 0.05, size * 0.05, -size * 0.05, -size * 0.05), t, ink);
    return pm;
}

static void drawArrow(QPainter *p, QPointF tip, QPointF dir, const QColor &c)
{
    const QPointF u = unit(dir), n(-u.y(), u.x());
    const double len = 8, half = 3;
    p->save();
    p->setPen(Qt::NoPen);
    p->setBrush(c);
    p->drawPolygon(QPolygonF({tip, tip - u * len + n * half, tip - u * len - n * half}));
    p->restore();
}

void Canvas::drawParametric(QPainter *p)
{
    m_hits.clear();
    if (!m_doc)
        return;
    const QTransform vt = viewportTransform();
    auto V = [&](QPointF s) { return vt.map(s); };
    // Directions map with the view (its Y flip included).
    auto Vdir = [&](QPointF at, QPointF dir) { return unit(V(at + dir * pxToMm(10)) - V(at)); };

    p->save();
    p->resetTransform();
    p->setRenderHint(QPainter::Antialiasing, true);

    // Geometry to light up: the picks, what is under the cursor, and the
    // geometry of the hovered / selected constraint.
    auto lightUp = [&](const RefShape &s, const QColor &col, double width) {
        QPen pen(col, width);
        pen.setCapStyle(Qt::RoundCap);
        p->setPen(pen);
        p->setBrush(Qt::NoBrush);
        if (s.ref.kind == GeoRef::Point) {
            p->setBrush(col);
            p->drawEllipse(V(s.a), width + 2, width + 2);
        } else if (s.ref.kind == GeoRef::Line) {
            p->drawLine(V(s.a), V(s.b));
        } else {
            const QPointF c = V(s.a);
            const double r = std::fabs(V(s.a + QPointF(s.r, 0)).x() - c.x());
            p->drawEllipse(c, r, r);
        }
    };
    const bool tools = m_tool == Constrain || m_tool == Dimension;
    for (const RefShape &s : m_picks)
        lightUp(s, kAmber, 3);
    if (tools && m_hasHover)
        lightUp(m_hover, kPick, 3);
    const Parametric &par = m_doc->parametric();
    if (overlayOn()) {
        for (const QString &uid : {m_hoverConstraint, m_selConstraint}) {
            const int i = par.indexOfUid(uid);
            if (i < 0)
                continue;
            for (const GeoRef &r : par.constraints.at(i).refs) {
                RefShape s;
                if (refShape(r, &s))
                    lightUp(s, QColor(kAmber.red(), kAmber.green(), kAmber.blue(), 170), 2.5);
            }
        }
    }

    QFont f = font();
    f.setPixelSize(12);
    f.setWeight(QFont::DemiBold);
    p->setFont(f);
    const QFontMetricsF fm(f);

    auto drawDim = [&](const Constraint &c, const DimDraw &d, const QColor &line, bool hilite) {
        QPen pen(line, 1.0);
        p->setPen(pen);
        p->setBrush(Qt::NoBrush);
        for (const QLineF &l : d.ext)
            p->drawLine(V(l.p1()), V(l.p2()));
        pen.setWidthF(1.2);
        p->setPen(pen);
        for (const QLineF &l : d.dim)
            p->drawLine(V(l.p1()), V(l.p2()));
        if (!d.arc.isEmpty())
            p->drawPath(vt.map(d.arc));
        for (const auto &a : d.arrows)
            drawArrow(p, V(a.first), Vdir(a.first, a.second), line);
        // The value on a dark pill; ƒx in violet when a parameter drives it.
        const QString fxs = d.expr ? QStringLiteral("ƒx ") : QString();
        const double wFx = fm.horizontalAdvance(fxs), wTx = fm.horizontalAdvance(d.label);
        const QPointF at = V(d.text);
        const QRectF pill(at.x() - (wFx + wTx) / 2 - 7, at.y() - fm.height() / 2 - 3,
                          wFx + wTx + 14, fm.height() + 6);
        p->setPen(QPen(line, hilite ? 1.6 : 1.0));
        p->setBrush(QColor(0x17, 0x19, 0x1e, 235));
        p->drawRoundedRect(pill, pill.height() / 2, pill.height() / 2);
        const double base = pill.center().y() + (fm.ascent() - fm.descent()) / 2;
        if (!fxs.isEmpty()) {
            p->setPen(kExpr);
            p->drawText(QPointF(pill.left() + 7, base), fxs);
        }
        p->setPen(kDimText);
        p->drawText(QPointF(pill.left() + 7 + wFx, base), d.label);
        if (!c.uid.isEmpty())
            m_hits.append({pill.adjusted(-2, -2, 2, 2), c.uid});
    };

    if (overlayOn()) {
        // Dimensions.
        for (const Constraint &c : par.constraints) {
            if (!c.isDimension())
                continue;
            DimDraw d;
            if (!dimGeometry(c, &d))
                continue;
            const bool sel = c.uid == m_selConstraint, hov = c.uid == m_hoverConstraint;
            const QColor col = m_failing.contains(c.uid) ? kFail : (sel || hov) ? kAmber : kDimLine;
            drawDim(c, d, col, sel || hov);
        }

        // Constraint glyphs: a small badge beside each piece of geometry a
        // constraint holds; badges on the same spot line up side by side.
        QHash<quint64, int> stack;
        for (const Constraint &c : par.constraints) {
            if (c.isDimension())
                continue;
            const bool sel = c.uid == m_selConstraint, hov = c.uid == m_hoverConstraint;
            const QColor ink = m_failing.contains(c.uid) ? kFail : (sel || hov) ? kAmber : kGlyph;
            const bool pointPair = c.type == Constraint::Coincident && c.refs.size() == 2
                                   && c.refs[1].kind == GeoRef::Point;
            for (int i = 0; i < c.refs.size(); ++i) {
                const GeoRef &r = c.refs.at(i);
                if (r.isOrigin())
                    continue;
                RefShape s;
                if (!refShape(r, &s))
                    continue;
                QPointF anchor = s.a;
                if (r.kind == GeoRef::Line)
                    anchor = (s.a + s.b) / 2;
                else if (r.kind == GeoRef::Circle)
                    anchor = s.a + QPointF(M_SQRT1_2, M_SQRT1_2) * std::fabs(s.r);
                const QPointF pa = V(anchor);
                if (pointPair) {
                    // Coincident points: one dot on the joint, no badge.
                    if (i > 0)
                        continue;
                    p->setPen(QPen(kGlyphBg, 1.5));
                    p->setBrush(ink);
                    p->drawEllipse(pa, 4, 4);
                    m_hits.append({QRectF(pa - QPointF(6, 6), QSizeF(12, 12)), c.uid});
                    continue;
                }
                const quint64 key = (quint64(quint32(qRound(pa.x() / 6))) << 32)
                                    | quint32(qRound(pa.y() / 6));
                const int slot = stack.value(key, 0);
                stack.insert(key, slot + 1);
                const QRectF badge(pa.x() + 8 + slot * 20, pa.y() - 26, 18, 18);
                p->setPen(QPen((sel || hov) ? kAmber : kGlyphEdge, 1));
                p->setBrush(kGlyphBg);
                p->drawRoundedRect(badge, 5, 5);
                drawGlyph(p, badge, c.type, ink);
                m_hits.append({badge, c.uid});
            }
        }
    }

    // The dimension being placed.
    if (m_tool == Dimension && m_dimPlacing) {
        DimDraw d;
        Constraint c = m_dimDraft;
        c.value = param::measure(*m_doc, c);
        if (dimGeometry(c, &d))
            drawDim(c, d, kPick, true);
    }
    p->restore();
}

QString Canvas::hitConstraint(const QPoint &viewPos) const
{
    if (!overlayOn())
        return QString();
    for (int i = m_hits.size() - 1; i >= 0; --i)
        if (m_hits.at(i).rect.contains(viewPos))
            return m_hits.at(i).uid;
    return QString();
}

// ---- the inline value editor -------------------------------------------------------------------

void Canvas::editDimension(const QString &uid)
{
    if (!m_doc)
        return;
    const int i = m_doc->parametric().indexOfUid(uid);
    if (i < 0 || !m_doc->parametric().constraints.at(i).isDimension())
        return;
    selectConstraint(uid);
    openDimEditor(m_doc->parametric().constraints.at(i), false);
}

void Canvas::openDimEditor(const Constraint &c, bool isNew)
{
    m_dimEditing = c;
    m_dimEditingNew = isNew;
    if (!m_dimEdit) {
        auto *e = new DimEdit(viewport());
        e->setObjectName(QStringLiteral("dimEdit"));
        e->setAlignment(Qt::AlignCenter);
        e->done = [this](bool escape) { commitDimEditor(escape); };
        m_dimEdit = e;
    }
    m_dimEdit->setStyleSheet(QStringLiteral(
        "#dimEdit { background: #17191e; color: #e8ecf2; border: 1.5px solid #f2a536;"
        " border-radius: 13px; padding: 3px 10px; font-size: 13px; font-weight: 600;"
        " selection-background-color: #4a90d9; }"));
    m_dimEdit->setText(c.expr.isEmpty() ? param::formatNumber(c.value) : c.expr);
    m_dimEdit->setToolTip(tr("%1 = a value (50, 1/4 in) or an expression with parameters (plate_w / 2)")
                              .arg(c.name));
    DimDraw d;
    QPointF at = mapToScene(viewport()->rect().center());
    if (dimGeometry(c, &d))
        at = d.text;
    const QPointF vp = viewportTransform().map(at);
    const int w = qMax(130, m_dimEdit->fontMetrics().horizontalAdvance(m_dimEdit->text()) + 40);
    m_dimEdit->setGeometry(int(vp.x()) - w / 2, int(vp.y()) - 15, w, 30);
    m_dimEdit->show();
    m_dimEdit->raise();
    m_dimEdit->setFocus();
    m_dimEdit->selectAll();
    emit statusHint(tr("%1 — type a value or an expression (parameters, + - * /, units); Enter sets it, Esc %2")
                        .arg(c.name, isNew ? tr("keeps the measured value") : tr("cancels")));
}

void Canvas::closeDimEditor()
{
    if (m_dimEdit && m_dimEdit->isVisible()) {
        m_dimEdit->hide();
        setFocus();
    }
}

void Canvas::commitDimEditor(bool escape)
{
    if (!m_doc || !m_dimEdit || !m_dimEdit->isVisible())
        return;
    if (escape && !m_dimEditingNew) {
        closeDimEditor();
        emit statusHint(tr("Edit cancelled"));
        return;
    }
    Constraint c = m_dimEditing;
    QString text = escape ? param::formatNumber(param::measure(*m_doc, c))
                          : m_dimEdit->text().trimmed();
    if (text.isEmpty())
        text = param::formatNumber(param::measure(*m_doc, c));
    if (param::namesIn(text).contains(c.name)) {
        emit statusHint(tr("%1 can't be defined by itself").arg(c.name));
        return;
    }
    double v = 0;
    QString why;
    if (!param::evaluateIn(*m_doc, text, &v, &why)) {
        m_dimEdit->setStyleSheet(m_dimEdit->styleSheet().replace(QStringLiteral("#f2a536"),
                                                                 QStringLiteral("#e85c5c")));
        emit statusHint(tr("%1 — %2").arg(text, why));
        return;
    }
    if ((c.type == Constraint::Radius || c.type == Constraint::Diameter
         || (c.type == Constraint::Distance && c.refs.size() == 1)) && v <= 0) {
        emit statusHint(tr("%1 must be more than 0").arg(Constraint::typeName(c.type)));
        return;
    }
    c.expr = text;
    Parametric after = m_doc->parametric();
    if (m_dimEditingNew) {
        after.constraints.append(c);
    } else {
        const int i = after.indexOfUid(c.uid);
        if (i < 0 || after.constraints[i].expr == text) {
            closeDimEditor();
            return;
        }
        after.constraints[i].expr = text;
    }
    // A loop through other names ("p = d1 + 1", then d1 = p) only shows
    // with the edit in place.
    if (const QString e = param::newError(*m_doc, m_doc->parametric(), after); !e.isEmpty()) {
        m_dimEdit->setStyleSheet(m_dimEdit->styleSheet().replace(QStringLiteral("#f2a536"),
                                                                 QStringLiteral("#e85c5c")));
        emit statusHint(tr("%1 — %2").arg(text, e));
        return;
    }
    closeDimEditor();
    const QString what = m_dimEditingNew ? tr("add dimension %1 = %2").arg(c.name, text)
                                         : tr("%1 = %2").arg(c.name, text);
    if (applyParametric(after, what)) {
        emit statusHint(what);
    } else {
        emit statusHint(m_dimEditingNew
            ? tr("%1 would over-constrain the sketch — not added").arg(Constraint::typeName(c.type))
            : tr("%1 = %2 can't be reached with the other constraints").arg(c.name, text));
    }
    if (m_dimEditingNew)
        selectConstraint(c.uid);
}

// ---- events --------------------------------------------------------------------------------

bool Canvas::parametricPress(QMouseEvent *event)
{
    if (!m_doc || event->button() != Qt::LeftButton)
        return false;
    if (m_dimEdit && m_dimEdit->isVisible())
        commitDimEditor(false);
    const QPointF pos = mapToScene(event->pos());
    if (m_tool == Constrain) {
        m_hasHover = pickGeo(pos, pickMask(), &m_hover);
        constrainClick();
        event->accept();
        return true;
    }
    if (m_tool == Dimension) {
        m_hasHover = pickGeo(pos, pickMask(), &m_hover);
        dimensionClick(pos, event->modifiers());
        event->accept();
        return true;
    }
    if (!overlayOn() || (m_tool != Select && m_tool != Pan && m_tool != SelectArea))
        return false;
    const QString uid = hitConstraint(event->pos());
    if (uid.isEmpty()) {
        if (!m_selConstraint.isEmpty())
            selectConstraint(QString());
        return false;
    }
    selectConstraint(uid);
    const int i = m_doc->parametric().indexOfUid(uid);
    if (i >= 0 && m_doc->parametric().constraints.at(i).isDimension() && m_tool == Select) {
        m_dragDim = uid;
        m_dragDimFrom = pos;
        m_dragDimLabel0 = m_doc->parametric().constraints.at(i).label;
    }
    const Constraint &c = m_doc->parametric().constraints.at(i);
    emit statusHint(c.isDimension()
        ? tr("%1 = %2 — double-click to change it, drag to move the label, Del deletes").arg(c.name, c.expr)
        : tr("%1 constraint — Del deletes it").arg(Constraint::typeName(c.type)));
    event->accept();
    return true;
}

bool Canvas::parametricMove(QMouseEvent *event)
{
    if (!m_doc)
        return false;
    const QPointF pos = mapToScene(event->pos());
    if (!m_dragDim.isEmpty()) {
        const int i = m_doc->parametric().indexOfUid(m_dragDim);
        if (i >= 0) {
            m_doc->parametricRef().constraints[i].label = m_dragDimLabel0 + (pos - m_dragDimFrom);
            viewport()->update();
        }
        event->accept();
        return true;
    }
    if (m_tool == Constrain || m_tool == Dimension) {
        m_hasHover = pickGeo(pos, pickMask(), &m_hover);
        if (m_dimPlacing)
            updateDimDraft(pos);
        viewport()->update();
        event->accept();
        return true;
    }
    if (overlayOn() && !(event->buttons() & Qt::LeftButton)) {
        const QString uid = hitConstraint(event->pos());
        if (uid != m_hoverConstraint) {
            m_hoverConstraint = uid;
            viewport()->update();
        }
    }
    return false;
}

bool Canvas::parametricRelease(QMouseEvent *event)
{
    if (!m_doc)
        return false;
    if (!m_dragDim.isEmpty()) {
        const QString uid = m_dragDim;
        m_dragDim.clear();
        const int i = m_doc->parametric().indexOfUid(uid);
        if (i >= 0) {
            Parametric after = m_doc->parametric();
            m_doc->parametricRef().constraints[i].label = m_dragDimLabel0;
            if (after.constraints[i].label != m_dragDimLabel0)
                applyParametric(after, tr("move dimension %1").arg(after.constraints[i].name), true);
        }
        event->accept();
        return true;
    }
    if (m_tool == Constrain || m_tool == Dimension) {
        event->accept();
        return true;
    }
    return false;
}

bool Canvas::parametricKey(QKeyEvent *event)
{
    if (!m_doc)
        return false;
    if (event->key() == Qt::Key_Escape && (!m_picks.isEmpty() || m_dimPlacing)) {
        resetPicks();
        return true;
    }
    if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)
        && !m_selConstraint.isEmpty() && !(m_tool == NodeEdit && m_selSub >= 0)) {
        deleteConstraint(m_selConstraint);
        return true;
    }
    return false;
}

bool Canvas::parametricMenu(QContextMenuEvent *event)
{
    if (m_tool == Constrain || m_tool == Dimension) {
        resetPicks();
        return true;
    }
    const QString uid = hitConstraint(event->pos());
    const int i = m_doc ? m_doc->parametric().indexOfUid(uid) : -1;
    if (i < 0)
        return false;
    selectConstraint(uid);
    const Constraint c = m_doc->parametric().constraints.at(i);
    QMenu menu(this);
    if (c.isDimension())
        connect(menu.addAction(tr("Edit %1…").arg(c.name)), &QAction::triggered, this,
                [this, uid] { editDimension(uid); });
    connect(menu.addAction(c.isDimension() ? tr("Delete dimension") : tr("Delete %1 constraint")
                                                .arg(Constraint::typeName(c.type).toLower())),
            &QAction::triggered, this, [this, uid] { deleteConstraint(uid); });
    menu.exec(event->globalPos());
    return true;
}

} // namespace c2d
