// A value typed (not committed) into toolpath A's form, then Delete: it must
// not land in toolpath B, which the form shows next.
#include "../src/c2ddocument.h"
#include "../src/canvas.h"
#include "../src/element.h"
#include "../src/toolpathfactory.h"
#include "../src/toolpathpanel.h"
#include <QApplication>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QToolButton>
#include <QTemporaryDir>
#include <QTest>
#include <QJsonArray>
#include <cmath>
#include <cstdio>
static double num(const QJsonValue &v) { return v.isString() ? v.toString().toDouble() : v.toDouble(); }
int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QTemporaryDir tmp; QString err;
    const QString blank = tmp.filePath("a.c2d");
    c2d::Document::createBlank(blank, 200, 200, 19, &err);
    c2d::Document d; d.load(blank, &err);
    const c2d::Element r1 = c2d::Element::makeRectangle({50, 50}, 40, 40, d.defaultLayer());
    d.addElement(r1);
    c2d::Toolpath a = c2d::makeToolpath(d, "pocket_toolpath", {r1.id});
    c2d::Toolpath b = c2d::makeToolpath(d, "pocket_toolpath", {r1.id});
    d.insertToolpath(0, a); d.insertToolpath(1, b);
    c2d::Canvas canvas;
    c2d::ToolpathPanel panel(&canvas);
    canvas.setDocument(&d);
    panel.setDocument(&d);
    panel.resize(500, 1200);
    panel.show();
    QApplication::setActiveWindow(&panel);
    QTest::qWaitForWindowActive(&panel);
    QDoubleSpinBox *depth = nullptr;
    for (QDoubleSpinBox *s : panel.findChildren<QDoubleSpinBox *>())
        if (s->property("depth").toBool() && s->isVisible() && std::abs(s->value() - std::abs(num(a.json.value("end_depth")))) < 1e-6) { depth = s; break; }
    if (!depth) { std::printf("no depth field\n"); return 2; }
    depth->setFocus();
    QTest::qWait(20);
    QLineEdit *le = depth->findChild<QLineEdit *>();
    le->selectAll();
    QTest::keyClicks(le, "9.5");
    QToolButton *del = nullptr;
    for (QToolButton *t : panel.findChildren<QToolButton *>()) if (t->text() == "Delete") del = t;
    QTest::mouseClick(del, Qt::LeftButton);
    QTest::qWait(50);
    QWidget *f = QApplication::focusWidget();
    if (f) f->clearFocus();
    QTest::qWait(50);
    const c2d::Toolpath *bb = d.toolpathByUuid(b.uuid);
    const bool ok = bb && std::abs(num(bb->json.value("end_depth")) - num(b.json.value("end_depth"))) < 1e-9;
    std::printf("%s Delete with a value still typed leaves the next toolpath's depth alone\n", ok ? "ok  " : "FAIL");
    return ok ? 0 : 1;
}
