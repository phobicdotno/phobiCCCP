// Layers on the Design page: a hidden layer's shapes are not drawn and are
// skipped by Select all, a locked layer's shapes can't be picked on the
// canvas, each layer change is one undo step, and the flags (Carbide
// Create's own `visible` / `locked` in each element's layer object) come
// back from a save and load.
#include "../src/c2ddocument.h"
#include "../src/canvas.h"
#include "../src/element.h"
#include "../src/layers.h"
#include "../src/layerspanel.h"
#include <QApplication>
#include <QHBoxLayout>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>
#include <cstdio>

static int failures = 0;
static void check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++failures;
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QTemporaryDir tmp;
    QString err;
    const QString blank = tmp.filePath("a.c2d");
    c2d::Document::createBlank(blank, 300, 300, 19, &err);
    c2d::Document d;
    d.load(blank, &err);

    const c2d::Element a = c2d::Element::makeRectangle({30, 30}, 20, 20, d.defaultLayer());
    d.addElement(a);
    QJsonObject hid = c2d::layers::makeLayer(QStringLiteral("Clamps"));
    hid.insert("visible", false);
    QJsonObject lck = c2d::layers::makeLayer(QStringLiteral("Border"));
    lck.insert("locked", true);
    const c2d::Element b = c2d::Element::makeRectangle({80, 30}, 20, 20, hid);
    const c2d::Element c = c2d::Element::makeRectangle({130, 30}, 20, 20, lck);
    d.addElement(b);
    d.addElement(c);
    const QString hidKey = c2d::layers::keyOf(hid), lckKey = c2d::layers::keyOf(lck);
    const QString defKey = c2d::layers::keyOf(a);

    const QVector<c2d::layers::Layer> ls = c2d::layers::list(d);
    check(ls.size() == 3 && ls.at(1).name() == "Clamps" && !ls.at(1).visible()
              && ls.at(2).locked(), "three layers listed with their flags");

    QWidget win;
    auto *h = new QHBoxLayout(&win);
    auto *canvas = new c2d::Canvas;
    h->addWidget(canvas, 1);
    canvas->setDocument(&d);
    auto *panel = new c2d::LayersPanel(canvas);
    h->addWidget(panel);
    win.resize(1200, 800);
    win.show();
    QApplication::setActiveWindow(&win);
    QTest::qWaitForWindowActive(&win);
    canvas->setTool(c2d::Canvas::Select);
    canvas->resetTransform();
    canvas->scale(4, -4);
    canvas->centerOn(QPointF(80, 30));
    QWidget *vp = canvas->viewport();
    auto at = [&](double x, double y) { return canvas->mapFromScene(QPointF(x, y)); };

    // Select all: only the shape on the shown, unlocked layer.
    canvas->selectAll();
    check(canvas->selectedElementIds() == QStringList{a.id}, "select all skips hidden and locked layers");

    // A click on the locked layer's shape does not pick it; on A it does.
    canvas->selectElements({});
    QTest::mouseClick(vp, Qt::LeftButton, {}, at(130, 30));
    check(canvas->selectedElementIds().isEmpty(), "a locked layer's shape can't be hit-selected");
    QTest::mouseClick(vp, Qt::LeftButton, {}, at(80, 30));
    check(canvas->selectedElementIds().isEmpty(), "a hidden layer's shape can't be hit-selected");
    QTest::mouseClick(vp, Qt::LeftButton, {}, at(30, 30));
    check(canvas->selectedElementIds() == QStringList{a.id}, "a shown layer's shape can");

    // Tapping a layer selects its shapes (none for a locked one).
    panel->selectLayer(lckKey);
    check(canvas->selectedElementIds().isEmpty(), "picking a locked layer selects nothing");
    panel->selectLayer(defKey);
    check(canvas->selectedElementIds() == QStringList{a.id}, "picking a layer selects its shapes");

    // New shapes land on a shown, unlocked layer.
    check(c2d::layers::keyOf(d.defaultLayer()) == defKey, "new shapes go on a usable layer");
    {
        // Only a hidden blank-uuid DEFAULT layer: the fallback is a layer of its own.
        c2d::Document h;
        h.load(blank, &err);
        QJsonObject def = h.defaultLayer();
        def.insert("visible", false);
        h.addElement(c2d::Element::makeRectangle({30, 30}, 20, 20, def));
        const QJsonObject fb = h.defaultLayer();
        check(c2d::layers::keyOf(fb) != c2d::layers::keyOf(def) && fb.value("visible").toBool(),
              "fallback layer is not the hidden DEFAULT");
    }

    // Show the hidden layer: one undo step, and undo hides it again.
    QUndoStack *undo = canvas->undoStack();
    const int steps = undo->count();
    panel->setVisible(hidKey, true);
    check(undo->count() == steps + 1, "showing a layer is one undo step");
    check(!c2d::layers::isHidden(*d.elementById(b.id)), "the layer is shown");
    canvas->selectAll();
    check(canvas->selectedElementIds().size() == 2, "select all now takes its shape too");
    undo->undo();
    check(c2d::layers::isHidden(*d.elementById(b.id)), "undo hides it again");

    // Move a shape to a new layer, then rename that layer.
    panel->addLayer(QStringLiteral("Pockets"));
    const QString newKey = panel->currentKey();
    check(!newKey.isEmpty() && c2d::layers::list(d).size() == 3, "an added layer waits in the panel only");
    canvas->selectElements({a.id});
    panel->moveSelectionTo(newKey);
    check(c2d::layers::keyOf(*d.elementById(a.id)) == newKey, "move selection to layer");
    check(panel->deleteLayer(defKey), "the emptied layer can be deleted");
    check(!panel->deleteLayer(newKey), "a layer with shapes can't be deleted");
    panel->renameLayer(newKey, QStringLiteral("Inlays"));
    check(d.elementById(a.id)->raw.value("layer").toObject().value("name").toString() == "Inlays",
          "rename layer");

    // Lock / unlock through the panel.
    panel->setLocked(lckKey, false);
    canvas->selectElements({});
    QTest::mouseClick(vp, Qt::LeftButton, {}, at(130, 30));
    check(canvas->selectedElementIds() == QStringList{c.id}, "an unlocked layer's shape can be picked");
    panel->setLocked(lckKey, true);

    // Group / Ungroup: a click on one grouped shape picks the group.
    const c2d::Element e1 = c2d::Element::makeCircle({30, 80}, 8, d.defaultLayer());
    const c2d::Element e2 = c2d::Element::makeCircle({60, 80}, 8, d.defaultLayer());
    d.addElement(e1);
    d.addElement(e2);
    canvas->rebuild();
    auto sorted = [](QStringList l) { l.sort(); return l; };
    const QStringList pair = sorted({e1.id, e2.id});
    canvas->selectElements({e1.id, e2.id});
    const int before = undo->count();
    canvas->groupSelected();
    check(undo->count() == before + 1, "group is one undo step");
    canvas->selectElements({});
    QTest::mouseClick(vp, Qt::LeftButton, {}, at(30, 80));
    check(sorted(canvas->selectedElementIds()) == pair, "a click on a grouped shape picks the group");
    canvas->selectElements({});
    QTest::mouseClick(vp, Qt::LeftButton, Qt::AltModifier, at(30, 80));
    check(canvas->selectedElementIds() == QStringList{e1.id}, "Alt+click picks one shape of a group");
    const QString gid = c2d::groups::outermost(*d.elementById(e1.id));
    check(!gid.isEmpty() && gid == c2d::groups::outermost(*d.elementById(e2.id)), "both share the group");
    undo->undo();
    check(c2d::groups::outermost(*d.elementById(e1.id)).isEmpty(), "undo takes the group off");
    undo->redo();
    canvas->selectElements({e1.id});
    canvas->ungroupSelected();
    canvas->selectElements({});
    QTest::mouseClick(vp, Qt::LeftButton, {}, at(30, 80));
    check(canvas->selectedElementIds() == QStringList{e1.id}, "after Ungroup a click picks one shape");
    canvas->selectElements({e1.id, e2.id});
    canvas->groupSelected();

    // Round trip through the file.
    const QString out = tmp.filePath("b.c2d");
    check(d.save(out, &err), "save");
    c2d::Document r;
    check(r.load(out, &err), "load");
    const c2d::Element *rb = r.elementById(b.id), *rc = r.elementById(c.id), *ra = r.elementById(a.id);
    check(rb && c2d::layers::isHidden(*rb) && !c2d::layers::isLocked(*rb), "hidden flag round-trips");
    check(rc && c2d::layers::isLocked(*rc) && !c2d::layers::isHidden(*rc), "locked flag round-trips");
    check(ra && ra->raw.value("layer").toObject().value("name").toString() == "Inlays",
          "layer name round-trips");
    const c2d::Element *r1 = r.elementById(e1.id), *r2 = r.elementById(e2.id);
    check(r1 && r2 && !c2d::groups::outermost(*r1).isEmpty()
              && c2d::groups::outermost(*r1) == c2d::groups::outermost(*r2), "a group round-trips");

    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
