#pragma once
// The toolpath form's designed fields (cards, labels, units, ranges, tips),
// shared by the desktop form and the pad link (the pad's "set" map).

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

// One designed field: where it goes, what it is called and how it is edited.
struct ToolpathField {
    enum Kind { Depth, Number, Int, Bool, InvBool, Choice };
    QString card;
    QString label;
    Kind kind = Number;
    QString unit;
    double min = 0, max = 100000, step = 0.1;
    int decimals = 2;
    QList<QPair<QJsonValue, QString>> choices;
    QString tip;
    QString enabledBy;    // bool key; the field is greyed while it is off
};

// The designed fields, in display order, for toolpath `type` (a key may
// come twice; the first one counts).
QList<QPair<QString, ToolpathField>> toolpathFieldCatalog(const QString &type);
// A key this type carries that the export does not use: no field.
bool toolpathFieldUnused(const QString &type, const QString &key);
// `json` with the keys the form offers at their meaning though a file may
// lack them (corners off, pocket strategy rings, ...).
QJsonObject toolpathFormDefaults(const QString &type, const QJsonObject &json);

// For the pad link: the designed fields of a toolpath as plain values
// (depths positive mm), and pad-edited values put back with the bounds and
// conventions the form uses. Keys in `skip` are left alone.
QJsonObject padSettings(const QString &type, const QJsonObject &json);
void applyPadSettings(const QString &type, QJsonObject &json, const QJsonObject &set,
                      const QStringList &skip = {});
