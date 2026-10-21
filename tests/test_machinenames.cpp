// Auto-named machine profiles collide after a removal; Remove machine picks by name.
#include "../src/machineprofiles.h"
#include <QCoreApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cstdio>
using namespace c2d;
int main(int argc, char **argv)
{
    QTemporaryDir home;
    qputenv("HOME", home.path().toLocal8Bit());
    qputenv("XDG_CONFIG_HOME", (home.path() + "/.config").toLocal8Bit());
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName("phobicccp-test");
    QCoreApplication::setApplicationName("test_machinenames");
    QSettings s;
    std::printf("settings at %s\n", qPrintable(s.fileName()));
    // The user adds two machines leaving the name empty (as the Add dialog allows).
    const QString b = machineprofiles::add(s, QString());
    const QString c = machineprofiles::add(s, QString());
    machineprofiles::remove(s, b);                 // forget "Machine 2"
    const QString d = machineprofiles::add(s, QString());
    for (const auto &p : machineprofiles::list(s))
        std::printf("  %s  %s\n", qPrintable(p.id), qPrintable(p.name));
    // Remove other machine: the dialog lists names and maps the pick back with indexOf.
    QStringList names, ids;
    const QString active = machineprofiles::activeId(s);
    for (const auto &p : machineprofiles::list(s))
        if (p.id != active) { names << p.name; ids << p.id; }
    const QString pick = names.at(1);             // user picks the second entry (the new machine d)
    const QString removed = ids.at(names.indexOf(pick));
    std::printf("picked entry 2 (%s, id %s); removed id %s\n", qPrintable(pick), qPrintable(d), qPrintable(removed));
    const bool bad = machineprofiles::nameOf(s, c) == machineprofiles::nameOf(s, d) && removed != d;
    std::printf("%s\n", bad ? "FAIL: two machines share an auto name and the other one is removed" : "ok");
    return bad ? 1 : 0;
}
