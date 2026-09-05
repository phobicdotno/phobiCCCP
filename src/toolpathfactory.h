#pragma once
#include "c2ddocument.h"
#include <QString>
#include <QStringList>
#include <QVector>

// New-toolpath factory: builds a toolpath payload with Carbide Create's own
// default parameter set for each type (exact key names and value types as CC
// 853 writes them — depths as 3-decimal strings, rates as numbers, flags as
// bools), so a file saved with one still opens in CC. The tool is copied
// from the document's last toolpath that uses a cutter of the right kind
// (flat / V-bit / engraver), else taken from the embedded tool library
// (#201 end mill, #301 V-bit, #501 engraver) with the document material's
// feeds. `engrave_toolpath` is phobiCCCP's own type (see gcodeexport.cpp).
namespace c2d {

struct ToolpathKind {
    QString type;    // CC `type` value, e.g. "pocket_toolpath"
    QString label;   // menu label, e.g. "Pocket"
};

// The creatable types in Carbide Create's menu order, plus Engrave.
const QVector<ToolpathKind> &toolpathKinds();
QString toolpathLabel(const QString &type);   // "pocket_toolpath" -> "Pocket"

// A fresh toolpath of `type` referencing `elementIds`, named "<Label> N",
// in the document's default group. Not added to the document.
Toolpath makeToolpath(const Document &doc, const QString &type,
                      const QStringList &elementIds);

// A copy with a new uuid and " copy" appended to the name.
Toolpath duplicateToolpath(const Toolpath &t);

// Depth string in the document's sign convention ("2.540" for build 853
// files, "-2.540" for build 843 ones) — decided by the existing toolpaths.
QString depthString(const Document &doc, double positiveDown);

} // namespace c2d
