// Tool library popup: the bit-kind icons along the top sort the list.
#include "../src/toollibrarydialog.h"
#include "../src/toollibrary.h"

#include <QApplication>
#include <QListWidget>
#include <QSet>
#include <QSettings>
#include <QToolButton>
#include <cstdio>

static int g_fail = 0;
static void check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

// The kinds of the listed bits, in list order, one entry per run.
static QStringList kindRuns(const c2d::ToolLibrary &lib, QListWidget *list)
{
    QStringList runs;
    for (int i = 0; i < list->count(); ++i) {
        const auto *t = lib.byNumber(list->item(i)->data(Qt::UserRole).toInt());
        if (t && (runs.isEmpty() || runs.last() != t->kind))
            runs << t->kind;
    }
    return runs;
}

static QToolButton *kindButton(QDialog &dlg, const QString &kind)
{
    for (QToolButton *b : dlg.findChildren<QToolButton *>())
        if (b->property("kind").toString() == kind)
            return b;
    return nullptr;
}

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && qEnvironmentVariableIsEmpty("DISPLAY")
        && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("phobicccp-test"));
    QApplication::setApplicationName(QStringLiteral("test_toollibrarydialog"));
    QSettings().remove(QStringLiteral("ui/toolKinds"));

    c2d::ToolLibrary lib;
    check(lib.loadDefault(), "embedded library loads");
    {
        c2d::ToolLibraryDialog dlg(lib, QString(), -1);
        auto *list = dlg.findChild<QListWidget *>();
        check(list && list->count() == lib.tools().size(), "nothing pressed lists every bit");
        const QStringList runs = list ? kindRuns(lib, list) : QStringList();
        check(runs.size() == QSet<QString>(runs.begin(), runs.end()).size(), "nothing pressed groups the bits by kind");

        QToolButton *v = kindButton(dlg, QStringLiteral("vbit"));
        QToolButton *ball = kindButton(dlg, QStringLiteral("ball"));
        check(v && ball, "one icon per bit kind");
        if (!v || !ball || !list)
            return 1;
        v->click();
        check(kindRuns(lib, list) == QStringList{QStringLiteral("vbit")}, "V-bit pressed: only V-bits");
        ball->click();
        check(kindRuns(lib, list) == QStringList({QStringLiteral("vbit"), QStringLiteral("ball")}),
              "then ball: V-bits first, balls after");
    }
    {
        c2d::ToolLibraryDialog dlg(lib, QString(), -1);
        auto *list = dlg.findChild<QListWidget *>();
        check(kindRuns(lib, list) == QStringList({QStringLiteral("vbit"), QStringLiteral("ball")}),
              "the pressed kinds come back next time");
        kindButton(dlg, QStringLiteral("vbit"))->click();
        check(kindRuns(lib, list) == QStringList{QStringLiteral("ball")}, "pressing again unmarks it");
        kindButton(dlg, QStringLiteral("ball"))->click();
        check(list->count() == lib.tools().size(), "all unmarked lists every bit again");
        if (argc > 1) {                     // screenshot with V-bit and ball pressed
            kindButton(dlg, QStringLiteral("vbit"))->click();
            kindButton(dlg, QStringLiteral("ball"))->click();
            dlg.show();
            app.processEvents();
            dlg.grab().save(QString::fromLocal8Bit(argv[1]));
        }
    }
    QSettings().remove(QStringLiteral("ui/toolKinds"));
    return g_fail ? 1 : 0;
}
