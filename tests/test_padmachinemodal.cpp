// A pad's machine switch while a dialog is open here: Preferences holds the
// old machine's settings and would write them into the new one on OK, so the
// switch is refused until the dialog closes.
//
// Plain checks, no framework; exits 0 on success.
#define private public
#include "../src/mainwindow.h"
#undef private
#include "../src/machineprofiles.h"

#include <QApplication>
#include <QDialog>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cstdio>

static int g_fails = 0;
static void check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fails;
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir home;
    qputenv("HOME", home.path().toLocal8Bit());
    qunsetenv("XDG_DOCUMENTS_DIR");
    qunsetenv("XDG_DATA_HOME");
    qunsetenv("XDG_CONFIG_HOME");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("phobicccp-test"));
    QCoreApplication::setApplicationName(QStringLiteral("test_padmachinemodal"));
    QSettings().clear();
    QString a, b;
    {
        QSettings s;
        a = c2d::machineprofiles::activeId(s);
        b = c2d::machineprofiles::add(s, QStringLiteral("B"));
        c2d::machineprofiles::activate(s, a);
    }

    c2d::MainWindow w;
    w.resize(1200, 800);
    w.show();
    w.newUntitled(300, 200, 19);
    QApplication::processEvents();

    auto active = [] { QSettings s; return c2d::machineprofiles::activeId(s); };
    QDialog prefs(&w);
    prefs.setWindowModality(Qt::ApplicationModal);
    prefs.show();
    QApplication::processEvents();
    w.onPadRequest(1, QJsonObject{{"op", "machine"}, {"id", b}}, QStringLiteral("10.0.0.9"));
    check(active() == a, "no machine switch while a dialog is open");

    prefs.close();
    QApplication::processEvents();
    w.onPadRequest(2, QJsonObject{{"op", "machine"}, {"id", b}}, QStringLiteral("10.0.0.9"));
    check(active() == b, "the switch goes through once the dialog is closed");

    w.m_dirty = false;
    std::printf("%s\n", g_fails ? "FAIL: test_padmachinemodal" : "test_padmachinemodal: OK");
    return g_fails ? 1 : 0;
}
