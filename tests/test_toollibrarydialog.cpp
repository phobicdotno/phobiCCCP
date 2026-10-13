// Tool library popup: the bit-kind icons along the top sort the list, and
// collections (My Tools, Unused, saved to / loaded from a file) narrow it.
#include "../src/toollibrarydialog.h"
#include "../src/toollibrary.h"

#include <QApplication>
#include <QComboBox>
#include <QTemporaryDir>
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
    QSettings().remove(QStringLiteral("ui/toolCollection"));
    QSettings().remove(QStringLiteral("tools/collections"));

    // The collections model on its own.
    {
        c2d::ToolCollections c = c2d::ToolCollections::defaults();
        check(c.all().size() == 2 && c.all()[0].name == QLatin1String("My Tools")
                  && c.all()[1].name == QLatin1String("Unused"),
              "new install starts with My Tools and Unused");
        check(!c.add(QStringLiteral(" My Tools ")) && !c.add(QString()), "no empty or repeated names");
        c.addTool(QStringLiteral("My Tools"), 201);
        c.addTool(QStringLiteral("My Tools"), 102);
        c.addTool(QStringLiteral("My Tools"), 201);
        check(c.byName(QStringLiteral("My Tools"))->tools == QList<int>({201, 102}),
              "bits kept in the order added, once each");
        check(c.rename(QStringLiteral("Unused"), QStringLiteral("Spares"))
                  && !c.rename(QStringLiteral("Spares"), QStringLiteral("My Tools")),
              "rename, but not onto a taken name");
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("c.json"));
        check(c.saveFile(path), "collections save to a file");
        c2d::ToolCollections back;
        check(c2d::ToolCollections::loadFile(path, &back)
                  && back.toJson() == c.toJson(), "and load back the same");
        c2d::ToolCollections bad;
        check(!c2d::ToolCollections::fromJson("{\"tools\": []}", &bad), "a file that isn't collections is refused");
        c2d::ToolCollections mine = c2d::ToolCollections::defaults();
        mine.merge(back);
        check(mine.all().size() == 3 && mine.byName(QStringLiteral("My Tools"))->tools.size() == 2
                  && mine.byName(QStringLiteral("Unused")) && mine.byName(QStringLiteral("Spares")),
              "loading replaces same-named collections and adds the rest");
    }

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

    // Collections in the popup: pick My Tools, it lists only its bits.
    {
        c2d::ToolCollections c = c2d::ToolCollections::defaults();
        c.addTool(QStringLiteral("My Tools"), 301);
        c.addTool(QStringLiteral("My Tools"), 201);
        c.saveSettings();
        c2d::ToolLibraryDialog dlg(lib, QString(), -1);
        auto *list = dlg.findChild<QListWidget *>();
        QComboBox *coll = nullptr;
        for (QComboBox *b : dlg.findChildren<QComboBox *>())
            if (b->findData(QStringLiteral("My Tools")) >= 0)
                coll = b;
        check(coll && coll->count() == 3, "collection picker: All bits, My Tools, Unused");
        if (!coll)
            return 1;
        check(list->count() == lib.tools().size(), "All bits shown at first");
        coll->setCurrentIndex(coll->findData(QStringLiteral("My Tools")));
        check(list->count() == 2 && list->item(0)->data(Qt::UserRole).toInt() == 301
                  && list->item(1)->data(Qt::UserRole).toInt() == 201,
              "My Tools lists its two bits, in the order added");
        coll->setCurrentIndex(coll->findData(QStringLiteral("Unused")));
        check(list->count() == 0, "an empty collection lists nothing");
    }
    {
        c2d::ToolLibraryDialog dlg(lib, QString(), -1);
        auto *list = dlg.findChild<QListWidget *>();
        check(list->count() == 0, "the shown collection comes back next time");
    }
    QSettings().remove(QStringLiteral("ui/toolCollection"));
    QSettings().remove(QStringLiteral("tools/collections"));
    return g_fail ? 1 : 0;
}
