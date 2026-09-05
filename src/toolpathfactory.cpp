#include "toolpathfactory.h"
#include "toollibrary.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QUuid>

namespace c2d {

const QVector<ToolpathKind> &toolpathKinds()
{
    static const QVector<ToolpathKind> kinds = {
        {QStringLiteral("contour"), QStringLiteral("Contour")},
        {QStringLiteral("pocket_toolpath"), QStringLiteral("Pocket")},
        {QStringLiteral("drilling_toolpath"), QStringLiteral("Drill")},
        {QStringLiteral("texture_toolpath"), QStringLiteral("Texture")},
        {QStringLiteral("advanced_vcarve_toolpath"), QStringLiteral("V-Carve")},
        {QStringLiteral("keyhole_toolpath"), QStringLiteral("Keyhole")},
        {QStringLiteral("cutout"), QStringLiteral("Cutout")},
        {QStringLiteral("engrave_toolpath"), QStringLiteral("Engrave")},
    };
    return kinds;
}

QString toolpathLabel(const QString &type)
{
    for (const ToolpathKind &k : toolpathKinds())
        if (k.type == type)
            return k.label;
    return type;
}

QString depthString(const Document &doc, double positiveDown)
{
    bool negative = false;
    for (const Toolpath &t : doc.toolpaths()) {
        const QString d = t.json.value("end_depth").toString();
        if (!d.isEmpty()) {
            negative = d.startsWith(QLatin1Char('-'));
            break;
        }
    }
    return QString::number(negative ? -positiveDown : positiveDown, 'f', 3);
}

namespace {

enum CutterKind { Flat, VBit, Engraver };

// tool + speeds + per-pass values for a new toolpath.
struct ToolPick {
    QJsonObject tool, speeds;
    double stepdown = 1.524;    // mm per pass
    double stepover = 3.175;    // mm
};

CutterKind kindOf(const QJsonObject &tool)
{
    const int type = int(tool.value("type").toDouble(0));
    const double angle = tool.value("angle").toDouble(0);
    if (type == 2 || angle > 0) {
        // CC has no engraver type: a V cutter narrower than 5 mm is the
        // library's #50x engraver family.
        return tool.value("diameter").toDouble(12.7) < 5.0 ? Engraver : VBit;
    }
    return Flat;
}

// The most recently used tool of this kind in the document (last toolpath
// first), or the library default with the document material's feeds.
ToolPick pickTool(const Document &doc, CutterKind kind)
{
    ToolPick p;
    const QVector<Toolpath> &tps = doc.toolpaths();
    for (int i = tps.size() - 1; i >= 0; --i) {
        const QJsonObject &j = tps.at(i).json;
        for (const char *key : {"tool", "tool_pocket"}) {
            const QJsonObject tool = j.value(QLatin1String(key)).toObject();
            if (tool.isEmpty() || kindOf(tool) != kind)
                continue;
            const bool pocketTool = QLatin1String(key) == QLatin1String("tool_pocket");
            p.tool = tool;
            p.speeds = j.value(pocketTool ? "speeds_pocket" : "speeds").toObject();
            const QJsonValue sd = j.value(pocketTool ? "stepdown_pocket" : "stepdown");
            const QJsonValue so = j.value(pocketTool ? "stepover_pocket" : "stepover");
            if (sd.isDouble() && sd.toDouble() > 0)
                p.stepdown = sd.toDouble();
            else if (tool.value("slot_depth").toDouble() > 0)
                p.stepdown = tool.value("slot_depth").toDouble();
            if (so.isDouble() && so.toDouble() > 0 && kind == Flat)
                p.stepover = so.toDouble();
            else
                p.stepover = tool.value("diameter").toDouble(6.35) / 2.0;
            if (p.speeds.isEmpty()) {
                p.speeds.insert(QStringLiteral("feedrate"), tool.value("slot_feedrate").toDouble(1905));
                p.speeds.insert(QStringLiteral("plungerate"), tool.value("plungerate").toDouble(381));
                p.speeds.insert(QStringLiteral("rpm"), tool.value("slot_rpm").toDouble(18000));
            }
            return p;
        }
    }

    // Library default.
    ToolLibrary lib;
    const int number = kind == VBit ? 301 : kind == Engraver ? 501 : 201;
    const LibraryTool *t = lib.loadDefault() ? lib.byNumber(number) : nullptr;
    ToolFeeds f;
    if (t) {
        QString mat = lib.materialIdForCC(doc.params().value("material"));
        if (mat.isEmpty())
            mat = QStringLiteral("softwood");
        if (const ToolFeeds *ff = t->feedsFor(mat))
            f = *ff;
        else if (!t->feeds.isEmpty())
            f = t->feeds.constBegin().value();
    }
    if (!f.valid()) {
        // Conservative catch-all (CC's #201 softwood numbers scaled down).
        f.rpm = 18000;
        f.feed = kind == Flat ? 1905 : 1143;
        f.plunge = kind == Flat ? 381 : 305;
        f.stepdown = kind == Flat ? 1.524 : 2.54;
        f.stepoverPct = kind == Flat ? 50 : 5;
    }
    if (t) {
        p.tool = ToolLibrary::toolObject(*t, f);
    } else {
        // No library: a bare #201 definition in CC's key set.
        p.tool = QJsonObject{
            {QStringLiteral("angle"), kind == Flat ? 0 : kind == VBit ? 90 : 30},
            {QStringLiteral("corner_radius"), 0},
            {QStringLiteral("diameter"), kind == Flat ? 6.35 : kind == VBit ? 12.7 : 3.175},
            {QStringLiteral("display_mm"), true},
            {QStringLiteral("finish_allowance"), 0.5},
            {QStringLiteral("flutes"), 2},
            {QStringLiteral("length"), 19.05},
            {QStringLiteral("model"), QString::number(number)},
            {QStringLiteral("name"), QString()},
            {QStringLiteral("number"), number},
            {QStringLiteral("overall_length"), 3.175},
            {QStringLiteral("plungerate"), f.plunge},
            {QStringLiteral("read_only"), false},
            {QStringLiteral("slot_depth"), f.stepdown},
            {QStringLiteral("slot_feedrate"), f.feed},
            {QStringLiteral("slot_rpm"), f.rpm},
            {QStringLiteral("surfacing_feedrate"), f.feed},
            {QStringLiteral("surfacing_rpm"), f.rpm},
            {QStringLiteral("surfacing_stepover"), 20},
            {QStringLiteral("type"), kind == Flat ? 0 : 2},
            {QStringLiteral("url"), QString()},
            {QStringLiteral("uuid"), QStringLiteral("{00000000-0000-0000-0000-000000000000}")},
            {QStringLiteral("vendor"), QStringLiteral("Carbide 3D")},
        };
    }
    p.speeds = QJsonObject{{QStringLiteral("feedrate"), f.feed},
                           {QStringLiteral("plungerate"), f.plunge},
                           {QStringLiteral("rpm"), f.rpm}};
    p.stepdown = f.stepdown > 0 ? f.stepdown : 1.524;
    const double dia = p.tool.value("diameter").toDouble(6.35);
    p.stepover = kind == Flat && f.stepoverPct > 0 ? dia * f.stepoverPct / 100.0
               : kind == Flat ? dia / 2.0 : 0.2;
    return p;
}

QString nextName(const Document &doc, const QString &type, const QString &stem)
{
    int n = 0;
    for (const Toolpath &t : doc.toolpaths())
        if (t.type == type)
            ++n;
    return QStringLiteral("%1 %2").arg(stem).arg(n + 1);
}

} // namespace

Toolpath makeToolpath(const Document &doc, const QString &type,
                      const QStringList &elementIds)
{
    QJsonArray refs;
    for (const QString &id : elementIds)
        refs.append(QJsonObject{{QStringLiteral("uuid"), id}});

    const bool cutout = type == QLatin1String("cutout");
    const bool vcarve = type == QLatin1String("advanced_vcarve_toolpath");
    const bool engrave = type == QLatin1String("engrave_toolpath");
    const bool texture = type == QLatin1String("texture_toolpath");
    const ToolPick tp = pickTool(doc, vcarve ? VBit : engrave ? Engraver : Flat);

    QJsonObject j;
    // Common keys (CC writes automatic_parameters on all but cutout).
    j.insert(QStringLiteral("elements"), refs);
    j.insert(QStringLiteral("enabled"), true);
    j.insert(QStringLiteral("speeds"), tp.speeds);
    j.insert(QStringLiteral("start_depth"), depthString(doc, 0.0));
    j.insert(QStringLiteral("tool"), tp.tool);
    j.insert(QStringLiteral("toolpath_group"), doc.defaultToolpathGroup());
    j.insert(QStringLiteral("toolpath_layers"), QJsonArray());
    j.insert(QStringLiteral("type"), type);
    j.insert(QStringLiteral("uuid"), QUuid::createUuid().toString());
    j.insert(QStringLiteral("version"), 1);
    if (!cutout)
        j.insert(QStringLiteral("automatic_parameters"), true);
    if (!cutout && !texture && !engrave) {
        j.insert(QStringLiteral("end_depth"), depthString(doc, 2.54));
        j.insert(QStringLiteral("enable_ramping"), false);
        j.insert(QStringLiteral("ramp_angle"), 20);
        j.insert(QStringLiteral("stepdown"), tp.stepdown);
        j.insert(QStringLiteral("tolerance"), 0.01);
    }

    QString stem = toolpathLabel(type) + QStringLiteral(" Toolpath");
    if (type == QLatin1String("contour")) {
        j.insert(QStringLiteral("climb"), false);
        j.insert(QStringLiteral("ignore_tabs"), false);
        j.insert(QStringLiteral("ofset_dir"), -1);       // sic: CC's spelling
        j.insert(QStringLiteral("stepover"), tp.stepover);
        j.insert(QStringLiteral("stock_to_leave"), 0);
        j.insert(QStringLiteral("tab_height"), 3);
        j.insert(QStringLiteral("tab_width"), 12);
    } else if (type == QLatin1String("pocket_toolpath")) {
        j.insert(QStringLiteral("angle"), 0);
        j.insert(QStringLiteral("enable_rest"), false);
        j.insert(QStringLiteral("rest_diameter"), tp.tool.value("diameter").toDouble(6.35));
        j.insert(QStringLiteral("stepover"), tp.stepover);
        j.insert(QStringLiteral("stock_to_leave"), 0);
    } else if (type == QLatin1String("drilling_toolpath")) {
        stem = QStringLiteral("Drilling Toolpath");
        j.insert(QStringLiteral("angle"), 0);
        j.insert(QStringLiteral("drill_type"), 0);
        j.insert(QStringLiteral("peck_distance"), 3.175);
        j.insert(QStringLiteral("stepover"), tp.stepover);
        j.insert(QStringLiteral("stock_to_leave"), 0.508);
    } else if (type == QLatin1String("keyhole_toolpath")) {
        j.insert(QStringLiteral("angle"), 90);
        j.insert(QStringLiteral("length"), 12.7);
        j.insert(QStringLiteral("stepover"), tp.stepover);
        j.insert(QStringLiteral("stock_to_leave"), 0.508);
    } else if (texture) {
        j.insert(QStringLiteral("angle"), 20);
        j.insert(QStringLiteral("max_depth"), depthString(doc, 2.54));
        j.insert(QStringLiteral("max_length"), 30);
        j.insert(QStringLiteral("max_overlap"), 0.5);
        j.insert(QStringLiteral("min_depth"), depthString(doc, 0.0));
        j.insert(QStringLiteral("min_length"), 15);
        j.insert(QStringLiteral("min_overlap"), 0.5);
        j.insert(QStringLiteral("stepover"), tp.stepover);
        j.insert(QStringLiteral("stepover_variation"), 0.5);
        j.insert(QStringLiteral("tolerance"), 0.01);
    } else if (vcarve) {
        stem = QStringLiteral("VCarve");
        const ToolPick pocket = pickTool(doc, Flat);
        j.insert(QStringLiteral("angle"), 0);
        j.insert(QStringLiteral("inlay_enabled"), false);
        j.insert(QStringLiteral("link_type"), 0);
        j.insert(QStringLiteral("link_uuid"), QString());
        j.insert(QStringLiteral("pocket_enabled"), false);
        j.insert(QStringLiteral("pocket_first"), true);
        j.insert(QStringLiteral("speeds_pocket"), pocket.speeds);
        j.insert(QStringLiteral("stepdown_pocket"), pocket.stepdown);
        j.insert(QStringLiteral("stepover"), 0.2);
        j.insert(QStringLiteral("stepover_pocket"), pocket.stepover);
        j.insert(QStringLiteral("stock_to_leave"), 0.508);
        j.insert(QStringLiteral("stock_to_leave_pocket"), 0.508);
        j.insert(QStringLiteral("tool_pocket"), pocket.tool);
    } else if (cutout) {
        stem = QStringLiteral("Cutout");
        j.insert(QStringLiteral("break_through"), depthString(doc, 0.0));
        j.insert(QStringLiteral("cut_depth"), doc.params().value("thickness", "19.05").toDouble());
        j.insert(QStringLiteral("depth_per_pass"), tp.stepdown);
        j.insert(QStringLiteral("enable_finish_allowance"), false);
        j.insert(QStringLiteral("enable_finishing"), false);
        j.insert(QStringLiteral("finish_doc"), 1);
        j.insert(QStringLiteral("finish_speeds"),
                 QJsonObject{{QStringLiteral("feedrate"), 10},
                             {QStringLiteral("plungerate"), 10},
                             {QStringLiteral("rpm"), 1000}});
        j.insert(QStringLiteral("flip_inside_outside"), false);
        j.insert(QStringLiteral("ignore_tabs"), true);
        j.insert(QStringLiteral("path_type"), 0);
        j.insert(QStringLiteral("ramp_angle"), 0);
        j.insert(QStringLiteral("slot_depth_per_pass"), 0);
        j.insert(QStringLiteral("stock_to_leave"), 0);
        j.insert(QStringLiteral("tab_height"), 6.35);
        j.insert(QStringLiteral("tab_width"), 127);
    } else if (engrave) {
        // phobiCCCP-only type. mode: "outline" traces the vectors with no
        // offset, "fill" hatches closed regions (inset by the tool radius)
        // at line_spacing/angle and finishes with an inset outline pass,
        // "both" adds the exact outline trace to a fill. crosshatch adds a
        // second hatch at angle + 90.
        stem = QStringLiteral("Engrave");
        j.insert(QStringLiteral("automatic_parameters"), false);
        j.insert(QStringLiteral("end_depth"), depthString(doc, 0.5));
        j.insert(QStringLiteral("stepdown"), 0.5);
        j.insert(QStringLiteral("mode"), QStringLiteral("outline"));
        j.insert(QStringLiteral("line_spacing"), 1.0);
        j.insert(QStringLiteral("angle"), 45);
        j.insert(QStringLiteral("crosshatch"), false);
        j.insert(QStringLiteral("tolerance"), 0.01);
    }
    j.insert(QStringLiteral("name"), nextName(doc, type, stem));

    Toolpath t;
    t.uuid = j.value("uuid").toString();
    t.type = type;
    t.json = j;
    return t;
}

Toolpath duplicateToolpath(const Toolpath &src)
{
    Toolpath t = src;
    t.uuid = QUuid::createUuid().toString();
    t.json.insert(QStringLiteral("uuid"), t.uuid);
    t.json.insert(QStringLiteral("name"),
                  src.json.value("name").toString() + QStringLiteral(" copy"));
    return t;
}

} // namespace c2d
