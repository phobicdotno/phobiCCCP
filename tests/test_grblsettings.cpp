// Controller settings ($$) editor: names and units of GRBL 1.1 settings,
// Save sends only the changed values, a backup file gives the differences.
//
// Plain asserts, no framework; exits 0 on success.

#include "../src/grblsettingsdialog.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

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

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    // A Shapeoko-like listing, with one grblHAL extra.
    QMap<int, double> s{{0, 10}, {13, 0}, {22, 1}, {100, 40}, {110, 5000}, {111, 5000},
                        {130, 838}, {341, 2}};
    GrblSettingsDialog dlg(s, QStringLiteral("Grbl 1.1h ['$' for help]"));
    const int r110 = dlg.rowOf(110);
    check(r110 >= 0, "$110 listed");
    check(dlg.cellText(r110, 0) == QLatin1String("$110"), "$110 number cell");
    check(dlg.cellText(r110, 1) == QLatin1String("X max rate"), "$110 name");
    check(dlg.cellText(r110, 2) == QLatin1String("5000"), "$110 value");
    check(dlg.cellText(r110, 3) == QLatin1String("mm/min"), "$110 unit");
    check(!dlg.cellText(r110, 4).isEmpty(), "$110 description");
    const int r341 = dlg.rowOf(341);
    check(r341 >= 0 && dlg.cellText(r341, 1) == QLatin1String("unknown"), "$341 unknown");
    check(grblsettings::info(100).unit == QLatin1String("steps/mm"), "$100 unit");
    check(grblsettings::info(120).unit == QString::fromUtf8("mm/s²"), "$120 unit");
    check(grblsettings::isRisky(22) && grblsettings::isRisky(101) && grblsettings::isRisky(132)
              && !grblsettings::isRisky(110), "risky set");

    // Nothing changed: nothing to send.
    check(dlg.pendingLines().isEmpty(), "no changes at start");
    dlg.setEdited(111, 5000);              // the same value is not a change
    check(dlg.pendingLines().isEmpty(), "same value is no change");

    // Edit $110 and Save: exactly one line.
    QSignalSpy spy(&dlg, &GrblSettingsDialog::sendRequested);
    dlg.setEdited(110, 6000);
    dlg.save();
    check(spy.count() == 1, "Save emitted once");
    const QStringList lines = spy.at(0).at(0).toStringList();
    check(lines == QStringList{QStringLiteral("$110=6000")}, "Save sends exactly $110=6000");
    check(grblsettings::formatValue(0.01) == QLatin1String("0.01"), "decimals kept");
    check(grblsettings::formatValue(40.0) == QLatin1String("40"), "no trailing zeros");

    // A fresh $$ drops the edits.
    s.insert(110, 6000);
    dlg.setSettings(s);
    check(dlg.pendingLines().isEmpty(), "re-read clears edits");
    check(dlg.cellText(dlg.rowOf(110), 2) == QLatin1String("6000"), "re-read shows new value");

    // Backup round trip and restore diff.
    QTemporaryDir tmp;
    check(tmp.isValid(), "temp dir");
    QMap<int, double> backup = s;
    backup.insert(110, 4000);
    backup.insert(100, 40.25);
    const QByteArray json = grblsettings::toJson(QStringLiteral("Grbl 1.1h"),
                                                 QDateTime::currentDateTime(), backup);
    QMap<int, double> back;
    QString banner, why;
    check(grblsettings::fromJson(json, &back, &banner, &why), "backup reads back");
    check(banner == QLatin1String("Grbl 1.1h") && back == backup, "backup same values");
    check(!grblsettings::fromJson("{}", &back, nullptr, &why) && !why.isEmpty(), "bad file refused");
    const QStringList diff = grblsettings::describeChanges(s, back);
    check(diff.size() == 2, "two differences");
    check(diff.at(0) == QString::fromUtf8("$100 X steps/mm: 40 → 40.25"), "diff $100");
    check(diff.at(1) == QString::fromUtf8("$110 X max rate: 6000 → 4000"), "diff $110");
    check(dlg.loadBackup(back), "restore has changes");
    check(dlg.pendingLines() == (QStringList{QStringLiteral("$100=40.25"), QStringLiteral("$110=4000")}),
          "restore sends the changed ones");

    // Homing ($22) goes before soft limits ($20), which GRBL refuses while homing is off.
    QMap<int, double> off = s, on = s;
    off[20] = 0; off[22] = 0; on[20] = 1; on[22] = 1;
    const QStringList order = grblsettings::changeLines(off, on);
    check(order.size() == 2 && order.at(0) == QLatin1String("$22=1") && order.at(1) == QLatin1String("$20=1"),
          "homing before soft limits");

    // Daily automatic copy: once per profile and day.
    const QString dir = tmp.filePath(QStringLiteral("grbl-backups"));
    const QString p1 = grblsettings::autoBackup(dir, QStringLiteral("Shapeoko 5/Pro"),
                                                QDate(2026, 11, 26), banner, s);
    check(p1.endsWith(QLatin1String("Shapeoko_5_Pro-2026-11-26.json")) && QFile::exists(p1),
          "auto backup written");
    check(grblsettings::autoBackup(dir, QStringLiteral("Shapeoko 5/Pro"), QDate(2026, 11, 26),
                                   banner, s).isEmpty(), "second read that day keeps the first");
    QFile f(p1);
    check(f.open(QIODevice::ReadOnly) && grblsettings::fromJson(f.readAll(), &back) && back == s,
          "auto backup readable");

    std::printf("grblsettings: %d checks passed\n", g_checks);
    return 0;
}
