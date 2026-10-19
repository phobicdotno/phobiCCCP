#include "padlink.h"

#include "c2ddocument.h"
#include "element.h"
#include "toollibrary.h"
#include "toolpathfactory.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>

#include <cmath>

namespace c2d::padlink {

namespace {

const QString kFormat = QStringLiteral("phobicccp-pad");

bool finite(const QJsonValue &v, double lo = -10000, double hi = 10000)
{
    if (!v.isDouble())
        return false;
    const double d = v.toDouble();
    return std::isfinite(d) && d >= lo && d <= hi;
}

const QStringList &shapeTypes()
{
    static const QStringList t = {QStringLiteral("rect"), QStringLiteral("circle"),
                                  QStringLiteral("polygon"), QStringLiteral("text"),
                                  QStringLiteral("path"), QStringLiteral("point")};
    return t;
}

const QStringList &toolpathTypes()
{
    static const QStringList t = {QStringLiteral("profile"), QStringLiteral("pocket"),
                                  QStringLiteral("drill"), QStringLiteral("engrave")};
    return t;
}

QString checkShape(const QJsonObject &s, int *points)
{
    const QString type = s.value(QStringLiteral("type")).toString();
    const QString id = s.value(QStringLiteral("id")).toString();
    if (id.isEmpty() || id.size() > 64)
        return QStringLiteral("a shape has no id");
    if (!shapeTypes().contains(type))
        return QStringLiteral("shape %1: unknown type %2").arg(id, type.left(20));
    auto need = [&](const char *key, double lo, double hi) {
        return finite(s.value(QLatin1String(key)), lo, hi);
    };
    if (type == QLatin1String("path")) {
        const QJsonArray pts = s.value(QStringLiteral("points")).toArray();
        if (pts.size() < 2)
            return QStringLiteral("shape %1: a path needs two points").arg(id);
        *points += int(pts.size());
        if (*points > kMaxPoints)
            return QStringLiteral("too many path points");
        for (const QJsonValue &p : pts) {
            const QJsonArray xy = p.toArray();
            if (xy.size() != 2 || !finite(xy.at(0)) || !finite(xy.at(1)))
                return QStringLiteral("shape %1: bad point").arg(id);
        }
        return {};
    }
    if (!need("x", -10000, 10000) || !need("y", -10000, 10000))
        return QStringLiteral("shape %1: bad position").arg(id);
    if (type == QLatin1String("rect") && !(need("w", 0.01, 10000) && need("h", 0.01, 10000)))
        return QStringLiteral("shape %1: bad size").arg(id);
    if ((type == QLatin1String("circle") || type == QLatin1String("polygon"))
        && !need("r", 0.01, 10000))
        return QStringLiteral("shape %1: bad radius").arg(id);
    if (type == QLatin1String("polygon")) {
        const int n = s.value(QStringLiteral("sides")).toInt();
        if (n < 3 || n > 100)
            return QStringLiteral("shape %1: bad number of sides").arg(id);
    }
    if (type == QLatin1String("text")) {
        const QString t = s.value(QStringLiteral("text")).toString();
        if (t.trimmed().isEmpty() || t.size() > 500)
            return QStringLiteral("shape %1: bad text").arg(id);
        if (!need("size", 0.5, 2000))
            return QStringLiteral("shape %1: bad text size").arg(id);
    }
    return {};
}

} // namespace

bool validId(const QString &id)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9-]{1,64}$"));
    return re.match(id).hasMatch();
}

QString check(const QJsonObject &d)
{
    if (d.value(QStringLiteral("format")).toString() != kFormat)
        return QStringLiteral("not a PhobiCCCP pad design");
    if (d.value(QStringLiteral("version")).toInt() != 1)
        return QStringLiteral("design version %1 is newer than this phobiCCCP")
            .arg(d.value(QStringLiteral("version")).toInt());
    if (!validId(d.value(QStringLiteral("id")).toString()))
        return QStringLiteral("bad design id");
    if (!finite(d.value(QStringLiteral("modified")), 0, 1e15))
        return QStringLiteral("bad modified time");
    if (d.value(QStringLiteral("deleted")).toBool())
        return {};
    const QJsonObject st = d.value(QStringLiteral("stock")).toObject();
    if (!finite(st.value(QStringLiteral("width")), 1, 10000)
        || !finite(st.value(QStringLiteral("height")), 1, 10000)
        || !finite(st.value(QStringLiteral("thickness")), 0.1, 1000))
        return QStringLiteral("bad stock size");
    const QJsonArray shapes = d.value(QStringLiteral("shapes")).toArray();
    if (shapes.size() > kMaxShapes)
        return QStringLiteral("too many shapes (%1)").arg(shapes.size());
    int points = 0;
    QSet<QString> ids;
    for (const QJsonValue &v : shapes) {
        const QJsonObject s = v.toObject();
        if (const QString e = checkShape(s, &points); !e.isEmpty())
            return e;
        const QString id = s.value(QStringLiteral("id")).toString();
        if (ids.contains(id))
            return QStringLiteral("two shapes are called %1").arg(id);
        ids.insert(id);
    }
    const QJsonArray tps = d.value(QStringLiteral("toolpaths")).toArray();
    if (tps.size() > kMaxToolpaths)
        return QStringLiteral("too many toolpaths (%1)").arg(tps.size());
    for (const QJsonValue &v : tps) {
        const QJsonObject t = v.toObject();
        const QString name = t.value(QStringLiteral("name")).toString().left(60);
        if (!toolpathTypes().contains(t.value(QStringLiteral("type")).toString()))
            return QStringLiteral("toolpath %1: unknown type").arg(name);
        if (!finite(t.value(QStringLiteral("depth")), 0, 1000))
            return QStringLiteral("toolpath %1: bad depth").arg(name);
        if (!finite(t.value(QStringLiteral("tool")), 1, 1e9))
            return QStringLiteral("toolpath %1: no tool").arg(name);
        if (t.contains(QStringLiteral("enabled")) && !t.value(QStringLiteral("enabled")).isBool())
            return QStringLiteral("toolpath %1: bad enabled").arg(name);
        for (const QJsonValue &sid : t.value(QStringLiteral("shapes")).toArray())
            if (!ids.contains(sid.toString()))
                return QStringLiteral("toolpath %1: unknown shape %2").arg(name, sid.toString().left(64));
    }
    return {};
}

bool buildDocument(const QJsonObject &d, const QString &c2dPath, QStringList *notes,
                   QString *error)
{
    auto fail = [&](const QString &why) {
        if (error) *error = why;
        return false;
    };
    if (const QString e = check(d); !e.isEmpty())
        return fail(e);
    if (d.value(QStringLiteral("deleted")).toBool())
        return fail(QStringLiteral("the design was deleted"));

    ToolLibrary lib;
    QString err;
    if (!lib.loadDefault(&err))
        return fail(err);
    QString material = d.value(QStringLiteral("material")).toString();
    const LibraryMaterial *mat = nullptr;
    for (const LibraryMaterial &m : lib.materials())
        if (m.id == material)
            mat = &m;
    if (!mat) {
        for (const LibraryMaterial &m : lib.materials())
            if (m.id == QLatin1String("softwood"))
                mat = &m;
        if (!material.isEmpty() && notes)
            *notes << QStringLiteral("unknown material %1, used %2").arg(material.left(30),
                                                                       mat ? mat->name : QString());
        material = mat ? mat->id : QStringLiteral("softwood");
    }

    const QJsonObject st = d.value(QStringLiteral("stock")).toObject();
    const double thickness = st.value(QStringLiteral("thickness")).toDouble();
    if (!Document::createBlank(c2dPath, st.value(QStringLiteral("width")).toDouble(),
                               st.value(QStringLiteral("height")).toDouble(), thickness, &err))
        return fail(err);
    if (mat && !mat->ccNames.isEmpty()) {
        // The material is a param, which save() carries over from the file.
        const QString conn = QStringLiteral("padlink-%1").arg(QUuid::createUuid().toString());
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            db.setDatabaseName(c2dPath);
            if (db.open()) {
                QSqlQuery q(db);
                q.prepare(QStringLiteral("UPDATE params SET value=? WHERE key='material'"));
                q.addBindValue(mat->ccNames.first());
                q.exec();
                db.close();
            }
        }
        QSqlDatabase::removeDatabase(conn);
    }

    Document doc;
    if (!doc.load(c2dPath, &err))
        return fail(err);
    const QJsonObject layer = doc.defaultLayer();
    QHash<QString, QStringList> elementIds;   // pad shape id -> element ids
    for (const QJsonValue &v : d.value(QStringLiteral("shapes")).toArray()) {
        const QJsonObject s = v.toObject();
        const QString type = s.value(QStringLiteral("type")).toString();
        const double x = s.value(QStringLiteral("x")).toDouble();
        const double y = s.value(QStringLiteral("y")).toDouble();
        Element e;
        if (type == QLatin1String("rect")) {
            const double w = s.value(QStringLiteral("w")).toDouble();
            const double h = s.value(QStringLiteral("h")).toDouble();
            e = Element::makeRectangle(QPointF(x + w / 2, y + h / 2), w, h, layer);
        } else if (type == QLatin1String("circle")) {
            e = Element::makeCircle(QPointF(x, y), s.value(QStringLiteral("r")).toDouble(), layer);
        } else if (type == QLatin1String("polygon")) {
            e = Element::makePolygon(QPointF(x, y), s.value(QStringLiteral("r")).toDouble(),
                                     s.value(QStringLiteral("sides")).toInt(), layer);
        } else if (type == QLatin1String("text")) {
            e = Element::makeText(s.value(QStringLiteral("text")).toString(), QPointF(x, y),
                                  s.value(QStringLiteral("size")).toDouble(),
                                  QStringLiteral("Helvetica"), layer);
        } else if (type == QLatin1String("point")) {
            e = Element::makePoint(QPointF(x, y), layer);
        } else {
            QVector<QPointF> pts;
            for (const QJsonValue &p : s.value(QStringLiteral("points")).toArray())
                pts.append(QPointF(p.toArray().at(0).toDouble(), p.toArray().at(1).toDouble()));
            e = Element::makePath(pts, s.value(QStringLiteral("closed")).toBool(), layer);
        }
        if (e.id.isEmpty()) {
            if (notes)
                *notes << QStringLiteral("shape %1 could not be made").arg(s.value(QStringLiteral("id")).toString());
            continue;
        }
        doc.addElement(e);
        elementIds[s.value(QStringLiteral("id")).toString()].append(e.id);
    }

    for (const QJsonValue &v : d.value(QStringLiteral("toolpaths")).toArray()) {
        const QJsonObject t = v.toObject();
        const QString kind = t.value(QStringLiteral("type")).toString();
        const QString name = t.value(QStringLiteral("name")).toString().trimmed().left(60);
        QStringList refs;
        for (const QJsonValue &sid : t.value(QStringLiteral("shapes")).toArray())
            refs += elementIds.value(sid.toString());
        if (refs.isEmpty()) {
            if (notes)
                *notes << QStringLiteral("toolpath %1 has no shapes and was left out").arg(name);
            continue;
        }
        const int number = int(t.value(QStringLiteral("tool")).toDouble());
        const LibraryTool *tool = lib.byNumber(number);
        if (!tool)
            return fail(QStringLiteral("toolpath %1: tool #%2 is not in the tool library").arg(name).arg(number));
        const ToolFeeds *feeds = tool->feedsFor(material);
        if (!feeds || !feeds->valid())
            return fail(QStringLiteral("toolpath %1: %2 has no feeds and speeds for %3")
                            .arg(name, tool->name, lib.materialName(material)));
        const QString ccType = kind == QLatin1String("profile") ? QStringLiteral("contour")
                             : kind == QLatin1String("pocket")  ? QStringLiteral("pocket_toolpath")
                             : kind == QLatin1String("drill")   ? QStringLiteral("drilling_toolpath")
                                                                : QStringLiteral("engrave_toolpath");
        Toolpath tp = makeToolpath(doc, ccType, refs);
        QJsonObject j = tp.json;
        ToolLibrary::applyToToolpath(j, *tool, *feeds);
        const double depth = t.value(QStringLiteral("depth")).toDouble();
        j.insert(QStringLiteral("end_depth"), depthString(doc, depth));
        if (ccType == QLatin1String("contour")) {
            const QString side = t.value(QStringLiteral("side")).toString();
            j.insert(QStringLiteral("ofset_dir"), side == QLatin1String("inside") ? -1
                                                 : side == QLatin1String("on")   ? 0 : 1);
        }
        if (!name.isEmpty())
            j.insert(QStringLiteral("name"), name);
        j.insert(QStringLiteral("enabled"), t.value(QStringLiteral("enabled")).toBool(true));
        if (depth > thickness && notes)
            *notes << QStringLiteral("toolpath %1 goes %2 mm deeper than the stock")
                          .arg(name).arg(depth - thickness, 0, 'f', 2);
        tp.json = j;
        doc.addToolpath(tp);
    }
    if (!doc.save(c2dPath, &err))
        return fail(err);
    return true;
}

// ---- .c2d back to a pad design ---------------------------------------------

namespace {

QString padShapeId(const QString &elementId)
{
    QString id = elementId;
    id.remove(QLatin1Char('{')).remove(QLatin1Char('}'));
    return id.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : id.left(60);
}

QJsonArray xyRow(QPointF p)
{
    return QJsonArray{p.x(), p.y()};
}

// The toolpath keys a pad design cannot carry: compared against a toolpath
// freshly built from the pad design, so whatever was set here (feeds, tabs,
// ramps, passes, ...) shows as a difference.
const QStringList &ignoredToolpathKeys()
{
    static const QStringList k = {QStringLiteral("uuid"), QStringLiteral("elements"),
                                  QStringLiteral("toolpath_group"), QStringLiteral("name"),
                                  QStringLiteral("tool")};
    return k;
}

} // namespace

QJsonObject designFromDocument(const QString &c2dPath, const QJsonObject &base,
                               QStringList *lostOut, QString *error)
{
    QStringList lost;
    auto lose = [&](const QString &what) {
        if (!lost.contains(what))
            lost << what;
    };
    Document doc;
    QString err;
    if (!doc.load(c2dPath, &err)) {
        if (error) *error = err;
        return {};
    }
    ToolLibrary lib;
    lib.loadDefault();

    QJsonObject d = base;
    d.insert(QStringLiteral("format"), kFormat);
    d.insert(QStringLiteral("version"), 1);
    d.insert(QStringLiteral("deleted"), false);
    const double thickness = doc.params().value(QStringLiteral("thickness"), QStringLiteral("0")).toDouble();
    d.insert(QStringLiteral("stock"), QJsonObject{{QStringLiteral("width"), doc.boardWidth()},
                                                   {QStringLiteral("height"), doc.boardHeight()},
                                                   {QStringLiteral("thickness"), thickness}});
    // The material the document was built with (feeds are looked up in it),
    // and the pad's own: one this library does not know was built as
    // softwood, and stays as the pad had it rather than turn into softwood
    // on the first save here.
    const QString padMaterial = base.value(QStringLiteral("material")).toString();
    bool known = padMaterial.isEmpty();
    for (const LibraryMaterial &m : lib.materials())
        known = known || m.id == padMaterial;
    QString material = known ? padMaterial : QStringLiteral("softwood");
    const QString ccMaterial = doc.params().value(QStringLiteral("material"));
    for (const LibraryMaterial &m : lib.materials())
        for (const QString &n : m.ccNames)
            if (n.compare(ccMaterial, Qt::CaseInsensitive) == 0)
                material = m.id;
    d.insert(QStringLiteral("material"), known || material != QLatin1String("softwood") ? material : padMaterial);

    QJsonArray shapes;
    QHash<QString, QStringList> padIds;   // element id -> pad shape ids
    int points = 0;
    auto add = [&](const Element &e, QJsonObject s) {
        int n = 0;
        if (!checkShape(s, &n).isEmpty() || shapes.size() >= kMaxShapes || points + n > kMaxPoints) {
            lose(QStringLiteral("a shape the pad cannot hold"));
            return;
        }
        points += n;
        padIds[e.id].append(s.value(QStringLiteral("id")).toString());
        shapes.append(s);
    };
    for (const Element &e : doc.elements()) {
        if (Element::isConstruction(e)) {
            lose(QStringLiteral("construction lines"));
            continue;
        }
        if (!e.raw.value(QStringLiteral("tabs")).toArray().isEmpty())
            lose(QStringLiteral("tabs"));
        const QString sid = padShapeId(e.id);
        const QJsonObject &r = e.raw;
        const QJsonArray c = r.value(QStringLiteral("center")).toArray();
        const double cx = c.at(0).toDouble(), cy = c.at(1).toDouble();
        const QString gt = e.geometryType;
        if (Element::isPoint(e)) {
            add(e, {{QStringLiteral("id"), sid}, {QStringLiteral("type"), QStringLiteral("point")},
                    {QStringLiteral("x"), cx}, {QStringLiteral("y"), cy}});
            continue;
        }
        if (gt == QLatin1String("circle")) {
            add(e, {{QStringLiteral("id"), sid}, {QStringLiteral("type"), QStringLiteral("circle")},
                    {QStringLiteral("x"), cx}, {QStringLiteral("y"), cy},
                    {QStringLiteral("r"), r.value(QStringLiteral("radius")).toDouble()}});
            continue;
        }
        const bool turned = !qFuzzyIsNull(r.value(QStringLiteral("rotation")).toDouble());
        if (gt == QLatin1String("rectangle") && !turned
            && r.value(QStringLiteral("corner_type")).toInt() == 0) {
            const double w = r.value(QStringLiteral("width")).toDouble();
            const double h = r.value(QStringLiteral("height")).toDouble();
            add(e, {{QStringLiteral("id"), sid}, {QStringLiteral("type"), QStringLiteral("rect")},
                    {QStringLiteral("x"), cx - w / 2}, {QStringLiteral("y"), cy - h / 2},
                    {QStringLiteral("w"), w}, {QStringLiteral("h"), h}});
            continue;
        }
        if (gt == QLatin1String("regular_polygon") && !turned) {
            add(e, {{QStringLiteral("id"), sid}, {QStringLiteral("type"), QStringLiteral("polygon")},
                    {QStringLiteral("x"), cx}, {QStringLiteral("y"), cy},
                    {QStringLiteral("r"), r.value(QStringLiteral("radius")).toDouble()},
                    {QStringLiteral("sides"), r.value(QStringLiteral("num_sides")).toInt()}});
            continue;
        }
        if (gt == QLatin1String("text")) {
            const QJsonArray t = r.value(QStringLiteral("transform")).toArray();
            const bool plain = t.size() == 9 && t.at(0).toDouble() == 1 && t.at(1).toDouble() == 0
                               && t.at(3).toDouble() == 0 && t.at(4).toDouble() == 1;
            if (!plain || r.value(QStringLiteral("arc_enabled")).toBool())
                lose(QStringLiteral("turned or curved text"));
            if (r.value(QStringLiteral("font")).toString() != QLatin1String("Helvetica-Regular")
                || r.value(QStringLiteral("spacing")).toDouble(1) != 1)
                lose(QStringLiteral("fonts and letter spacing"));
            add(e, {{QStringLiteral("id"), sid}, {QStringLiteral("type"), QStringLiteral("text")},
                    {QStringLiteral("x"), t.size() == 9 ? t.at(6).toDouble() : 0},
                    {QStringLiteral("y"), t.size() == 9 ? t.at(7).toDouble() : 0},
                    {QStringLiteral("text"), r.value(QStringLiteral("text")).toString()},
                    {QStringLiteral("size"), r.value(QStringLiteral("font_height")).toDouble()}});
            continue;
        }
        if (gt != QLatin1String("path"))
            lose(QStringLiteral("turned or rounded shapes (sent as paths)"));
        const PathModel m = Element::pathModel(e);
        for (int si = 0; si < m.subs.size(); ++si) {
            const SubPath &sub = m.subs.at(si);
            QJsonArray pts;
            if (!sub.nodes.isEmpty())
                pts.append(xyRow(sub.nodes.first().p));
            for (int i = 0; i < sub.segmentCount(); ++i) {
                const PathNode *a = nullptr, *b = nullptr;
                sub.segment(i, &a, &b);
                const bool closing = sub.closed && i == sub.segmentCount() - 1;
                if (a->hasOut() || b->hasIn()) {
                    lose(QStringLiteral("curves (sent as short straight lines)"));
                    for (int k = 1; k < 16; ++k)
                        pts.append(xyRow(sub.pointAt(i, k / 16.0)));
                }
                if (!closing)
                    pts.append(xyRow(b->p));
            }
            add(e, {{QStringLiteral("id"), m.subs.size() == 1 ? sid : QStringLiteral("%1-%2").arg(sid).arg(si)},
                    {QStringLiteral("type"), QStringLiteral("path")},
                    {QStringLiteral("x"), 0}, {QStringLiteral("y"), 0},
                    {QStringLiteral("points"), pts}, {QStringLiteral("closed"), sub.closed}});
        }
    }
    d.insert(QStringLiteral("shapes"), shapes);

    QJsonArray toolpaths;
    for (const Toolpath &t : doc.toolpaths()) {
        const QString type = t.json.value(QStringLiteral("type")).toString();
        const QString kind = type == QLatin1String("contour")           ? QStringLiteral("profile")
                           : type == QLatin1String("pocket_toolpath")   ? QStringLiteral("pocket")
                           : type == QLatin1String("drilling_toolpath") ? QStringLiteral("drill")
                           : type == QLatin1String("engrave_toolpath")  ? QStringLiteral("engrave")
                                                                        : QString();
        if (kind.isEmpty()) {
            lose(QStringLiteral("%1 toolpaths").arg(toolpathLabel(type)));
            continue;
        }
        if (toolpaths.size() >= kMaxToolpaths) {
            lose(QStringLiteral("toolpaths over %1").arg(kMaxToolpaths));
            continue;
        }
        const QString name = t.json.value(QStringLiteral("name")).toString().left(60);
        const int number = t.json.value(QStringLiteral("tool")).toObject().value(QStringLiteral("number")).toInt();
        const LibraryTool *tool = lib.byNumber(number);
        const ToolFeeds *feeds = tool ? tool->feedsFor(material) : nullptr;
        if (!tool || !feeds || !feeds->valid()) {
            lose(QStringLiteral("toolpaths with a bit the pad does not know"));
            continue;
        }
        QJsonArray refs;
        QStringList elementIds;
        for (const QJsonValue &v : t.json.value(QStringLiteral("elements")).toArray()) {
            const QString id = v.toObject().value(QStringLiteral("uuid")).toString();
            elementIds << id;
            for (const QString &p : padIds.value(id))
                refs.append(p);
        }
        const double depth = std::abs(t.json.value(QStringLiteral("end_depth")).toString().toDouble());
        const int dir = t.json.value(QStringLiteral("ofset_dir")).toInt(1);
        const QString side = dir < 0 ? QStringLiteral("inside") : dir == 0 ? QStringLiteral("on")
                                                                         : QStringLiteral("outside");
        toolpaths.append(QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("type"), kind},
                                     {QStringLiteral("shapes"), refs}, {QStringLiteral("tool"), number},
                                     {QStringLiteral("depth"), depth}, {QStringLiteral("side"), side},
                                     {QStringLiteral("enabled"), t.json.value(QStringLiteral("enabled")).toBool(true)}});

        // What building this toolpath from the pad would give; anything else
        // was set here and would be lost.
        Toolpath fresh = makeToolpath(doc, type, elementIds);
        QJsonObject j = fresh.json;
        ToolLibrary::applyToToolpath(j, *tool, *feeds);
        j.insert(QStringLiteral("end_depth"), depthString(doc, depth));
        if (type == QLatin1String("contour"))
            j.insert(QStringLiteral("ofset_dir"), dir);
        j.insert(QStringLiteral("enabled"), t.json.value(QStringLiteral("enabled")).toBool(true));
        QJsonObject have = t.json;
        for (const QString &k : ignoredToolpathKeys()) {
            have.remove(k);
            j.remove(k);
        }
        if (have != j)
            lose(QStringLiteral("toolpath settings made here (feeds, passes, ramps, ...)"));
    }
    // Toolpaths still waiting for their shapes were left out of the document
    // (buildDocument); keep them, or a save here would take them off every pad.
    for (const QJsonValue &v : base.value(QStringLiteral("toolpaths")).toArray())
        if (v.toObject().value(QStringLiteral("shapes")).toArray().isEmpty() && toolpaths.size() < kMaxToolpaths)
            toolpaths.append(v);
    d.insert(QStringLiteral("toolpaths"), toolpaths);
    if (lostOut)
        *lostOut = lost;
    return d;
}

// ---- Links ------------------------------------------------------------------

namespace {

QString canonical(const QString &path)
{
    const QFileInfo fi(path);
    const QString c = fi.canonicalFilePath();
    return c.isEmpty() ? fi.absoluteFilePath() : c;
}

} // namespace

Links::Links(const QString &dir) : m_file(QDir(dir).filePath(QStringLiteral(".links.json")))
{
    QFile f(m_file);
    if (f.size() < 64 * 1024 * 1024 && f.open(QIODevice::ReadOnly))
        m_links = QJsonDocument::fromJson(f.readAll()).object();
}

QString Links::idFor(const QString &c2dPath) const
{
    return m_links.value(canonical(c2dPath)).toObject().value(QStringLiteral("id")).toString();
}

QStringList Links::pathsFor(const QString &id) const
{
    QStringList out;
    for (auto it = m_links.constBegin(); it != m_links.constEnd(); ++it)
        if (it.value().toObject().value(QStringLiteral("id")).toString() == id)
            out << it.key();
    return out;
}

bool Links::padOnly(const QString &c2dPath) const
{
    return m_links.value(canonical(c2dPath)).toObject().value(QStringLiteral("padOnly")).toBool();
}

double Links::modified(const QString &c2dPath) const
{
    return m_links.value(canonical(c2dPath)).toObject().value(QStringLiteral("modified")).toDouble();
}

void Links::set(const QString &c2dPath, const QString &id, bool padOnly, double modified)
{
    QJsonObject o{{QStringLiteral("id"), id}, {QStringLiteral("padOnly"), padOnly}};
    if (modified > 0)
        o.insert(QStringLiteral("modified"), modified);
    m_links.insert(canonical(c2dPath), o);
    write();
}

void Links::remove(const QString &c2dPath)
{
    m_links.remove(canonical(c2dPath));
    m_links.remove(c2dPath);
    write();
}

void Links::write() const
{
    QDir().mkpath(QFileInfo(m_file).absolutePath());
    QSaveFile f(m_file);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(m_links).toJson(QJsonDocument::Compact));
        f.commit();
    }
}

QJsonObject toolsJson()
{
    ToolLibrary lib;
    QJsonObject out;
    if (!lib.loadDefault())
        return out;
    QJsonArray mats;
    for (const LibraryMaterial &m : lib.materials())
        mats.append(QJsonObject{{QStringLiteral("id"), m.id}, {QStringLiteral("name"), m.name}});
    QJsonArray tools;
    for (const LibraryTool &t : lib.tools()) {
        QJsonArray with;
        for (auto it = t.feeds.cbegin(); it != t.feeds.cend(); ++it)
            if (it.value().valid())
                with.append(it.key());
        if (with.isEmpty())
            continue;
        tools.append(QJsonObject{{QStringLiteral("number"), t.number},
                                 {QStringLiteral("name"), t.name},
                                 {QStringLiteral("kind"), t.kind},
                                 {QStringLiteral("diameter"), t.diameter},
                                 {QStringLiteral("angle"), t.angle},
                                 {QStringLiteral("flutes"), t.flutes},
                                 {QStringLiteral("materials"), with}});
    }
    out.insert(QStringLiteral("materials"), mats);
    out.insert(QStringLiteral("tools"), tools);
    return out;
}

// ---- Store ----------------------------------------------------------------

QString Store::path(const QString &id) const
{
    return QDir(m_dir).filePath(id + QStringLiteral(".json"));
}

QJsonArray Store::list() const
{
    QJsonArray out;
    const QFileInfoList files = QDir(m_dir).entryInfoList({QStringLiteral("*.json")}, QDir::Files,
                                                          QDir::Name);
    for (const QFileInfo &fi : files) {
        const QJsonObject d = get(fi.completeBaseName());
        if (d.isEmpty())
            continue;
        out.append(QJsonObject{{QStringLiteral("id"), d.value(QStringLiteral("id"))},
                               {QStringLiteral("name"), d.value(QStringLiteral("name"))},
                               {QStringLiteral("modified"), d.value(QStringLiteral("modified"))},
                               {QStringLiteral("deleted"), d.value(QStringLiteral("deleted")).toBool()}});
    }
    return out;
}

QJsonObject Store::get(const QString &id) const
{
    if (!validId(id))
        return {};
    QFile f(path(id));
    if (f.size() > 64LL * 1024 * 1024 || !f.open(QIODevice::ReadOnly))
        return {};
    const QJsonObject d = QJsonDocument::fromJson(f.readAll()).object();
    if (!check(d).isEmpty() || d.value(QStringLiteral("id")).toString() != id)
        return {};
    return d;
}

bool Store::put(const QJsonObject &design, QString *error)
{
    if (const QString e = check(design); !e.isEmpty()) {
        if (error) *error = e;
        return false;
    }
    const QString id = design.value(QStringLiteral("id")).toString();
    const QJsonObject have = get(id);
    if (!have.isEmpty()
        && have.value(QStringLiteral("modified")).toDouble()
               >= design.value(QStringLiteral("modified")).toDouble())
        return false;
    if (!QDir().mkpath(m_dir)) {
        if (error) *error = QStringLiteral("cannot create %1").arg(m_dir);
        return false;
    }
    QSaveFile f(path(id));
    if (!f.open(QIODevice::WriteOnly)
        || f.write(QJsonDocument(design).toJson(QJsonDocument::Compact)) < 0 || !f.commit()) {
        if (error) *error = QStringLiteral("cannot write %1").arg(path(id));
        return false;
    }
    return true;
}

} // namespace c2d::padlink
