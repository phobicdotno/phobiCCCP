// The touch profile (touchprofile.h) on a pad-sized screen: with it on, the
// main window's bars, tabs, menus, the mode switch and the everyday controls
// are finger targets (44 px at least, the profile aims at 48), the main window
// still fits a 1280x800 landscape pad, and a dialog taller than the screen
// gets a scroll area with OK / Cancel outside it. With it off, the desktop
// sizes are untouched.
//
// Run offscreen on a 1280x800 screen (see CMakeLists.txt). Plain asserts, no
// framework; exits 0 on success.

#include "../src/apptheme.h"
#include "../src/c2ddocument.h"
#include "../src/canvas.h"
#include "../src/mainwindow.h"
#include "../src/toolpathform.h"
#include "../src/touchprofile.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QJsonObject>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QStyleOptionSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>

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

static void checkSize(const char *what, QSize got, int minW, int minH)
{
    ++g_checks;
    if (got.width() < minW || got.height() < minH) {
        std::fprintf(stderr, "FAIL: %s is %dx%d, wants at least %dx%d\n", what, got.width(), got.height(),
                     minW, minH);
        std::exit(1);
    }
}

// What a finger needs at least (the profile's own target is 48).
constexpr int kFinger = 44;

static void testDesktopUnchanged()
{
    c2d::touch::setEnabled(false);
    check(c2d::touch::iconSize(24) == QSize(24, 24), "off: icons keep their desktop size");
    check(c2d::touch::minSide(34) == 34, "off: buttons keep their desktop size");
    QSettings s;
    check(c2d::touch::mode(s) == c2d::touch::Mode::Automatic, "nothing saved reads as Automatic");
    check(!c2d::touch::wantedBy(c2d::touch::Mode::Automatic), "Automatic is off on a computer");
    check(c2d::touch::wantedBy(c2d::touch::Mode::On), "On is on");
    c2d::touch::setMode(s, c2d::touch::Mode::On);
    check(c2d::touch::mode(s) == c2d::touch::Mode::On, "the choice is kept");
    c2d::touch::setMode(s, c2d::touch::Mode::Automatic);
    c2d::touch::setEnabled(true);
    check(c2d::touch::iconSize(24).width() >= 32, "on: icons grow");
    check(c2d::touch::minSide(34) >= 48, "on: buttons reach the target");
}

static void testMainWindow()
{
    c2d::MainWindow w;
    // Nothing in the window may ask for more than the pad has: 1280x800 dp,
    // less Android's status bar (24 dp) and navigation bar (48 dp).
    const char *pages[] = {"Design", "Toolpaths", "Simulate", "Machine"};
    for (int m = 0; m < 4; ++m) {
        w.setMode(c2d::MainWindow::Mode(m), false);
        const QSize min = w.minimumSizeHint().expandedTo(w.minimumSize());
        std::printf("%s page: main window minimum %dx%d\n", pages[m], min.width(), min.height());
        check(min.width() <= 1280 && min.height() <= 800 - 24 - 48, "the main window's minimum fits the pad");
    }
    w.setMode(c2d::MainWindow::Mode::Design, false);
    w.resize(1280, 800);
    w.show();
    QTest::qWait(200);
    check(w.width() <= 1280 && w.height() <= 800, "the main window stays at 1280x800");

    // Every button on the top bars is a finger target.
    int buttons = 0;
    for (QToolBar *bar : w.findChildren<QToolBar *>(QString(), Qt::FindDirectChildrenOnly)) {
        if (!bar->isVisible())
            continue;
        check(bar->iconSize().width() >= 26, "touch-sized icons on the bars");
        for (QToolButton *b : bar->findChildren<QToolButton *>()) {
            if (!b->isVisible() || b->objectName() == QLatin1String("qt_toolbar_ext_button"))
                continue;
            checkSize(qPrintable(QStringLiteral("tool bar button \"%1\" on %2")
                                     .arg(b->text(), bar->windowTitle())),
                      b->size(), 40, kFinger);
            ++buttons;
        }
    }
    std::printf("%d tool bar buttons checked\n", buttons);
    check(buttons > 20, "the drawing tools and options were found");

    // The mode switch at the top right.
    int modes = 0;
    if (QWidget *sw = w.findChild<QWidget *>(QStringLiteral("modeSwitch")))
        for (QToolButton *b : sw->findChildren<QToolButton *>()) {
            checkSize("mode switch button", b->size(), kFinger, kFinger);
            ++modes;
        }
    check(modes == 4, "four mode buttons");

    // The menu bar and the File menu's rows.
    QMenuBar *bar = w.findChild<QMenuBar *>();
    check(bar, "menu bar");
    QMenu *file = nullptr;
    for (QAction *a : bar->actions()) {
        checkSize("menu bar entry", bar->actionGeometry(a).size(), kFinger, kFinger);
        if (!file && a->menu())
            file = a->menu();
    }
    check(file, "a menu");
    file->ensurePolished();
    file->adjustSize();
    for (QAction *a : file->actions())
        if (!a->isSeparator() && a->isVisible())
            checkSize("menu row", file->actionGeometry(a).size(), 100, kFinger);

    // Finish and Cancel stand in for Enter and Esc while drawing.
    auto *canvas = w.findChild<c2d::Canvas *>();
    QAction *finish = w.findChild<QAction *>(QStringLiteral("touchFinish"));
    QAction *cancel = w.findChild<QAction *>(QStringLiteral("touchCancel"));
    check(canvas && finish && cancel, "Finish and Cancel on the Options bar");
    canvas->setTool(c2d::Canvas::Select);
    check(!finish->isVisible() && !cancel->isVisible(), "not while selecting");
    canvas->setTool(c2d::Canvas::DrawPath);
    check(finish->isVisible() && cancel->isVisible(), "both while drawing a path");
    if (w.newUntitled(200, 200, 10)) {
        const int before = canvas->document()->elements().size();
        QWidget *vp = canvas->viewport();
        const QPoint c = vp->rect().center();
        for (QPoint p : {c - QPoint(60, 0), c + QPoint(60, 0), c + QPoint(60, 60)}) {
            QTest::mousePress(vp, Qt::LeftButton, {}, p);
            QTest::mouseRelease(vp, Qt::LeftButton, {}, p);
            QTest::qWait(10);
        }
        finish->trigger();
        check(canvas->document()->elements().size() == before + 1, "Finish ends the path, as Enter does");
    }
    canvas->setTool(c2d::Canvas::Select);

    // The Design page's tabs.
    for (QTabWidget *tabs : w.findChildren<QTabWidget *>())
        if (tabs->isVisible() && tabs->count() > 1)
            checkSize("tab", tabs->tabBar()->tabRect(0).size(), kFinger, kFinger);
}

static void testControls()
{
    QWidget host;
    QSpinBox spin(&host);
    QDoubleSpinBox dspin(&host);
    dspin.setSuffix(QStringLiteral(" mm"));
    dspin.setRange(-10000, 10000);
    QComboBox combo(&host);
    combo.addItems({QStringLiteral("one"), QStringLiteral("two")});
    QCheckBox box(QStringLiteral("Tick"), &host);
    QPushButton push(QStringLiteral("Press"), &host);
    QScrollBar scroll(Qt::Vertical, &host);
    QTreeWidget tree(&host);
    new QTreeWidgetItem(&tree, {QStringLiteral("row")});
    tree.resize(200, 200);
    host.show();
    QTest::qWait(50);

    // What a layout gives it: its size hint, or more when it asks for room.
    auto laidOut = [](QWidget &w) { return w.sizeHint().expandedTo(w.minimumSize()); };
    // Its arrows are two finger-wide buttons, side by side.
    for (QAbstractSpinBox *sb : {static_cast<QAbstractSpinBox *>(&spin), static_cast<QAbstractSpinBox *>(&dspin)}) {
        checkSize("spin box", laidOut(*sb), 100, kFinger);
        sb->resize(laidOut(*sb));
        QStyleOptionSpinBox opt;
        opt.initFrom(sb);
        opt.rect = sb->rect();
        opt.frame = true;
        opt.buttonSymbols = QAbstractSpinBox::UpDownArrows;
        opt.subControls = QStyle::SC_All;
        const QRect up = sb->style()->subControlRect(QStyle::CC_SpinBox, &opt, QStyle::SC_SpinBoxUp, sb);
        const QRect down = sb->style()->subControlRect(QStyle::CC_SpinBox, &opt, QStyle::SC_SpinBoxDown, sb);
        checkSize("spin box + button", up.size(), 32, 40);
        checkSize("spin box - button", down.size(), 32, 40);
        check(!up.intersects(down), "+ and - apart");
        // ... and the number keeps room beside them.
        const QRect field = sb->style()->subControlRect(QStyle::CC_SpinBox, &opt, QStyle::SC_SpinBoxEditField, sb);
        check(field.width() >= sb->fontMetrics().horizontalAdvance(sb->text()), "the number is not cut");
    }
    checkSize("combo box", combo.sizeHint(), 40, kFinger);
    checkSize("check box", box.sizeHint(), 40, kFinger);
    checkSize("push button", push.size(), 60, kFinger);
    check(box.style()->pixelMetric(QStyle::PM_IndicatorWidth, nullptr, &box) >= 24, "big tick box");
    check(scroll.sizeHint().width() >= 28, "finger-wide scroll bar");
    check(tree.visualItemRect(tree.topLevelItem(0)).height() >= 40, "list rows a finger high");
}

// The toolpath settings form, as wide as the Toolpaths side on the pad:
// every field and switch a finger high.
static void testToolpathForm()
{
    QJsonObject tool{{"number", 1106}, {"diameter", 10.0}, {"flutes", 2}, {"type", 0},
                     {"name", "Straight 2-flute end mill 10 mm"}};
    QJsonObject j{{"type", "pocket_toolpath"}, {"uuid", "{tp}"}, {"name", "Card recess"},
                  {"enabled", true}, {"start_depth", "-0.000"}, {"end_depth", "-5.000"},
                  {"stepdown", 2.5}, {"stepover", 4.0}, {"helix_entry", true},
                  {"helix_diameter", 10.0}, {"helix_angle", 2}, {"tool", tool}};
    c2d::ToolpathForm form;
    form.resize(380, 1400);
    form.setToolpath("{tp}", "pocket_toolpath", j);
    form.show();
    QTest::qWait(50);
    int fields = 0;
    for (QWidget *f : form.findChildren<QWidget *>()) {
        if (!f->isVisible() || !(qobject_cast<QAbstractSpinBox *>(f) || qobject_cast<QComboBox *>(f)
                                 || qobject_cast<QLineEdit *>(f) || qobject_cast<QAbstractButton *>(f)))
            continue;
        if (qobject_cast<QLineEdit *>(f) && qobject_cast<QAbstractSpinBox *>(f->parentWidget()))
            continue;   // the text inside a spin box
        checkSize(qPrintable(QStringLiteral("toolpath form %1 %2").arg(f->metaObject()->className(), f->toolTip().left(40))),
                  f->size(), 24, 40);
        ++fields;
    }
    check(fields > 5, "the form's fields were found");
    if (qEnvironmentVariableIsSet("TOUCHSIZE_SHOTS"))
        form.grab().save(qEnvironmentVariable("TOUCHSIZE_SHOTS") + QStringLiteral("/toolpath-form.png"));
}

static void testDialogFits()
{
    const QRect avail = QGuiApplication::primaryScreen()->availableGeometry();
    std::printf("screen %dx%d\n", avail.width(), avail.height());

    // A long form: far taller than the screen.
    QDialog dlg;
    auto *form = new QFormLayout(&dlg);
    for (int i = 0; i < 40; ++i)
        form->addRow(QStringLiteral("Value %1").arg(i), new QDoubleSpinBox);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    dlg.show();
    QTest::qWait(50);
    check(dlg.height() <= avail.height(), "a long dialog is no taller than the screen");
    auto *area = dlg.findChild<QScrollArea *>(QStringLiteral("phobicFitScroll"));
    check(area, "its form scrolls");
    check(!area->isAncestorOf(buttons) && buttons->isVisible(), "OK / Cancel stay in sight, outside the scroll");
    dlg.close();

    // A size asked for in code is cut to the screen.
    QDialog big;
    big.setMinimumSize(1600, 1000);
    big.resize(1700, 1100);
    big.show();
    QTest::qWait(50);
    check(big.width() <= avail.width() && big.height() <= avail.height(), "a forced size is cut to the screen");
    big.close();

    // A dialog that fits is left as it is.
    QDialog small;
    auto *v = new QFormLayout(&small);
    v->addRow(QStringLiteral("One"), new QSpinBox);
    small.show();
    QTest::qWait(50);
    check(!small.findChild<QScrollArea *>(), "a dialog that fits gets no scroll area");
}

int main(int argc, char **argv)
{
    // Its own settings, so the window starts the same on every machine.
    QTemporaryDir home;
    qputenv("XDG_CONFIG_HOME", home.path().toLocal8Bit());
    qputenv("XDG_CACHE_HOME", (home.path() + QStringLiteral("/cache")).toLocal8Bit());
    qputenv("XDG_DATA_HOME", (home.path() + QStringLiteral("/data")).toLocal8Bit());
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("phobicdotno-test"));
    QCoreApplication::setApplicationName(QStringLiteral("touchsize"));

    testDesktopUnchanged();
    c2d::touch::setEnabled(true);
    c2d::applyAppTheme(app);
    c2d::touch::install(app);
    testControls();
    testDialogFits();
    testToolpathForm();
    testMainWindow();
    std::printf("touchsize: %d checks passed\n", g_checks);
    return 0;
}
