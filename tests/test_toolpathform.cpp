// The toolpath settings form: designed fields per type, real editors, and
// edits that keep each value's JSON type (depth strings keep their sign).
//
// Plain asserts, no framework; exits 0 on success.

#include "../src/toolpathform.h"

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QJsonObject>
#include <QToolButton>

#include <cstdio>
#include <cstdlib>

static int g_checks = 0;

static void check(bool cond, const char *what)
{
    ++g_checks;
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

template <typename T>
static T *editorFor(QWidget *form, const QString &key)
{
    for (T *w : form->findChildren<T *>())
        if (w->toolTip().contains(QLatin1Char('(') + key + QLatin1Char(')')) || w->toolTip() == key)
            return w;
    return nullptr;
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    QJsonObject speeds{{"feedrate", 3000.0}, {"plungerate", 600.0}, {"rpm", 16000.0}};
    QJsonObject tool{{"number", 1106}, {"diameter", 10.0}, {"flutes", 2}, {"type", 0},
                     {"name", "Straight 2-flute end mill 10 mm"}};
    QJsonObject j{{"type", "pocket_toolpath"}, {"uuid", "{tp}"}, {"name", "Card recess"},
                  {"enabled", true}, {"start_depth", "-0.000"}, {"end_depth", "-5.000"},
                  {"stepdown", 2.5}, {"stepover", 4.0}, {"helix_entry", true},
                  {"helix_diameter", 10.0}, {"helix_angle", 2}, {"rest_auto", true},
                  {"rest_overlap", 1.0}, {"speeds", speeds}, {"tool", tool},
                  {"automatic_parameters", true}};

    c2d::ToolpathForm form;
    form.resize(360, 900);
    form.show();
    QJsonObject last;
    int edits = 0;
    QObject::connect(&form, &c2d::ToolpathForm::edited, [&](const QJsonObject &o) { last = o; ++edits; });
    form.setToolpath("{tp}", "pocket_toolpath", j);
    app.processEvents();

    // A switch, not a word to type.
    auto *helix = editorFor<QAbstractButton>(&form, "helix_entry");
    check(helix && helix->isCheckable() && helix->isChecked(), "helix entry is a switch, on");
    auto *helixD = editorFor<QDoubleSpinBox>(&form, "helix_diameter");
    check(helixD && helixD->isEnabled(), "helix diameter is live while helix is on");
    helix->click();
    check(edits == 1 && last.value("helix_entry").isBool() && !last.value("helix_entry").toBool(),
          "flipping the switch writes a bool false");
    check(!helixD->isEnabled(), "helix diameter greys out with the switch off");
    check(helixD->buttonSymbols() == QAbstractSpinBox::NoButtons, "no tiny spin arrows");
    {
        QToolButton *up = nullptr;
        for (QToolButton *b : helixD->parentWidget()->findChildren<QToolButton *>())
            if (b->text() == QStringLiteral("\u25b2"))
                up = b;
        check(up && up->width() >= 40 && !up->isEnabled(), "a big up button beside it, greyed with it");
    }

    // Depths: shown positive, written back as the file's negative strings.
    form.setToolpath("{tp}", "pocket_toolpath", last);
    auto *end = editorFor<QDoubleSpinBox>(&form, "end_depth");
    check(end && qAbs(end->value() - 5.0) < 1e-9 && end->suffix() == QLatin1String(" mm"),
          "end depth shows 5 mm");
    {
        QToolButton *up = nullptr;
        for (QToolButton *b : end->parentWidget()->findChildren<QToolButton *>())
            if (b->text() == QStringLiteral("\u25b2"))
                up = b;
        check(up != nullptr, "end depth has an up button");
        up->click();
        check(last.value("end_depth").toString() == QLatin1String("-5.100"), "up steps the depth by 0.1 mm");
    }
    end->setValue(8.0);
    check(last.value("end_depth").isString() && last.value("end_depth").toString() == QLatin1String("-8.000"),
          "end depth goes back as the string -8.000");

    // Speeds keep their nesting and number type.
    auto *feed = editorFor<QDoubleSpinBox>(&form, "speeds.feedrate");
    check(feed != nullptr, "feed has a field");
    feed->setValue(2500);
    check(last.value("speeds").toObject().value("feedrate").toDouble() == 2500.0
              && last.value("speeds").toObject().value("rpm").toDouble() == 16000.0,
          "feed edit keeps the other speeds");

    // Keys without a designed field are in Advanced, still typed.
    auto *autoParams = editorFor<QAbstractButton>(&form, "automatic_parameters");
    (void)autoParams;
    bool advanced = false;
    for (QAbstractButton *b : form.findChildren<QAbstractButton *>())
        if (b->text().startsWith(QLatin1String("ADVANCED")))
            advanced = true;
    check(advanced, "an Advanced card holds the other keys");

    // The same toolpath again: values update in place, editors survive.
    form.setToolpath("{tp}", "pocket_toolpath", last);
    check(editorFor<QDoubleSpinBox>(&form, "end_depth") == end, "a refresh keeps the editors");

    // 3D finish: the strategy is a choice, offered (as raster) even on a
    // toolpath saved before waterline existed, and written only on an edit.
    {
        QJsonObject f{{"type", "3d_finish_toolpath"}, {"uuid", "{f3d}"}, {"name", "3D Finish"},
                      {"enabled", true}, {"stepdown", 1.0}, {"stepover", 0.5},
                      {"direction", "zigzag"}, {"speeds", speeds}, {"tool", tool}};
        form.setToolpath("{f3d}", "3d_finish_toolpath", f);
        app.processEvents();
        auto *strategy = editorFor<QComboBox>(&form, "strategy");
        check(strategy && strategy->currentText() == QStringLiteral("Raster"),
              "3D finish: strategy offered, raster when absent");
        auto *direction = editorFor<QComboBox>(&form, "direction");
        check(direction && direction->currentText() == QStringLiteral("Zigzag"), "3D finish: direction is a choice");
        const int before = edits;
        strategy->setCurrentIndex(1);
        emit strategy->activated(1);
        check(edits == before + 1 && last.value("strategy").toString() == QLatin1String("waterline")
                  && last.value("stepdown").toDouble() == 1.0,
              "3D finish: choosing waterline writes strategy = waterline");
    }

    std::printf("test_toolpathform: %d checks OK\n", g_checks);
    return 0;
}
