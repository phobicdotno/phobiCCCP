// Typing a number with either decimal key. A number box follows the system's
// number format, and Qt drops the "wrong" one of "." and ",": on the shop PC
// (Norwegian numbers) a typed 3.175 became 317.
#include "../src/decimalkey.h"

#include <QApplication>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QtTest/QTest>
#include <cmath>
#include <cstdio>

static int g_fail = 0;
static void check(bool ok, const char *what)
{
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

static double typed(const QLocale &loc, const char *text, bool filter, bool groups = false)
{
    Q_UNUSED(filter);
    QDoubleSpinBox s;
    s.setLocale(loc);
    s.setDecimals(4);
    s.setRange(-100000, 100000);
    s.setGroupSeparatorShown(groups);
    s.setSuffix(QStringLiteral(" mm"));
    s.setValue(1.0);
    s.show();
    s.setFocus();
    s.selectAll();
    QTest::keyClicks(&s, QString::fromLatin1(text));
    QTest::keyClick(&s, Qt::Key_Return);
    return s.value();
}

static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    const QLocale nb(QLocale::NorwegianBokmal, QLocale::Norway), en(QLocale::English, QLocale::UnitedStates);
    check(nb.decimalPoint() == QLatin1String(","), "the Norwegian decimal sign is a comma");

    // What Qt does on its own, so the test says when that changes.
    check(!near(typed(nb, "3.175", false), 3.175), "without the filter a dot is lost with comma decimals");

    c2d::DecimalKey::install(&app);
    check(near(typed(nb, "3.175", true), 3.175), "comma decimals: 3.175 typed with a dot");
    check(near(typed(nb, "3,175", true), 3.175), "comma decimals: 3,175 typed with a comma");
    check(near(typed(nb, "-12.5", true), -12.5), "comma decimals: -12.5");
    check(near(typed(nb, "1.5875", true), 1.5875), "comma decimals: four decimals kept");
    check(near(typed(en, "3.175", true), 3.175), "dot decimals: 3.175 typed with a dot");
    check(near(typed(en, "3,175", true), 3.175), "dot decimals: 3,175 typed with a comma");
    check(near(typed(en, "1,234.5", true, true), 1234.5), "thousands shown: the comma stays a thousands sign");

    // Whole-number boxes are left alone.
    {
        QSpinBox n;
        n.setLocale(nb);
        n.setRange(0, 100000);
        n.show();
        n.setFocus();
        n.selectAll();
        QTest::keyClicks(&n, QStringLiteral("42"));
        QTest::keyClick(&n, Qt::Key_Return);
        check(n.value() == 42, "a whole-number box still takes its digits");
    }
    std::printf(g_fail ? "FAILED: %d\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
