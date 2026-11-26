#pragma once
#include "c2ddocument.h"
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <functional>

namespace c2d {

// Carbide Create keeps no layer table: every element carries its own copy of
// its layer object ({name, uuid, red, green, blue, visible, locked}). A layer
// is the elements whose objects share a uuid (or the name, when the uuid is
// blank). Hiding a layer is view-only: its shapes are not drawn and not
// picked, but toolpaths that use them still cut them. A locked layer's
// shapes are drawn but cannot be selected or edited on the canvas.
namespace layers {

struct Layer {
    QString key;          // uuid, or "name:<name>" when the uuid is blank
    QJsonObject obj;      // the layer object, as the first element has it
    int count = 0;        // elements on it
    QString name() const { return obj.value(QStringLiteral("name")).toString(); }
    bool visible() const { return obj.value(QStringLiteral("visible")).toBool(true); }
    bool locked() const { return obj.value(QStringLiteral("locked")).toBool(false); }
};

QString keyOf(const QJsonObject &layer);
QString keyOf(const Element &e);
bool isHidden(const Element &e);
bool isLocked(const Element &e);

// The document's layers, in the order they first appear, then `extra`
// (layers made in the panel with nothing on them yet) not already present.
QVector<Layer> list(const Document &d, const QVector<QJsonObject> &extra = {});

// A fresh, visible, unlocked layer object with a new uuid.
QJsonObject makeLayer(const QString &name);

// The elements of `key`'s layer with `change` applied to their layer
// object (rename, show/hide, lock), as replacement elements.
QVector<Element> edited(const Document &d, const QString &key,
                        const std::function<void(QJsonObject &)> &change);
// The elements `ids` moved onto `layer` (only those not on it already).
QVector<Element> movedTo(const Document &d, const QStringList &ids, const QJsonObject &layer);
// Ids of the elements on `key`'s layer.
QStringList idsOn(const Document &d, const QString &key);

} // namespace layers
} // namespace c2d
