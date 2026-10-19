#include "machineprofiles.h"

#include <QSettings>
#include <QStringList>
#include <QUuid>

namespace c2d::machineprofiles {

namespace {

const QString kMachine = QStringLiteral("machine");
// UI choices that are about this computer, not about a machine.
const QStringList kGlobal = {QStringLiteral("logOpen")};
// What a new profile does not take over from the one it is copied from.
const QStringList kPerMachine = {QStringLiteral("port"), QStringLiteral("ref/valid"),
                                 QStringLiteral("ref/z"), QStringLiteral("homedAt"),
                                 QStringLiteral("heightmap/map"), QStringLiteral("heightmap/use")};

QString dataGroup(const QString &id)
{
    return QStringLiteral("profiles/%1/data").arg(id);
}

// Every key under `from` (recursively), with its value.
QList<QPair<QString, QVariant>> readGroup(QSettings &s, const QString &from)
{
    QList<QPair<QString, QVariant>> out;
    s.beginGroup(from);
    const QStringList keys = s.allKeys();
    for (const QString &k : keys)
        out.append({k, s.value(k)});
    s.endGroup();
    return out;
}

void writeGroup(QSettings &s, const QString &to, const QList<QPair<QString, QVariant>> &kv)
{
    s.beginGroup(to);
    for (const auto &p : kv)
        s.setValue(p.first, p.second);
    s.endGroup();
}

QStringList ids(QSettings &s)
{
    return s.value(QStringLiteral("profiles/ids")).toStringList();
}

// The first time profiles are used, the current machine/ becomes "Machine 1".
void ensure(QSettings &s)
{
    if (!ids(s).isEmpty())
        return;
    const QString id = QStringLiteral("m1");
    s.setValue(QStringLiteral("profiles/ids"), QStringList{id});
    s.setValue(QStringLiteral("profiles/%1/name").arg(id), QStringLiteral("Machine 1"));
    s.setValue(QStringLiteral("profiles/active"), id);
}

} // namespace

QVector<Profile> list(QSettings &s)
{
    ensure(s);
    QVector<Profile> out;
    for (const QString &id : ids(s))
        out.append({id, nameOf(s, id)});
    return out;
}

QString activeId(QSettings &s)
{
    ensure(s);
    const QString a = s.value(QStringLiteral("profiles/active")).toString();
    return ids(s).contains(a) ? a : ids(s).value(0);
}

QString nameOf(QSettings &s, const QString &id)
{
    const QString n = s.value(QStringLiteral("profiles/%1/name").arg(id)).toString();
    return n.isEmpty() ? id : n;
}

QString add(QSettings &s, const QString &name)
{
    ensure(s);
    const QString id = QUuid::createUuid().toString(QUuid::Id128).left(12);
    QList<QPair<QString, QVariant>> kv;
    for (const auto &p : readGroup(s, kMachine))
        if (!kPerMachine.contains(p.first) && !kGlobal.contains(p.first))
            kv.append(p);
    writeGroup(s, dataGroup(id), kv);
    s.setValue(QStringLiteral("profiles/%1/name").arg(id),
               name.trimmed().isEmpty() ? QStringLiteral("Machine %1").arg(ids(s).size() + 1)
                                        : name.trimmed().left(60));
    s.setValue(QStringLiteral("profiles/ids"), ids(s) << id);
    return id;
}

bool rename(QSettings &s, const QString &id, const QString &name)
{
    ensure(s);
    if (!ids(s).contains(id) || name.trimmed().isEmpty())
        return false;
    s.setValue(QStringLiteral("profiles/%1/name").arg(id), name.trimmed().left(60));
    return true;
}

bool remove(QSettings &s, const QString &id)
{
    ensure(s);
    QStringList all = ids(s);
    if (!all.contains(id) || all.size() < 2 || id == activeId(s))
        return false;
    all.removeAll(id);
    s.setValue(QStringLiteral("profiles/ids"), all);
    s.remove(QStringLiteral("profiles/%1").arg(id));
    return true;
}

bool activate(QSettings &s, const QString &id)
{
    ensure(s);
    if (!ids(s).contains(id))
        return false;
    const QString was = activeId(s);
    if (was == id)
        return true;
    // Park the current machine/ group under its profile...
    s.remove(dataGroup(was));
    QList<QPair<QString, QVariant>> keep;   // UI choices stay put
    QList<QPair<QString, QVariant>> park;
    for (const auto &p : readGroup(s, kMachine))
        (kGlobal.contains(p.first) ? keep : park).append(p);
    writeGroup(s, dataGroup(was), park);
    // ...and put the other one in its place.
    const auto next = readGroup(s, dataGroup(id));
    s.remove(kMachine);
    writeGroup(s, kMachine, keep);
    writeGroup(s, kMachine, next);
    s.remove(dataGroup(id));
    s.setValue(QStringLiteral("profiles/active"), id);
    s.sync();
    return true;
}

} // namespace c2d::machineprofiles
