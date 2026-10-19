#pragma once
#include <QString>
#include <QVector>

class QSettings;

// Several machines on one phobiCCCP. Everything the Machine page keeps about
// "the" machine lives in the QSettings group machine/ (size, serial port,
// BitSetter and tool-change spots, reference Z, surface map, where it was
// last homed). A profile is a named copy of that group: switching saves the
// group into profiles/<id>/ and loads the other profile's copy in its place,
// so the rest of the app keeps reading machine/ and never knows.
//
// With no profiles stored yet there is one, "Machine 1", holding whatever
// machine/ has now.
namespace c2d::machineprofiles {

struct Profile {
    QString id;
    QString name;
};

QVector<Profile> list(QSettings &s);
QString activeId(QSettings &s);
QString nameOf(QSettings &s, const QString &id);

// A new profile named `name`, starting as a copy of the active machine's size
// and positions, without its port, reference or homed position (those belong
// to the other machine). It is not made active. Returns its id.
QString add(QSettings &s, const QString &name);
bool rename(QSettings &s, const QString &id, const QString &name);
// The active profile cannot be removed, nor the last one.
bool remove(QSettings &s, const QString &id);
// Make `id` the machine/ group. False when there is no such profile.
bool activate(QSettings &s, const QString &id);

} // namespace c2d::machineprofiles
