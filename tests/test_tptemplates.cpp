// Toolpath templates: save two toolpaths' settings without their shapes,
// apply them to another design with a rectangle selected, and the file round
// trip. Plain asserts; exits 0 on success.

#include "../src/c2ddocument.h"
#include "../src/canvas.h"
#include "../src/element.h"
#include "../src/parametric.h"
#include "../src/toolpathfactory.h"
#include "../src/toolpathpanel.h"
#include "../src/toolpathtemplates.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUndoStack>

#include <cstdio>
#include <cstdlib>

using namespace c2d;

static void check(bool cond, const char *what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

static QJsonObject withoutRefs(QJsonObject j)
{
    for (const char *k : {"uuid", "elements", "toolpath_group", "name", "phobic_expr"})
        j.remove(QLatin1String(k));
    return j;
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("phobicccp-test-tptemplates"));
    QDir(tptemplates::folder()).removeRecursively();

    QTemporaryDir tmp;
    QString err;
    const QString aPath = tmp.filePath(QStringLiteral("a.c2d"));
    const QString bPath = tmp.filePath(QStringLiteral("b.c2d"));
    check(Document::createBlank(aPath, 200, 150, 18, &err), "blank design a");
    check(Document::createBlank(bPath, 300, 200, 12, &err), "blank design b");

    // Design A: a circle, an inside pocket and an outside contour, one depth
    // bound to a parameter A has and B lacks, one to a built-in.
    Document a;
    check(a.load(aPath, &err), "a loads");
    a.addElement(Element::makeCircle(QPointF(50, 50), 20, QJsonObject()));
    const QString circle = a.elements().last().id;
    Parametric pa = a.parametric();
    pa.params << UserParameter{QStringLiteral("pocket_d"), QStringLiteral("4"), QString()};
    a.setParametric(pa);
    Toolpath pocket = makeToolpath(a, QStringLiteral("pocket_toolpath"), {circle});
    pocket.json.insert(QStringLiteral("end_depth"), QStringLiteral("4.000"));
    pocket.json.insert(QStringLiteral("phobic_expr"), QJsonObject{
        {QStringLiteral("end_depth"), QJsonObject{{QStringLiteral("expr"), QStringLiteral("pocket_d")}}},
        {QStringLiteral("stepdown"), QJsonObject{{QStringLiteral("expr"), QStringLiteral("stock_t / 10")}}}});
    a.addToolpath(pocket);
    Toolpath chamfer = makeToolpath(a, QStringLiteral("chamfer_toolpath"), {circle});
    chamfer.json.insert(QStringLiteral("side"), QStringLiteral("inside"));
    chamfer.json.insert(QStringLiteral("chamfer_width"), 1.75);
    a.addToolpath(chamfer);

    Canvas canvas;
    ToolpathPanel panel(&canvas);
    canvas.setDocument(&a);
    panel.setDocument(&a);
    check(panel.saveTemplate(QStringLiteral("Lid: pocket + chamfer"), {pocket.uuid, chamfer.uuid}, &err),
          "template saves");
    check(QFile::exists(tptemplates::fileFor(QStringLiteral("Lid: pocket + chamfer"))),
          "one json file in the templates folder");
    check(tptemplates::names() == QStringList{QStringLiteral("Lid: pocket + chamfer")},
          "listed under its own name");

    // File round trip: no shapes, every setting.
    QJsonObject tpl;
    check(tptemplates::load(QStringLiteral("Lid: pocket + chamfer"), &tpl, &err), "template loads");
    const QJsonArray saved = tpl.value("toolpaths").toArray();
    check(saved.size() == 2, "two toolpaths saved");
    for (const QJsonValue &v : saved) {
        const QJsonObject j = v.toObject().value("json").toObject();
        check(!j.contains("elements") && !j.contains("uuid") && !j.contains("toolpath_group"),
              "no shape references in the file");
    }
    QJsonObject want = pocket.json;
    for (const char *k : {"uuid", "elements", "toolpath_group"})
        want.remove(QLatin1String(k));
    check(saved.at(0).toObject().value("json").toObject() == want,
          "the pocket's settings come back as saved");

    // Design B, a rectangle selected: apply.
    Document b;
    check(b.load(bPath, &err), "b loads");
    b.addElement(Element::makeRectangle(QPointF(100, 80), 60, 40, QJsonObject()));
    const QString rect = b.elements().last().id;
    canvas.setDocument(&b);
    panel.setDocument(&b);
    canvas.selectElements({rect});
    const int before = canvas.undoStack()->count();
    QStringList notes;
    check(panel.applyTemplate(QStringLiteral("Lid: pocket + chamfer"), &notes, &err) == 2,
          "two toolpaths added");
    check(b.toolpaths().size() == 2, "the design has them");
    check(canvas.undoStack()->count() == before + 1, "applying is one undo step");
    const Toolpath &p2 = b.toolpaths().at(0);
    const Toolpath &c2 = b.toolpaths().at(1);
    check(p2.type == pocket.type && c2.type == chamfer.type, "types equal");
    check(withoutRefs(p2.json) == withoutRefs(pocket.json), "pocket params equal");
    check(withoutRefs(c2.json) == withoutRefs(chamfer.json), "chamfer params equal");
    check(c2.json.value("side").toString() == QLatin1String("inside"), "the chamfer keeps its side");
    for (const Toolpath *t : {&p2, &c2}) {
        const QJsonArray els = t->json.value("elements").toArray();
        check(els.size() == 1 && els.at(0).toObject().value("uuid").toString() == rect,
              "bound to the selected rectangle");
        check(t->uuid != pocket.uuid && t->uuid != chamfer.uuid, "fresh uuids");
        check(t->json.value("toolpath_group").toString() == b.defaultToolpathGroup(),
              "in the design's own group");
    }
    const QJsonObject binds = p2.json.value("phobic_expr").toObject();
    check(!binds.contains("end_depth") && binds.contains("stepdown"),
          "the binding to a missing parameter goes, the built-in one stays");
    check(p2.json.value("end_depth").toString() == QLatin1String("4.000"), "and its number stays");
    check(notes.size() == 1 && notes.first().contains("pocket_d"), "the missing parameter is named");

    canvas.undoStack()->undo();
    check(b.toolpaths().isEmpty(), "one undo takes both away");
    canvas.undoStack()->redo();
    check(b.toolpaths().size() == 2, "redo brings both back");

    // Nothing selected: added without shapes. Names do not clash.
    canvas.selectElements({});
    check(panel.applyTemplate(QStringLiteral("Lid: pocket + chamfer"), nullptr, &err) == 2,
          "applies again");
    check(b.toolpaths().at(2).json.value("elements").toArray().isEmpty(), "no shapes when none selected");
    check(b.toolpaths().at(2).json.value("name").toString()
              != b.toolpaths().at(0).json.value("name").toString(), "a new name");

    check(tptemplates::remove(QStringLiteral("Lid: pocket + chamfer")), "delete");
    check(tptemplates::names().isEmpty(), "gone from the list");
    QDir(tptemplates::folder()).removeRecursively();
    std::printf("tptemplates: all checks passed\n");
    return 0;
}
