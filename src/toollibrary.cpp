#include "toollibrary.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>

// The library ships inside the core static library; referencing the resource
// initializer from here keeps the linker from dropping qrc_tools.o.
static void initToolResources()
{
    Q_INIT_RESOURCE(tools);
}

namespace c2d {

int LibraryTool::ccType() const
{
    if (kind == QLatin1String("ball"))
        return 1;
    if (kind == QLatin1String("vbit") || kind == QLatin1String("engraver") || angle > 0)
        return 2;
    return 0;
}

const ToolFeeds *LibraryTool::feedsFor(const QString &materialId) const
{
    auto it = feeds.constFind(materialId);
    return it == feeds.constEnd() ? nullptr : &it.value();
}

bool ToolLibrary::loadDefault(QString *err)
{
    initToolResources();
    QFile f(QStringLiteral(":/data/tool-library.json"));
    if (!f.open(QIODevice::ReadOnly)) {
        if (err)
            *err = QStringLiteral("embedded tool library missing");
        return false;
    }
    const bool ok = loadJson(f.readAll(), err);
    if (ok)
        m_source = QStringLiteral("built-in");
    return ok;
}

bool ToolLibrary::loadFile(const QString &path, QString *err)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err)
            *err = f.errorString();
        return false;
    }
    const bool ok = loadJson(f.readAll(), err);
    if (ok)
        m_source = path;
    return ok;
}

bool ToolLibrary::loadJson(const QByteArray &json, QString *err)
{
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (doc.isNull() || !doc.isObject()) {
        if (err)
            *err = QStringLiteral("tool library: %1").arg(pe.errorString());
        return false;
    }
    const QJsonObject root = doc.object();
    QVector<LibraryMaterial> mats;
    for (const QJsonValue &v : root.value("materials").toArray()) {
        const QJsonObject m = v.toObject();
        LibraryMaterial lm;
        lm.id = m.value("id").toString();
        lm.name = m.value("name").toString(lm.id);
        for (const QJsonValue &n : m.value("cc_names").toArray())
            lm.ccNames << n.toString();
        if (!lm.id.isEmpty())
            mats.append(lm);
    }
    QVector<LibraryTool> tools;
    for (const QJsonValue &v : root.value("tools").toArray()) {
        const QJsonObject t = v.toObject();
        LibraryTool lt;
        lt.number = t.value("number").toInt();
        lt.model = t.value("model").toString(QString::number(lt.number));
        lt.name = t.value("name").toString();
        lt.kind = t.value("kind").toString(QStringLiteral("endmill"));
        lt.url = t.value("url").toString();
        lt.notes = t.value("notes").toString();
        lt.vendor = t.value("vendor").toString(QStringLiteral("Carbide 3D"));
        lt.diameter = t.value("diameter").toDouble();
        lt.angle = t.value("angle").toDouble();
        lt.cornerRadius = t.value("corner_radius").toDouble();
        lt.length = t.value("length").toDouble();
        lt.shank = t.value("shank").toDouble(lt.diameter);
        lt.flutes = t.value("flutes").toInt(2);
        lt.uncertain = t.value("uncertain").toBool(false);
        const QJsonObject feeds = t.value("feeds").toObject();
        for (auto it = feeds.constBegin(); it != feeds.constEnd(); ++it) {
            const QJsonObject f = it.value().toObject();
            ToolFeeds tf;
            tf.rpm = f.value("rpm").toDouble();
            tf.feed = f.value("feed").toDouble();
            tf.plunge = f.value("plunge").toDouble();
            tf.stepdown = f.value("stepdown").toDouble();
            tf.stepoverPct = f.value("stepover_pct").toDouble();
            tf.guess = f.value("guess").toBool(false);
            tf.note = f.value("note").toString();
            lt.feeds.insert(it.key(), tf);
        }
        if (lt.number > 0 && lt.diameter > 0)
            tools.append(lt);
    }
    if (tools.isEmpty()) {
        if (err)
            *err = QStringLiteral("tool library: no tools");
        return false;
    }
    m_materials = mats;
    m_tools = tools;
    return true;
}

const LibraryTool *ToolLibrary::byNumber(int number) const
{
    for (const LibraryTool &t : m_tools)
        if (t.number == number)
            return &t;
    return nullptr;
}

QString ToolLibrary::materialName(const QString &id) const
{
    for (const LibraryMaterial &m : m_materials)
        if (m.id == id)
            return m.name;
    return id;
}

QString ToolLibrary::materialIdForCC(const QString &ccMaterial) const
{
    const QString want = ccMaterial.trimmed();
    if (want.isEmpty())
        return QString();
    for (const LibraryMaterial &m : m_materials) {
        if (m.id.compare(want, Qt::CaseInsensitive) == 0
            || m.name.compare(want, Qt::CaseInsensitive) == 0)
            return m.id;
        for (const QString &n : m.ccNames)
            if (n.compare(want, Qt::CaseInsensitive) == 0)
                return m.id;
    }
    return QString();
}

QJsonObject ToolLibrary::toolObject(const LibraryTool &t, const ToolFeeds &f,
                                    const QJsonObject &base)
{
    // Key set and value types as CC 843/853 write them (see the embedded tool
    // object in GEOMETRY-TOOLPATHS.md). A user-defined tool: read_only false,
    // null uuid, full definition embedded, so CC needs no library lookup.
    QJsonObject o = base;
    o.insert(QStringLiteral("angle"), t.angle);
    o.insert(QStringLiteral("corner_radius"), t.cornerRadius);
    o.insert(QStringLiteral("diameter"), t.diameter);
    if (!o.contains(QStringLiteral("display_mm")))
        o.insert(QStringLiteral("display_mm"), true);
    if (!o.contains(QStringLiteral("finish_allowance")))
        o.insert(QStringLiteral("finish_allowance"), 0.5);
    o.insert(QStringLiteral("flutes"), t.flutes);
    o.insert(QStringLiteral("length"), t.length > 0 ? t.length : t.diameter * 3);
    o.insert(QStringLiteral("model"), t.model);
    o.insert(QStringLiteral("name"), t.name);
    o.insert(QStringLiteral("number"), t.number);
    if (!o.contains(QStringLiteral("overall_length")))
        o.insert(QStringLiteral("overall_length"), 3.175);
    o.insert(QStringLiteral("plungerate"), f.plunge);
    o.insert(QStringLiteral("read_only"), false);
    o.insert(QStringLiteral("slot_depth"), f.stepdown);
    o.insert(QStringLiteral("slot_feedrate"), f.feed);
    o.insert(QStringLiteral("slot_rpm"), f.rpm);
    o.insert(QStringLiteral("surfacing_feedrate"), f.feed);
    o.insert(QStringLiteral("surfacing_rpm"), f.rpm);
    if (!o.contains(QStringLiteral("surfacing_stepover")))
        o.insert(QStringLiteral("surfacing_stepover"), 20);
    o.insert(QStringLiteral("type"), t.ccType());
    o.insert(QStringLiteral("url"), t.url);
    o.insert(QStringLiteral("uuid"), QStringLiteral("{00000000-0000-0000-0000-000000000000}"));
    o.insert(QStringLiteral("vendor"), t.vendor);
    return o;
}

void ToolLibrary::applyToToolpath(QJsonObject &tp, const LibraryTool &t, const ToolFeeds &f)
{
    tp.insert(QStringLiteral("tool"), toolObject(t, f, tp.value("tool").toObject()));
    QJsonObject speeds = tp.value("speeds").toObject();
    speeds.insert(QStringLiteral("feedrate"), f.feed);
    speeds.insert(QStringLiteral("plungerate"), f.plunge);
    speeds.insert(QStringLiteral("rpm"), f.rpm);
    tp.insert(QStringLiteral("speeds"), speeds);

    // Depth per pass / stepover live under type-specific names; only touch
    // keys the payload already has so no type gains a key CC does not expect.
    const double stepover = t.diameter * f.stepoverPct / 100.0;
    if (f.stepdown > 0) {
        if (tp.contains(QStringLiteral("stepdown")))
            tp.insert(QStringLiteral("stepdown"), f.stepdown);
        if (tp.contains(QStringLiteral("depth_per_pass")))
            tp.insert(QStringLiteral("depth_per_pass"), f.stepdown);
    }
    if (stepover > 0 && tp.contains(QStringLiteral("stepover"))
        && tp.value("type").toString() != QLatin1String("advanced_vcarve_toolpath"))
        tp.insert(QStringLiteral("stepover"), stepover);
    if (tp.contains(QStringLiteral("automatic_parameters")))
        tp.insert(QStringLiteral("automatic_parameters"), false);
}

} // namespace c2d
