// A machine switch reloads the BitZero page: it kept probing with the last
// machine's plate numbers, and the next edit saved them into the new one.
#include <QApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QDoubleSpinBox>
#include <QButtonGroup>
#include <cmath>
#include <cstdio>
#include "../src/machineprofiles.h"
#define private public
#define protected public
#include "../src/bitzeropanel.h"
#include "../src/machinepanel.h"
#undef private
#undef protected

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("phobicccp-test"));
    QApplication::setApplicationName(QStringLiteral("test_bzprofile"));
    QSettings().clear();
    QString a, b;
    {
        QSettings s;
        a = c2d::machineprofiles::activeId(s);
        s.setValue("machine/bitzero/v2/thickness", 15.0);
        b = c2d::machineprofiles::add(s, "Machine B");
        c2d::machineprofiles::activate(s, b);
        s.setValue("machine/bitzero/v2/thickness", 20.4);   // B's plate is thicker
        c2d::machineprofiles::activate(s, a);
        s.sync();
    }
    c2d::MachinePanel panel;
    {
        QSettings s;
        c2d::machineprofiles::activate(s, b);       // what MainWindow::switchMachine does
    }
    QString why;
    panel.reloadMachine(&why);
    const double used = panel.m_bzPanel->config().thickness;
    // Any edit on the page now writes A's numbers into B's profile.
    panel.m_bzPanel->m_search->setValue(panel.m_bzPanel->m_search->value() + 1);
    const double stored = QSettings().value("machine/bitzero/v2/thickness").toDouble();
    const bool ok = std::abs(used - 20.4) < 1e-9 && std::abs(stored - 20.4) < 1e-9;
    std::printf("%s after a machine switch the BitZero page uses and keeps the new machine's plate\n",
                ok ? "ok  " : "FAIL");
    return ok ? 0 : 1;
}
