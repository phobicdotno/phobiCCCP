// About dialog: the changelog summary and the dialog itself (offscreen).
#include "../src/aboutdialog.h"

#include <QApplication>
#include <QFile>
#include <cstdio>

static int g_fail = 0;
static void check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && qEnvironmentVariableIsEmpty("DISPLAY")
        && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    const auto r = c2d::summarizeChangelog(QStringLiteral(
        "# Changelog\n\n## v1.2.3 (build 7) — 2026-01-02\n\n**Files**\n- a\n\n**Testing**\n- b\n\n"
        "**Look**\n- c\n\n## v1.2.2 — 2026-01-01\n\n**Files**\n- d\n"));
    check(r.size() == 2, "one entry per release heading");
    check(r.size() == 2 && r[0].version == QLatin1String("1.2.3") && r[0].build == 7
              && r[0].date == QLatin1String("2026-01-02"), "version, build and date parsed");
    check(r.size() == 2 && r[0].areas == QStringList({QStringLiteral("Files"), QStringLiteral("Look")}),
          "section titles kept, Testing dropped");
    check(r.size() == 2 && r[1].build == 0 && r[1].areas.size() == 1, "heading without build");

    QFile f(QStringLiteral(":/data/CHANGELOG.md"));
    check(f.open(QIODevice::ReadOnly), "changelog compiled in");
    const auto real = c2d::summarizeChangelog(QString::fromUtf8(f.readAll()));
    check(!real.isEmpty() && real.first().version == QLatin1String(PHOBICCCP_VERSION),
          "newest changelog entry matches the build's version");

    c2d::AboutDialog dlg;
    dlg.show();
    app.processEvents();
    if (argc > 1)
        dlg.grab().save(QString::fromLocal8Bit(argv[1]));
    check(dlg.isVisible(), "dialog opens");
    return g_fail ? 1 : 0;
}
