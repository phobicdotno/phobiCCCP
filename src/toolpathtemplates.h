#pragma once
#include "c2ddocument.h"
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

// Toolpath templates: a named set of toolpaths' settings (type, tool, depths,
// feeds, every parameter and its `phobic_expr` bindings) without the shapes
// they cut, kept as one JSON file per template in the app data folder
// (<AppDataLocation>/templates/<name>.json). Applying one adds fresh copies to
// a design, bound to the given shapes or to none.
namespace c2d {
namespace tptemplates {

QString folder();                                  // created on demand
QString fileFor(const QString &name);              // the .json path of a name
QStringList names();                               // saved templates, sorted

// The template object for these toolpaths: {"name", "version", "toolpaths":
// [{"type", "json"}]}, each json without uuid, shapes or group.
QJsonObject build(const QString &name, const QVector<Toolpath> &tps);
bool save(const QJsonObject &tpl, QString *error = nullptr);
bool load(const QString &name, QJsonObject *tpl, QString *error = nullptr);
bool remove(const QString &name);

// New toolpaths for `doc` from a template, each cutting `elementIds` (may be
// empty), in the document's default group with a name not yet used. A
// binding to a user parameter the design does not have is dropped (the
// field keeps its number) and named in *notes.
QVector<Toolpath> instantiate(const Document &doc, const QJsonObject &tpl,
                              const QStringList &elementIds, QStringList *notes = nullptr);

} // namespace tptemplates
} // namespace c2d
