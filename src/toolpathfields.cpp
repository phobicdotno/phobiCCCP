#include "toolpathfields.h"

#include <QSet>
#include <cmath>

namespace {

QJsonValue flatGet(const QJsonObject &j, const QString &key)
{
    const int dot = key.indexOf(QLatin1Char('.'));
    if (dot < 0)
        return j.value(key);
    return j.value(key.left(dot)).toObject().value(key.mid(dot + 1));
}

void flatSet(QJsonObject &j, const QString &key, const QJsonValue &v)
{
    const int dot = key.indexOf(QLatin1Char('.'));
    if (dot < 0) {
        j.insert(key, v);
        return;
    }
    QJsonObject sub = j.value(key.left(dot)).toObject();
    sub.insert(key.mid(dot + 1), v);
    j.insert(key.left(dot), sub);
}

double numberOf(const QJsonValue &v)
{
    return v.isString() ? v.toString().toDouble() : v.toDouble();
}

} // namespace

// The designed fields, in display order, for toolpath `type`.
QList<QPair<QString, ToolpathField>> toolpathFieldCatalog(const QString &type)
{
    using F = ToolpathField;
    QList<QPair<QString, F>> c;
    auto add = [&c](const QString &key, F f) { c.append({key, f}); };
    auto mm = [](const QString &card, const QString &label, double max = 1000, const QString &tip = {}) {
        // 3 decimals: at 2 a typed 3.175 came back as 3.18 and was saved so.
        F f; f.card = card; f.label = label; f.kind = F::Number; f.unit = QStringLiteral("mm");
        f.max = max; f.tip = tip; f.decimals = 3; return f;
    };
    auto depth = [](const QString &label, const QString &tip = {}) {
        F f; f.card = QStringLiteral("depth"); f.label = label; f.kind = F::Depth;
        f.unit = QStringLiteral("mm"); f.max = 1000; f.tip = tip; f.decimals = 3; return f;
    };
    auto deg = [](const QString &card, const QString &label, double min, double max) {
        F f; f.card = card; f.label = label; f.kind = F::Number; f.unit = QStringLiteral("°");
        f.min = min; f.max = max; f.step = 1; f.decimals = 1; return f;
    };
    auto sw = [](const QString &card, const QString &label, const QString &tip = {}) {
        F f; f.card = card; f.label = label; f.kind = F::Bool; f.tip = tip; return f;
    };
    auto choice = [](const QString &card, const QString &label,
                     QList<QPair<QJsonValue, QString>> ch) {
        F f; f.card = card; f.label = label; f.kind = F::Choice; f.choices = ch; return f;
    };
    auto enabledBy = [](F f, const QString &key) { f.enabledBy = key; return f; };

    // Depth
    add(QStringLiteral("start_depth"), depth(QStringLiteral("Start"), QStringLiteral("Depth the first pass starts from")));
    if (type == QLatin1String("fluting_toolpath"))
        add(QStringLiteral("end_depth"), depth(QStringLiteral("Flute depth"),
                                               QStringLiteral("Depth of the groove between the ramps")));
    else
        add(QStringLiteral("end_depth"), depth(QStringLiteral("End")));
    add(QStringLiteral("cut_depth"), depth(QStringLiteral("Cut depth")));
    add(QStringLiteral("break_through"), depth(QStringLiteral("Break through"), QStringLiteral("Extra depth below the stock")));
    add(QStringLiteral("min_depth"), depth(QStringLiteral("Shallowest")));
    add(QStringLiteral("max_depth"), depth(QStringLiteral("Deepest")));
    add(QStringLiteral("stepdown"), mm(QStringLiteral("depth"), QStringLiteral("Per pass"), 100));
    add(QStringLiteral("depth_per_pass"), mm(QStringLiteral("depth"), QStringLiteral("Per pass"), 100));
    add(QStringLiteral("thread_depth"), mm(QStringLiteral("depth"), QStringLiteral("Thread depth"), 50,
                                           QStringLiteral("0 = from the pitch")));

    // Cutting
    if (type == QLatin1String("contour"))
        add(QStringLiteral("ofset_dir"), choice(QStringLiteral("cut"), QStringLiteral("Side"),
            {{-1, QStringLiteral("Inside")}, {0, QStringLiteral("On the line")}, {1, QStringLiteral("Outside")}}));
    if (type == QLatin1String("thread_toolpath"))
        add(QStringLiteral("side"), choice(QStringLiteral("cut"), QStringLiteral("Thread"),
            {{QStringLiteral("internal"), QStringLiteral("Internal (nut)")},
             {QStringLiteral("external"), QStringLiteral("External (bolt)")}}));
    else if (type == QLatin1String("circular_toolpath"))
        add(QStringLiteral("side"), choice(QStringLiteral("cut"), QStringLiteral("Shape"),
            {{QStringLiteral("inside"), QStringLiteral("Round pocket")},
             {QStringLiteral("outside"), QStringLiteral("Boss")}}));
    else
        add(QStringLiteral("side"), choice(QStringLiteral("cut"), QStringLiteral("Side"),
            {{QStringLiteral("outside"), QStringLiteral("Outside")}, {QStringLiteral("inside"), QStringLiteral("Inside")}}));
    add(QStringLiteral("flip_inside_outside"), sw(QStringLiteral("cut"), QStringLiteral("Cut inside the line")));
    if (type == QLatin1String("contour") || type == QLatin1String("pocket_toolpath")
        || type == QLatin1String("cutout")) {
        F f = choice(QStringLiteral("cut"), QStringLiteral("Corners"),
            {{QStringLiteral("off"), QStringLiteral("Sharp (off)")}, {QStringLiteral("dogbone"), QStringLiteral("Dogbone")},
             {QStringLiteral("tbone"), QStringLiteral("T-bone")}});
        f.tip = QStringLiteral("Cut a bit-sized bite into each inside corner so a square part fits. "
                               "Dogbone goes diagonally, T-bone along the longer side.");
        add(QStringLiteral("corners"), f);
    }
    add(QStringLiteral("mode"), choice(QStringLiteral("cut"), QStringLiteral("Engrave"),
        {{QStringLiteral("outline"), QStringLiteral("Outline")}, {QStringLiteral("fill"), QStringLiteral("Hatch fill")},
         {QStringLiteral("both"), QStringLiteral("Outline + fill")}}));
    if (type == QLatin1String("3d_finish_toolpath")) {
        F f = choice(QStringLiteral("cut"), QStringLiteral("Strategy"),
            {{QStringLiteral("raster"), QStringLiteral("Raster")},
             {QStringLiteral("waterline"), QStringLiteral("Waterline (Z-level)")},
             {QStringLiteral("pencil"), QStringLiteral("Pencil (creases only)")}});
        f.tip = QStringLiteral("Raster: parallel passes over the surface. Waterline: one pass "
                               "around every wall per stepdown. Pencil: only the valleys and "
                               "inside corners a bigger bit could not reach");
        add(QStringLiteral("strategy"), f);
        F rd = mm(QStringLiteral("cut"), QStringLiteral("Bigger bit Ø"), 100,
                  QStringLiteral("Pencil: cut only where this bigger bit (the one that did the "
                                 "main finish) left material. 0 = twice this bit"));
        add(QStringLiteral("pencil_ref_diameter"), rd);
        add(QStringLiteral("direction"), choice(QStringLiteral("cut"), QStringLiteral("Direction"),
            {{QStringLiteral("climb"), QStringLiteral("Climb")},
             {QStringLiteral("conventional"), QStringLiteral("Conventional")},
             {QStringLiteral("zigzag"), QStringLiteral("Zigzag")}}));
    }
    if (type == QLatin1String("pocket_toolpath")) {
        F f = choice(QStringLiteral("cut"), QStringLiteral("Strategy"),
            {{QStringLiteral("offset"), QStringLiteral("Offset (rings)")},
             {QStringLiteral("raster"), QStringLiteral("Raster")}});
        f.tip = QStringLiteral("Offset: rings that follow the walls inward. Raster: one lap of the "
                               "wall, then straight back-and-forth lines. With \"Only what a bigger "
                               "tool left\" on, rings are cut either way");
        add(QStringLiteral("pocket_strategy"), f);
        F a = deg(QStringLiteral("cut"), QStringLiteral("Raster angle"), -180, 180);
        a.tip = QStringLiteral("Direction of the raster lines; 0 = along X, 90 = along Y");
        add(QStringLiteral("raster_angle"), a);
    }
    add(QStringLiteral("stepover"), mm(QStringLiteral("cut"), QStringLiteral("Stepover"), 100,
                                       QStringLiteral("Sideways step between passes")));
    add(QStringLiteral("stock_to_leave"), mm(QStringLiteral("cut"), QStringLiteral("Leave"), 50,
                                             QStringLiteral("Material left on the walls for a later pass")));
    add(QStringLiteral("climb"), sw(QStringLiteral("cut"), QStringLiteral("Climb milling")));
    add(QStringLiteral("chamfer_width"), mm(QStringLiteral("cut"), QStringLiteral("Chamfer width"), 50));
    add(QStringLiteral("tip_offset"), mm(QStringLiteral("cut"), QStringLiteral("Tip offset"), 20));
    add(QStringLiteral("boss_clearance"), mm(QStringLiteral("cut"), QStringLiteral("Boss clearance"), 100));
    {
        F f = mm(QStringLiteral("cut"), QStringLiteral("Pitch"), 20);
        f.decimals = 4; // thread pitches like 1/20" = 1.27 mm and 0.7938 need 4
        add(QStringLiteral("pitch"), f);
    }
    {
        F f; f.card = QStringLiteral("cut"); f.label = QStringLiteral("Passes"); f.kind = F::Int;
        f.min = 1; f.max = 20; add(QStringLiteral("passes"), f); // the export cuts at most 20
    }
    add(QStringLiteral("left_hand"), sw(QStringLiteral("cut"), QStringLiteral("Left-hand thread")));
    // Fluting ramps
    add(QStringLiteral("ramp_ends"), choice(QStringLiteral("entry"), QStringLiteral("Ramp"),
        {{QStringLiteral("both"), QStringLiteral("Both ends")}, {QStringLiteral("start"), QStringLiteral("Start only")},
         {QStringLiteral("end"), QStringLiteral("End only")}}));
    {
        F f; f.card = QStringLiteral("entry"); f.label = QStringLiteral("Ramp length"); f.kind = F::Number;
        f.max = 1000; f.decimals = 2; f.step = 1;
        f.tip = QStringLiteral("How long the groove takes to reach full depth, in mm or % of the vector");
        add(QStringLiteral("ramp_length"), f);
    }
    add(QStringLiteral("ramp_unit"), choice(QStringLiteral("entry"), QStringLiteral("Ramp length in"),
        {{QStringLiteral("percent"), QStringLiteral("% of the vector")}, {QStringLiteral("mm"), QStringLiteral("mm")}}));
    add(QStringLiteral("ramp_shape"), choice(QStringLiteral("entry"), QStringLiteral("Ramp shape"),
        {{QStringLiteral("linear"), QStringLiteral("Straight")}, {QStringLiteral("smooth"), QStringLiteral("Smooth (rounded)")}}));
    add(QStringLiteral("length"), mm(QStringLiteral("cut"), QStringLiteral("Slot length"), 500));
    add(QStringLiteral("angle"), deg(QStringLiteral("cut"), QStringLiteral("Angle"), -360, 360));
    add(QStringLiteral("line_spacing"), mm(QStringLiteral("cut"), QStringLiteral("Line spacing"), 50));
    add(QStringLiteral("crosshatch"), sw(QStringLiteral("cut"), QStringLiteral("Crosshatch")));
    add(QStringLiteral("min_length"), mm(QStringLiteral("cut"), QStringLiteral("Shortest stroke"), 1000));
    add(QStringLiteral("max_length"), mm(QStringLiteral("cut"), QStringLiteral("Longest stroke"), 1000));
    {
        F f; f.card = QStringLiteral("cut"); f.label = QStringLiteral("Stepover variation"); f.kind = F::Number;
        f.max = 1; f.step = 0.05; add(QStringLiteral("stepover_variation"), f);
    }
    add(QStringLiteral("peck_distance"), mm(QStringLiteral("cut"), QStringLiteral("Peck"), 100,
                                            QStringLiteral("0 = drill in one go")));
    {
        F f; f.card = QStringLiteral("cut"); f.label = QStringLiteral("Dwell"); f.kind = F::Number;
        f.unit = QStringLiteral("s"); f.max = 60; add(QStringLiteral("dwell"), f);
    }
    add(QStringLiteral("chip_break"), sw(QStringLiteral("cut"), QStringLiteral("Chip break")));
    add(QStringLiteral("chip_retract"), enabledBy(mm(QStringLiteral("cut"), QStringLiteral("Back off"), 10),
                                                  QStringLiteral("chip_break")));
    {
        F f; f.card = QStringLiteral("cut"); f.label = QStringLiteral("Feed"); f.kind = F::Number;
        f.unit = QStringLiteral("mm/min"); f.max = 20000; f.step = 50; f.decimals = 0;
        add(QStringLiteral("speeds.feedrate"), f);
        f.label = QStringLiteral("Plunge");
        add(QStringLiteral("speeds.plungerate"), f);
        f.label = QStringLiteral("Spindle"); f.unit = QStringLiteral("rpm"); f.max = 30000; f.step = 500;
        add(QStringLiteral("speeds.rpm"), f);
    }

    // Entry
    add(QStringLiteral("enable_ramping"), sw(QStringLiteral("entry"), QStringLiteral("Ramp in")));
    // Adaptive ramps between 0.5° and 15°, whatever is set.
    if (type == QLatin1String("adaptive_toolpath"))
        add(QStringLiteral("ramp_angle"), deg(QStringLiteral("entry"), QStringLiteral("Ramp angle"), 0.5, 15));
    else
        add(QStringLiteral("ramp_angle"), deg(QStringLiteral("entry"), QStringLiteral("Ramp angle"), 0, 45));
    add(QStringLiteral("helix_entry"), sw(QStringLiteral("entry"), QStringLiteral("Helix in")));
    add(QStringLiteral("helix_diameter"), enabledBy(mm(QStringLiteral("entry"), QStringLiteral("Helix Ø"), 200),
                                                    QStringLiteral("helix_entry")));
    add(QStringLiteral("helix_angle"), enabledBy(deg(QStringLiteral("entry"), QStringLiteral("Helix angle"), 0.5, 15),
                                                 QStringLiteral("helix_entry")));
    add(QStringLiteral("lead_radius"), mm(QStringLiteral("entry"), QStringLiteral("Lead-in arc"), 50,
                                          QStringLiteral("0 = straight in")));

    // Tabs
    {
        F f; f.card = QStringLiteral("tabs"); f.label = QStringLiteral("Tabs"); f.kind = F::InvBool;
        f.tip = QStringLiteral("Hold the part with small bridges on the last passes");
        add(QStringLiteral("ignore_tabs"), f);
    }
    add(QStringLiteral("tab_height"), mm(QStringLiteral("tabs"), QStringLiteral("Height"), 50));
    add(QStringLiteral("tab_width"), mm(QStringLiteral("tabs"), QStringLiteral("Width"), 30));

    // Rest machining
    if (type == QLatin1String("3d_rough_toolpath")) {
        // Per level, only what a bigger end mill could not reach.
        add(QStringLiteral("enable_rest"), sw(QStringLiteral("rest"), QStringLiteral("Rest: only what a bigger bit left"),
                                              QStringLiteral("Cut only the corners and narrow spots a bigger "
                                                             "3D Rough could not get into")));
        add(QStringLiteral("rest_auto"), enabledBy(sw(QStringLiteral("rest"), QStringLiteral("Bigger bit: the 3D Rough above"),
                                                      QStringLiteral("Use the bit of the nearest 3D Rough above this "
                                                                     "one in the list")),
                                                   QStringLiteral("enable_rest")));
        add(QStringLiteral("rest_diameter"), enabledBy(mm(QStringLiteral("rest"), QStringLiteral("Previous bit Ø"), 100,
                                                          QStringLiteral("Used when auto is off or there is no "
                                                                         "3D Rough above")),
                                                       QStringLiteral("enable_rest")));
    }
    add(QStringLiteral("rest_auto"), sw(QStringLiteral("rest"), QStringLiteral("Skip what the toolpaths above cut")));
    add(QStringLiteral("rest_overlap"), enabledBy(mm(QStringLiteral("rest"), QStringLiteral("Overlap"), 50),
                                                  QStringLiteral("rest_auto")));
    add(QStringLiteral("enable_rest"), sw(QStringLiteral("rest"), QStringLiteral("Only what a bigger tool left")));
    add(QStringLiteral("rest_diameter"), enabledBy(mm(QStringLiteral("rest"), QStringLiteral("Bigger tool Ø"), 100),
                                                   QStringLiteral("enable_rest")));

    // Finishing
    {
        F f; f.card = QStringLiteral("finish"); f.label = QStringLiteral("Passes"); f.kind = F::Int;
        f.min = 0; f.max = 10; add(QStringLiteral("finish_passes"), f);
    }
    add(QStringLiteral("finish_stepover"), mm(QStringLiteral("finish"), QStringLiteral("Stepover"), 10));
    add(QStringLiteral("spring_pass"), sw(QStringLiteral("finish"), QStringLiteral("Spring pass")));
    return c;
}

// Keys this type carries (written by the factory, kept for Carbide Create)
// that the export does not use for it: a field there would do nothing.
bool toolpathFieldUnused(const QString &type, const QString &k)
{
    auto is = [&type](std::initializer_list<const char *> types) {
        for (const char *t : types)
            if (type == QLatin1String(t))
                return true;
        return false;
    };
    // Circular and Adaptive always enter on their own ramp or helix at
    // ramp_angle: a "Ramp in" switch there read off while they ramped.
    if (k == QLatin1String("enable_ramping"))
        return is({"drilling_toolpath", "keyhole_toolpath", "advanced_vcarve_toolpath", "face_toolpath",
                   "bore_toolpath", "thread_toolpath", "slot_toolpath", "circular_toolpath",
                   "adaptive_toolpath", "fluting_toolpath"});
    if (k == QLatin1String("ramp_angle"))
        return is({"cutout", "drilling_toolpath", "keyhole_toolpath", "advanced_vcarve_toolpath",
                   "face_toolpath", "bore_toolpath", "thread_toolpath", "slot_toolpath",
                   "fluting_toolpath"});
    if (k == QLatin1String("stock_to_leave"))
        return is({"cutout", "drilling_toolpath", "keyhole_toolpath", "advanced_vcarve_toolpath"});
    if (k == QLatin1String("stepdown"))
        return is({"drilling_toolpath", "keyhole_toolpath", "thread_toolpath",
                   "fluting_toolpath"});   // peck, pitch; passes
    if (k == QLatin1String("stepover"))
        return is({"drilling_toolpath", "keyhole_toolpath"});
    if (k == QLatin1String("angle"))
        return is({"pocket_toolpath", "drilling_toolpath", "advanced_vcarve_toolpath"});
    if (k == QLatin1String("start_depth"))
        return is({"3d_rough_toolpath", "3d_finish_toolpath"});
    return false;
}


// The keys the form offers at their meaning though the file may lack them.
QJsonObject toolpathFormDefaults(const QString &type, const QJsonObject &json_)
{
    QJsonObject json = json_;
    // The form only shows the keys a toolpath has. A 3D finish saved before
    // waterline existed has no `strategy`; offer it at its meaning (raster),
    // which is written back only with the user's next edit.
    if (type == QLatin1String("3d_finish_toolpath") && !json.contains(QStringLiteral("strategy")))
        json.insert(QStringLiteral("strategy"), QStringLiteral("raster"));
    // Pencil's bigger bit: offered at 0 (twice this bit), shown with Pencil.
    if (type == QLatin1String("3d_finish_toolpath") && !json.contains(QStringLiteral("pencil_ref_diameter")))
        json.insert(QStringLiteral("pencil_ref_diameter"), 0.0);
    // Corner relief is a phobiCCCP key: offered as off on the toolpaths
    // that take it, written only when changed.
    if ((type == QLatin1String("contour") || type == QLatin1String("pocket_toolpath")
         || type == QLatin1String("cutout"))
        && !json.contains(QStringLiteral("corners")))
        json.insert(QStringLiteral("corners"), QStringLiteral("off"));
    // Pockets: the clearing strategy, rings unless set (raster_angle shows
    // only with Raster; see rebuild()).
    if (type == QLatin1String("pocket_toolpath")) {
        if (!json.contains(QStringLiteral("pocket_strategy")))
            json.insert(QStringLiteral("pocket_strategy"), QStringLiteral("offset"));
        if (!json.contains(QStringLiteral("raster_angle")))
            json.insert(QStringLiteral("raster_angle"), 0.0);
    }
    // 3D Rough: rest roughing offered off, the bigger bit taken from the
    // 3D Rough above; written only with the user's next edit.
    if (type == QLatin1String("3d_rough_toolpath")) {
        if (!json.contains(QStringLiteral("enable_rest")))
            json.insert(QStringLiteral("enable_rest"), false);
        if (!json.contains(QStringLiteral("rest_auto")))
            json.insert(QStringLiteral("rest_auto"), true);
        if (!json.contains(QStringLiteral("rest_diameter")))
            json.insert(QStringLiteral("rest_diameter"), 6.35);
    }
    return json;
}

// The pad's "set" map: every designed field this toolpath has, numbers as
// numbers and depths as positive mm (the form's own view of them).
QJsonObject padSettings(const QString &type, const QJsonObject &json_)
{
    using Field = ToolpathField;
    const QJsonObject json = toolpathFormDefaults(type, json_);
    const QJsonObject binds = json.value(QStringLiteral("phobic_expr")).toObject();
    QJsonObject out;
    for (const auto &kf : toolpathFieldCatalog(type)) {
        const QString &k = kf.first;
        const QJsonValue v = flatGet(json, k);
        if (out.contains(k) || v.isUndefined() || toolpathFieldUnused(type, k) || binds.contains(k))
            continue;
        switch (kf.second.kind) {
        case Field::Depth: out.insert(k, std::fabs(numberOf(v))); break;
        case Field::Number: out.insert(k, numberOf(v)); break;
        case Field::Int: out.insert(k, int(std::lround(numberOf(v)))); break;
        case Field::Bool:
        case Field::InvBool:
            if (v.isBool())
                out.insert(k, v);
            break;
        case Field::Choice: out.insert(k, v); break;
        }
    }
    return out;
}

// Pad-edited "set" values over `json`, with the form's bounds and the file's
// conventions (depth sign, string vs number). Unknown keys, keys this type
// has no field for and values of the wrong kind are skipped; a key in `skip`
// too. A changed value drops the field's expression binding.
void applyPadSettings(const QString &type, QJsonObject &json, const QJsonObject &set,
                                    const QStringList &skip)
{
    using Field = ToolpathField;
    const QJsonObject withDefaults = toolpathFormDefaults(type, json);
    QJsonObject binds = json.value(QStringLiteral("phobic_expr")).toObject();
    QSet<QString> seen;
    for (const auto &kf : toolpathFieldCatalog(type)) {
        const QString &k = kf.first;
        const Field &f = kf.second;
        if (seen.contains(k))
            continue;
        seen.insert(k);
        const QJsonValue in = set.value(k);
        const QJsonValue orig = flatGet(withDefaults, k);
        if (in.isUndefined() || orig.isUndefined() || toolpathFieldUnused(type, k) || skip.contains(k))
            continue;
        QJsonValue v;
        switch (f.kind) {
        case Field::Depth:
        case Field::Number: {
            if (!in.isDouble() || !std::isfinite(in.toDouble()))
                continue;
            const double d = qBound(f.min, in.toDouble(), f.max);
            if (std::abs(d - (f.kind == Field::Depth ? std::fabs(numberOf(orig)) : numberOf(orig))) < 1e-9)
                continue;
            bool neg = false;
            if (f.kind == Field::Depth) {
                neg = orig.isString() ? orig.toString().trimmed().startsWith(QLatin1Char('-'))
                                      : orig.toDouble() < 0;
                if (!neg && numberOf(orig) == 0)
                    for (const char *dk : {"end_depth", "start_depth", "max_depth", "cut_depth"})
                        if (json.value(QLatin1String(dk)).toString().trimmed().startsWith(QLatin1Char('-')))
                            neg = true;
            }
            const double o = neg ? -d : d;
            v = orig.isString() ? QJsonValue(f.kind == Field::Depth ? QString::number(o, 'f', 3)
                                                                    : QString::number(o))
                                : QJsonValue(o);
            break;
        }
        case Field::Int: {
            if (!in.isDouble())
                continue;
            const int n = int(std::lround(qBound(f.min, in.toDouble(), f.max)));
            if (n == int(std::lround(numberOf(orig))))
                continue;
            v = orig.isString() ? QJsonValue(QString::number(n)) : QJsonValue(n);
            break;
        }
        case Field::Bool:
        case Field::InvBool:
            if (!in.isBool() || in == orig)
                continue;
            v = in;
            break;
        case Field::Choice: {
            bool ok = false;
            for (const auto &c : f.choices)
                if (c.first == in || (c.first.isDouble() && in.isDouble() && c.first.toDouble() == in.toDouble()))
                    ok = true, v = c.first;
            if (!ok || v == orig || (v.isDouble() && orig.isDouble() && v.toDouble() == orig.toDouble()))
                continue;
            break;
        }
        }
        flatSet(json, k, v);
        binds.remove(k);
    }
    if (json.contains(QStringLiteral("phobic_expr"))) {
        if (binds.isEmpty())
            json.remove(QStringLiteral("phobic_expr"));
        else
            json.insert(QStringLiteral("phobic_expr"), binds);
    }
}

