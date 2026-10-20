// The parametric tools on the real canvas (offscreen, synthetic events):
// - Constrain: picking geometry adds the constraint and the drawing obeys
//   it; one undo step each;
// - Dimension: pick a line, place the label, type an expression, Enter;
//   a parameter change then drives the geometry;
// - edits keep the sketch true: dragging a node or moving the shape
//   re-solves in the same undo step;
// - Delete removes the selected constraint.
//
// Plain asserts, no test framework; exits 0 on success.

#include "../src/canvas.h"
#include "../src/c2ddocument.h"
#include "../src/element.h"
#include "../src/parameterspanel.h"

#include <QApplication>
#include <QHeaderView>
#include <QJsonObject>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace c2d;

static int g_checks = 0;

static void check(bool cond, const char *what)
{
    ++g_checks;
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

// Replace one element, as a node delete or a fillet does.
class SwapCmd : public QUndoCommand
{
public:
    SwapCmd(Canvas *c, Document *d, const Element &before, const Element &after)
        : QUndoCommand(QStringLiteral("swap")), m_c(c), m_d(d), m_before(before), m_after(after) {}
    void redo() override { m_d->replaceElement(m_after); m_c->rebuild(); }
    void undo() override { m_d->replaceElement(m_before); m_c->rebuild(); }
private:
    Canvas *m_c;
    Document *m_d;
    Element m_before, m_after;
};

static QPoint vp(Canvas *c, QPointF mm) { return c->mapFromScene(mm); }

static void click(Canvas *c, QPointF mm, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QTest::mouseMove(c->viewport(), vp(c, mm));
    QTest::mouseClick(c->viewport(), Qt::LeftButton, mods, vp(c, mm));
}

static void drag(Canvas *c, QPointF from, QPointF to)
{
    QTest::mousePress(c->viewport(), Qt::LeftButton, Qt::NoModifier, vp(c, from));
    QTest::mouseMove(c->viewport(), vp(c, (from + to) / 2));
    QTest::mouseMove(c->viewport(), vp(c, to));
    QTest::mouseRelease(c->viewport(), Qt::LeftButton, Qt::NoModifier, vp(c, to));
}

static QPointF node(const Document &d, const QString &id, int i)
{
    GeoRef r;
    r.id = id;
    r.index = i;
    QPointF p;
    check(param::point(d, r, &p), "node resolves");
    return p;
}

static bool approx(double a, double b, double eps = 1e-4) { return std::fabs(a - b) < eps; }

int main(int argc, char *argv[])
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && qEnvironmentVariableIsEmpty("DISPLAY")
        && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    QTemporaryDir tmp;
    QString err;
    const QString blank = tmp.filePath("blank.c2d");
    check(Document::createBlank(blank, 300, 200, 12, &err), "createBlank");
    Document doc;
    check(doc.load(blank, &err), "load");
    const Element quad = Element::makePath({{20, 20}, {80, 26}, {76, 70}, {24, 64}}, true,
                                           doc.defaultLayer());
    doc.addElement(quad);
    const QString id = quad.id;

    Canvas canvas;
    canvas.resize(900, 700);
    canvas.show();
    canvas.setDocument(&doc);
    canvas.resetTransform();
    canvas.scale(3, -3);
    canvas.centerOn(QPointF(60, 50));
    QUndoStack *undo = canvas.undoStack();
    QTest::qWait(20);

    // --- Constrain: Horizontal on the bottom line --------------------------------
    canvas.setTool(Canvas::Constrain);
    canvas.setConstraintType(Constraint::Horizontal);
    click(&canvas, QPointF(50, 23));                      // middle of segment 0
    check(doc.parametric().constraints.size() == 1, "horizontal added");
    check(approx(node(doc, id, 0).y(), node(doc, id, 1).y()), "bottom line is horizontal");
    const int afterH = undo->count();
    undo->undo();
    check(doc.parametric().constraints.isEmpty(), "undo removes it");
    check(approx(node(doc, id, 1).y() - node(doc, id, 0).y(), 6, 1e-9), "undo puts the line back");
    undo->redo();
    check(undo->count() == afterH && doc.parametric().constraints.size() == 1, "redo");

    // Vertical on the right line, then Perpendicular left/bottom.
    canvas.setConstraintType(Constraint::Vertical);
    const QPointF r1 = (node(doc, id, 1) + node(doc, id, 2)) / 2;
    click(&canvas, r1);
    check(doc.parametric().constraints.size() == 2, "vertical added");
    check(approx(node(doc, id, 1).x(), node(doc, id, 2).x()), "right line vertical");
    canvas.setConstraintType(Constraint::Parallel);
    click(&canvas, (node(doc, id, 2) + node(doc, id, 3)) / 2);
    click(&canvas, (node(doc, id, 0) + node(doc, id, 1)) / 2);
    check(doc.parametric().constraints.size() == 3, "parallel takes two picks");
    check(approx(node(doc, id, 2).y(), node(doc, id, 3).y()), "top parallel to bottom");

    // Asking for the same constraint again changes nothing.
    canvas.setConstraintType(Constraint::Vertical);
    const int n0 = undo->count();
    click(&canvas, (node(doc, id, 1) + node(doc, id, 2)) / 2);
    check(undo->count() == n0 && doc.parametric().constraints.size() == 3, "no duplicate");

    // --- Dimension: the bottom line's length = plate ------------------------------
    {
        Parametric p = doc.parametric();
        p.params.append({QStringLiteral("plate"), QStringLiteral("50"), QString()});
        check(canvas.applyParametric(p, QStringLiteral("add plate")), "add parameter");
    }
    canvas.setTool(Canvas::Dimension);
    const QPointF bottom = (node(doc, id, 0) + node(doc, id, 1)) / 2;
    click(&canvas, bottom);                                // the line
    click(&canvas, bottom - QPointF(0, 12));               // empty space: place it
    QLineEdit *edit = canvas.viewport()->findChild<QLineEdit *>(QStringLiteral("dimEdit"));
    check(edit && edit->isVisible(), "value editor opens");
    edit->selectAll();
    QTest::keyClicks(edit, QStringLiteral("plate"));
    QTest::keyClick(edit, Qt::Key_Return);
    check(!edit->isVisible(), "editor closes on Enter");
    const Constraint &dim = doc.parametric().constraints.last();
    check(dim.isDimension() && dim.name == QLatin1String("d1") && dim.expr == QLatin1String("plate"),
          "dimension d1 = plate");
    check(approx(QLineF(node(doc, id, 0), node(doc, id, 1)).length(), 50), "line is 50 long");
    check(dim.label.y() < -5, "label placed below the line");

    // A bad expression keeps the editor open.
    canvas.editDimension(dim.uid);
    check(edit->isVisible(), "double-click path opens the editor");
    edit->selectAll();
    QTest::keyClicks(edit, QStringLiteral("nothing + 1"));
    QTest::keyClick(edit, Qt::Key_Return);
    check(edit->isVisible() && doc.parametric().constraints.last().expr == QLatin1String("plate"),
          "unknown name refused");
    QTest::keyClick(edit, Qt::Key_Escape);
    check(!edit->isVisible(), "Esc cancels an edit");

    // The parameter drives it.
    {
        Parametric p = doc.parametric();
        p.params[0].expr = QStringLiteral("64");
        check(canvas.applyParametric(p, QStringLiteral("plate = 64"), true), "change parameter");
    }
    check(approx(QLineF(node(doc, id, 0), node(doc, id, 1)).length(), 64), "line follows plate");
    undo->undo();
    check(approx(QLineF(node(doc, id, 0), node(doc, id, 1)).length(), 50), "undo the parameter");
    undo->redo();

    // --- edits re-solve in the same undo step -----------------------------------
    canvas.setTool(Canvas::NodeEdit);
    canvas.selectElements({id});
    QTest::qWait(10);
    const QPointF n1 = node(doc, id, 1);
    const int before = undo->count();
    drag(&canvas, n1, n1 + QPointF(10, 8));
    check(undo->count() == before + 1, "node drag + solve is one undo step");
    check(approx(node(doc, id, 0).y(), node(doc, id, 1).y()), "still horizontal after the drag");
    check(approx(QLineF(node(doc, id, 0), node(doc, id, 1)).length(), 64), "still 64 long");
    check(approx(node(doc, id, 1).x(), node(doc, id, 2).x()), "still vertical");
    undo->undo();
    check(approx(QLineF(node(doc, id, 1), n1).length(), 0), "one undo restores");

    // --- the Parameters tab refuses what can't hold ---------------------------------
    {
        ParametersPanel panel(&canvas);
        panel.setDocument(&doc);
        QTableWidget *params = nullptr, *dims = nullptr;
        for (QTableWidget *t : panel.findChildren<QTableWidget *>())
            (t->horizontalHeaderItem(t->columnCount() - 1)->text() == QLatin1String("Comment") ? params : dims) = t;
        check(params && dims, "both tables");
        const QPointF a0 = node(doc, id, 0), a1 = node(doc, id, 1);
        const int steps = undo->count();
        // plate is row 3 (after stock_w, stock_h, stock_t): -5 can't be a length.
        params->item(3, 1)->setText(QStringLiteral("-5"));
        check(doc.parametric().params.at(0).expr == QLatin1String("64"), "an unreachable value is refused");
        check(undo->count() == steps && approx(QLineF(node(doc, id, 0), a0).length(), 0)
                  && approx(QLineF(node(doc, id, 1), a1).length(), 0), "and nothing moved");
        // q uses d1; d1 = q would go round in a loop.
        Parametric p = doc.parametric();
        p.params.append({QStringLiteral("q"), QStringLiteral("d1 + 1"), QString()});
        check(canvas.applyParametric(p, QStringLiteral("add q")), "add q");
        int d1row = -1;
        for (int r = 0; r < dims->rowCount(); ++r)
            if (dims->item(r, 0)->text() == QLatin1String("d1"))
                d1row = r;
        check(d1row >= 0, "d1 row");
        dims->item(d1row, 2)->setText(QStringLiteral("q"));
        check(doc.parametric().constraints.last().expr == QLatin1String("plate"), "a loop through q is refused");
        // The canvas editor refuses it too.
        canvas.editDimension(doc.parametric().constraints.last().uid);
        QLineEdit *ed = canvas.viewport()->findChild<QLineEdit *>(QStringLiteral("dimEdit"));
        ed->selectAll();
        QTest::keyClicks(ed, QStringLiteral("q * 2"));
        QTest::keyClick(ed, Qt::Key_Return);
        check(ed->isVisible() && doc.parametric().constraints.last().expr == QLatin1String("plate"),
              "the canvas editor refuses the loop");
        QTest::keyClick(ed, Qt::Key_Escape);
    }

    // --- Delete removes a selected constraint --------------------------------------
    canvas.setTool(Canvas::Select);
    const QString vuid = doc.parametric().constraints.at(1).uid;
    canvas.selectConstraint(vuid);
    canvas.setFocus();
    QTest::keyClick(&canvas, Qt::Key_Delete);
    check(doc.parametric().indexOfUid(vuid) < 0, "Delete removes the constraint");
    check(doc.elementById(id) != nullptr, "and leaves the shape");

    // Deleting the shape takes its constraints along in the same undo step; d1,
    // which q uses, stays as a parameter.
    {
        const int nCons = doc.parametric().constraints.size();
        const int steps = undo->count();
        canvas.selectElements({id});
        canvas.deleteSelected();
        check(undo->count() == steps + 1, "delete is one undo step");
        check(doc.parametric().constraints.isEmpty(), "its constraints went with it");
        check(doc.parametric().indexOfParam(QStringLiteral("d1")) >= 0, "d1 lives on as a parameter");
        double v = 0;
        check(param::evaluateIn(doc, QStringLiteral("q"), &v) && approx(v, 65), "q still evaluates");
        undo->undo();
        check(doc.elementById(id) && doc.parametric().constraints.size() == nCons
                  && doc.parametric().indexOfParam(QStringLiteral("d1")) < 0, "undo brings it all back");
    }

    // A node delete renumbers the constraints after it; undo puts the old
    // numbers back with the node.
    {
        const Element open = Element::makePath({{150, 20}, {160, 30}, {170, 20}, {190, 20}}, false,
                                               doc.defaultLayer());
        doc.addElement(open);
        Parametric p = doc.parametric();
        Constraint h;
        h.type = Constraint::Horizontal;
        GeoRef seg;
        seg.kind = GeoRef::Line; seg.id = open.id; seg.index = 2;           // node 2 -> 3
        h.refs = {seg};
        h.uid = p.nextUid();
        p.constraints.append(h);
        check(canvas.applyParametric(p, QStringLiteral("add H")), "H on the last segment");
        PathModel m = Element::pathModel(*doc.elementById(open.id));
        check(m.subs[0].removeNode(1), "remove node 1");
        const Element was = *doc.elementById(open.id);
        canvas.pushEdit(new SwapCmd(&canvas, &doc, was, Element::withPathModel(was, m)), {open.id});
        const int at = doc.parametric().indexOfUid(h.uid);
        check(at >= 0 && doc.parametric().constraints.at(at).refs.at(0).index == 1, "H moved to segment 1");
        undo->undo();
        check(doc.parametric().constraints.at(doc.parametric().indexOfUid(h.uid)).refs.at(0).index == 2,
              "undo puts it back on segment 2");
        undo->redo();
        check(doc.parametric().constraints.at(doc.parametric().indexOfUid(h.uid)).refs.at(0).index == 1,
              "redo moves it again");
    }

    // A constraint that already can't hold doesn't block an unrelated edit.
    {
        const Element round = [&] {
            Element r = Element::makeRectangle({240, 40}, 20, 10, doc.defaultLayer());
            r.raw.insert("corner_type", 1);
            return Element::fromJson(r.raw);
        }();
        doc.addElement(round);
        Parametric p = doc.parametric();
        Constraint w;
        w.type = Constraint::Distance;
        GeoRef e;
        e.kind = GeoRef::Line; e.id = round.id; e.index = 0;
        w.refs = {e};
        w.uid = p.nextUid();
        w.name = p.nextDimensionName();
        w.expr = QStringLiteral("30");                 // can't: its size is locked
        p.constraints.append(w);
        check(canvas.applyParametric(p, QStringLiteral("width"), true), "added, failing");
        check(param::unsatisfied(doc).contains(w.uid), "it is red");
        Parametric p2 = doc.parametric();
        p2.params.append({QStringLiteral("depth2"), QStringLiteral("stock_t / 2"), QString()});
        check(canvas.applyParametric(p2, QStringLiteral("add depth2")), "an unrelated edit still goes in");
        Parametric p3 = doc.parametric();
        p3.constraints.removeAt(p3.indexOfUid(w.uid));
        check(canvas.applyParametric(p3, QStringLiteral("drop width"), true), "clean up");
    }

    // A value edit with something already red: the rest follows the edit
    // exactly, the red one is left out instead of bending what holds.
    {
        const Element ln = Element::makePath({{120, 120}, {170, 121}}, false, doc.defaultLayer());
        doc.addElement(ln);
        GeoRef seg, n0;
        seg.kind = GeoRef::Line; seg.id = ln.id; seg.index = 0;
        n0.id = ln.id; n0.index = 0;
        Parametric p = doc.parametric();
        p.params.append({QStringLiteral("L"), QStringLiteral("50"), QString()});
        Constraint fix;
        fix.type = Constraint::Fix;
        fix.refs = {n0};
        fix.at = QPointF(120, 120);
        fix.uid = p.nextUid();
        p.constraints.append(fix);
        Constraint h;
        h.type = Constraint::Horizontal;
        h.refs = {seg};
        h.uid = p.nextUid();
        p.constraints.append(h);
        Constraint len;
        len.type = Constraint::Distance;
        len.refs = {seg};
        len.uid = p.nextUid();
        len.name = p.nextDimensionName();
        len.expr = QStringLiteral("L");
        p.constraints.append(len);
        check(canvas.applyParametric(p, QStringLiteral("line")), "line constrained");
        check(approx(QLineF(node(doc, ln.id, 0), node(doc, ln.id, 1)).length(), 50), "line is 50");
        Parametric p2 = doc.parametric();
        Constraint len2 = len;
        len2.uid = p2.nextUid();
        len2.name = p2.nextDimensionName();
        len2.expr = QStringLiteral("40");
        p2.constraints.append(len2);
        check(canvas.applyParametric(p2, QStringLiteral("second length"), true), "conflicting length kept");
        check(param::unsatisfied(doc) == QStringList{len2.uid}, "only the second length is red");
        const QPointF q0 = node(doc, id, 0), q2 = node(doc, id, 2);
        const int steps = undo->count();
        Parametric p3 = doc.parametric();
        p3.params[p3.indexOfParam(QStringLiteral("L"))].expr = QStringLiteral("60");
        check(canvas.applyParametric(p3, QStringLiteral("L = 60")), "the edit goes in");
        check(undo->count() == steps + 1, "one undo step");
        check(approx(QLineF(node(doc, ln.id, 0), node(doc, ln.id, 1)).length(), 60, 1e-6),
              "the line follows L exactly, no compromise with the red 40");
        check(approx(QLineF(node(doc, ln.id, 0), QPointF(120, 120)).length(), 0, 1e-6)
                  && approx(node(doc, ln.id, 0).y(), node(doc, ln.id, 1).y(), 1e-6),
              "the fix and the horizontal still hold");
        check(param::unsatisfied(doc) == QStringList{len2.uid}, "the second length is still the only red one");
        check(approx(QLineF(node(doc, id, 0), q0).length(), 0, 1e-9)
                  && approx(QLineF(node(doc, id, 2), q2).length(), 0, 1e-9), "nothing else moved");
        // A value the rest can't follow even without the red one is refused.
        Parametric p4 = doc.parametric();
        Constraint v;
        v.type = Constraint::Vertical;
        v.refs = {seg};
        v.uid = p4.nextUid();
        p4.constraints.append(v);
        check(!canvas.applyParametric(p4, QStringLiteral("V")), "a new failure is refused");
        check(doc.parametric().indexOfUid(v.uid) < 0 && undo->count() == steps + 1, "and nothing changed");

        // --- the value editor follows its dimension when the view moves ---------
        canvas.editDimension(len.uid);
        QLineEdit *ed = canvas.viewport()->findChild<QLineEdit *>(QStringLiteral("dimEdit"));
        check(ed && ed->isVisible(), "editor open on the line's length");
        const QPoint c0 = ed->geometry().center();
        canvas.zoomBy(1.5);
        check((ed->geometry().center() - c0).manhattanLength() > 10, "it moves with a zoom");
        canvas.zoomBy(1 / 1.5);
        check((ed->geometry().center() - c0).manhattanLength() <= 2, "and comes back with it");
        QScrollBar *hs = canvas.horizontalScrollBar();
        if (hs->maximum() - hs->minimum() > 80) {
            const int at = hs->value() + 40 <= hs->maximum() ? hs->value() + 40 : hs->value() - 40;
            const int dx = at - hs->value();
            hs->setValue(at);
            check(std::abs(ed->geometry().center().x() - (c0.x() - dx)) <= 1, "it moves with a scroll");
            hs->setValue(at - dx);
        }
        QTest::keyClick(ed, Qt::Key_Escape);
        Parametric p5 = doc.parametric();
        p5.constraints.removeAt(p5.indexOfUid(len2.uid));
        check(canvas.applyParametric(p5, QStringLiteral("drop second length"), true), "clean up");
    }

    // An angle dimension is between 0 and 180: 270 is refused everywhere.
    {
        const Element ang = Element::makePath({{120, 160}, {160, 160}, {180, 185}}, false, doc.defaultLayer());
        doc.addElement(ang);
        Parametric p = doc.parametric();
        Constraint a;
        a.type = Constraint::Angle;
        GeoRef l0, l1;
        l0.kind = l1.kind = GeoRef::Line;
        l0.id = l1.id = ang.id;
        l0.index = 0; l1.index = 1;
        a.refs = {l0, l1};
        a.uid = p.nextUid();
        a.name = p.nextDimensionName();
        param::prime(doc, a);
        a.expr = QStringLiteral("60");
        p.constraints.append(a);
        check(canvas.applyParametric(p, QStringLiteral("angle")), "angle dimension added");
        canvas.editDimension(a.uid);
        QLineEdit *ed = canvas.viewport()->findChild<QLineEdit *>(QStringLiteral("dimEdit"));
        ed->selectAll();
        QTest::keyClicks(ed, QStringLiteral("270"));
        QTest::keyClick(ed, Qt::Key_Return);
        const auto exprOf = [&] { return doc.parametric().constraints.at(doc.parametric().indexOfUid(a.uid)).expr; };
        check(ed->isVisible() && exprOf() == QLatin1String("60"), "the canvas editor refuses 270");
        QTest::keyClick(ed, Qt::Key_Escape);
        ParametersPanel panel(&canvas);
        panel.setDocument(&doc);
        QTableWidget *dims = nullptr;
        for (QTableWidget *t : panel.findChildren<QTableWidget *>())
            if (t->horizontalHeaderItem(t->columnCount() - 1)->text() != QLatin1String("Comment"))
                dims = t;
        int row = -1;
        for (int r = 0; r < dims->rowCount(); ++r)
            if (dims->item(r, 0)->data(Qt::UserRole).toString() == a.uid)
                row = r;
        check(row >= 0, "angle row");
        dims->item(row, 2)->setText(QStringLiteral("270"));
        check(exprOf() == QLatin1String("60"), "the Parameters tab refuses 270");
        dims->item(row, 2)->setText(QStringLiteral("100"));
        check(exprOf() == QLatin1String("100"), "100 goes in");
    }

    // Removing a parameter unbinds the toolpath fields bound to it, keeping
    // their values, in the same undo step.
    {
        Parametric p = doc.parametric();
        p.params.append({QStringLiteral("cut"), QStringLiteral("4"), QString()});
        check(canvas.applyParametric(p, QStringLiteral("add cut")), "add cut");
        Toolpath t;
        t.uuid = QStringLiteral("tpcut");
        t.type = QStringLiteral("contour_toolpath");
        t.json.insert(QStringLiteral("name"), QStringLiteral("Outline"));
        t.json.insert(QStringLiteral("end_depth"), QStringLiteral("-4.000"));
        t.json.insert(QStringLiteral("feed"), 1000);
        QJsonObject binds;
        binds.insert(QStringLiteral("end_depth"), QJsonObject{{"expr", "cut"}, {"neg", true}, {"str", true}});
        binds.insert(QStringLiteral("feed"), QJsonObject{{"expr", "stock_t * 100"}});
        t.json.insert(QStringLiteral("phobic_expr"), binds);
        doc.addToolpath(t);
        ParametersPanel panel(&canvas);
        panel.setDocument(&doc);
        panel.refresh();
        QTableWidget *params = nullptr;
        for (QTableWidget *tw : panel.findChildren<QTableWidget *>())
            if (tw->horizontalHeaderItem(tw->columnCount() - 1)->text() == QLatin1String("Comment"))
                params = tw;
        int row = -1;
        for (int r = 0; r < params->rowCount(); ++r)
            if (params->item(r, 0) && params->item(r, 0)->text() == QLatin1String("cut"))
                row = r;
        check(row >= 0, "cut row");
        params->setCurrentCell(row, 0);
        QPushButton *remove = nullptr;
        for (QPushButton *b : panel.findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Remove"))
                remove = b;
        check(remove && remove->isEnabled(), "Remove button");
        const int steps = undo->count();
        remove->click();
        const auto tpJson = [&] { return doc.toolpaths().last().json; };
        check(doc.parametric().indexOfParam(QStringLiteral("cut")) < 0, "cut removed");
        check(undo->count() == steps + 1, "one undo step");
        check(!tpJson().value(QStringLiteral("phobic_expr")).toObject().contains(QStringLiteral("end_depth"))
                  && tpJson().value(QStringLiteral("phobic_expr")).toObject().contains(QStringLiteral("feed")),
              "only the field bound to cut is unbound");
        check(tpJson().value(QStringLiteral("end_depth")).toString() == QLatin1String("-4.000"), "its value kept");
        Parametric again = doc.parametric();
        again.params.append({QStringLiteral("cut"), QStringLiteral("9"), QString()});
        check(canvas.applyParametric(again, QStringLiteral("add cut again")), "a new cut");
        check(tpJson().value(QStringLiteral("end_depth")).toString() == QLatin1String("-4.000"),
              "a new parameter of the same name doesn't take it over");
        undo->undo();
        undo->undo();
        check(tpJson().value(QStringLiteral("phobic_expr")).toObject().contains(QStringLiteral("end_depth"))
                  && doc.parametric().indexOfParam(QStringLiteral("cut")) >= 0, "undo binds it again");
        undo->redo();
    }

    // Freedom: a closed quad with H, parallel and a length still slides about.
    canvas.rebuild();
    check(canvas.freedomLeft() > 0 && !canvas.fullyConstrained().contains(id), "not fully constrained");

    // Convert to path on a rectangle: its edges run the other way as path
    // segments, and a distance or angle on one keeps its side and value.
    {
        auto ref = [](GeoRef::Kind k, const QString &rid, int index) {
            GeoRef r;
            r.kind = k; r.id = rid; r.index = index;
            return r;
        };
        auto add = [](Document &d, Constraint::Type t, const QVector<GeoRef> &refs, const QString &expr = QString()) {
            Constraint c;
            c.type = t;
            c.refs = refs;
            c.uid = d.parametric().nextUid();
            if (c.isDimension()) { c.name = d.parametric().nextDimensionName(); c.expr = expr; }
            param::prime(d, c);
            d.parametricRef().constraints.append(c);
        };
        Document cd;
        const Element rect = Element::makeRectangle({50, 50}, 40, 20, cd.defaultLayer());
        const Element pt = Element::makePoint({50, 45}, cd.defaultLayer());       // 5 above the bottom, inside
        const Element ln = Element::makePath({{80, 80}, {100, 90}}, false, cd.defaultLayer());
        cd.addElement(rect);
        cd.addElement(pt);
        cd.addElement(ln);
        add(cd, Constraint::Distance, {ref(GeoRef::Point, pt.id, -1), ref(GeoRef::Line, rect.id, 0)}, "5");
        add(cd, Constraint::Fix, {ref(GeoRef::Line, rect.id, 0)});
        add(cd, Constraint::Angle, {ref(GeoRef::Line, rect.id, 0), ref(GeoRef::Line, ln.id, 0)}, "30");
        param::solve(cd);
        canvas.setDocument(&cd);
        QPointF a0, b0;
        check(param::line(cd, ref(GeoRef::Line, ln.id, 0), &a0, &b0), "free line resolves");
        canvas.convertToPaths({rect.id});
        check(cd.elementById(rect.id)->geometryType == QLatin1String("path"), "rectangle is a path");
        check(cd.parametric().constraints.size() == 3 && param::unsatisfied(cd).isEmpty(), "all three still hold");
        check(QLineF(node(cd, pt.id, -1), QPointF(50, 45)).length() < 1e-6, "the point stays inside, 5 above the bottom");
        QPointF a1, b1;
        check(param::line(cd, ref(GeoRef::Line, ln.id, 0), &a1, &b1)
                  && QLineF(a0, a1).length() < 1e-6 && QLineF(b0, b1).length() < 1e-6, "the angled line stays put");
    }

    // Fix on a whole shape holds it as Fix on a point does: a move is put back.
    {
        Document fd;
        const Element c1 = Element::makeCircle({50, 50}, 10, fd.defaultLayer());
        fd.addElement(c1);
        Constraint c;
        c.type = Constraint::Fix;
        GeoRef r;
        r.kind = GeoRef::Circle;
        r.id = c1.id;
        c.refs = {r};
        c.uid = fd.parametric().nextUid();
        fd.parametricRef().constraints.append(c);
        canvas.setDocument(&fd);
        QString hint;
        auto conn = QObject::connect(&canvas, &Canvas::statusHint, [&](const QString &h) { hint = h; });
        canvas.moveElementBy(c1.id, 30, 0);
        QObject::disconnect(conn);
        check(QLineF(node(fd, c1.id, -1), QPointF(50, 50)).length() < 1e-9, "the locked circle stays put");
        check(hint.contains(QStringLiteral("locked")), "and the status bar says why");
        canvas.undoStack()->undo();
        check(QLineF(node(fd, c1.id, -1), QPointF(50, 50)).length() < 1e-9, "undo leaves it there too");
        canvas.setDocument(&doc);
    }

    // Trim, Extend and Break refuse a shape locked with Fix: no "done" in the
    // status bar, and no pieces left beside the shape put back.
    {
        Document fd;
        const Element h = Element::makePath({{0, 50}, {100, 50}}, false, fd.defaultLayer());
        const Element v1 = Element::makePath({{30, 0}, {30, 100}}, false, fd.defaultLayer());
        const Element v2 = Element::makePath({{120, 0}, {120, 100}}, false, fd.defaultLayer());
        fd.addElement(h);
        fd.addElement(v1);
        fd.addElement(v2);
        Constraint c;
        c.type = Constraint::Fix;
        GeoRef r;
        r.kind = GeoRef::Line;
        r.id = h.id;
        r.index = 0;
        c.refs = {r};
        c.uid = fd.parametric().nextUid();
        fd.parametricRef().constraints.append(c);
        canvas.setDocument(&fd);
        canvas.resetTransform();
        canvas.scale(3, -3);
        canvas.centerOn(QPointF(60, 50));
        QString hint;
        auto conn = QObject::connect(&canvas, &Canvas::statusHint, [&](const QString &s) { hint = s; });
        const int steps = undo->count();
        for (Canvas::Tool t : {Canvas::Break, Canvas::Trim, Canvas::Extend}) {
            canvas.setTool(t);
            hint.clear();
            click(&canvas, QPointF(t == Canvas::Extend ? 95 : 60, 50));
            check(fd.elements().size() == 3, "locked line: nothing added or removed");
            check(hint.contains(QStringLiteral("locked")), "locked line: the status bar says why");
        }
        const Element *still = fd.elementById(h.id);
        check(still && still->raw == h.raw, "locked line: unchanged");
        check(undo->count() == steps, "locked line: no undo step");
        QObject::disconnect(conn);
        canvas.setTool(Canvas::Select);
        canvas.setDocument(&doc);
    }

    std::printf("canvasparametric: %d checks passed\n", g_checks);
    return 0;
}
