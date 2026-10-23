#include "padlink.h"

#include "c2ddocument.h"
#include "parametric.h"
#include "element.h"
#include "toollibrary.h"
#include "toolpathfactory.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSysInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>

#include <algorithm>
#include <cmath>


// makeToolpath() sizes the lead-in, rest and helix from the document's last
// toolpath of the kind (or #201), which differs between building a design
// and reading it back; size them from the pad's own bit in both.
static void sizeToTool(QJsonObject &j, const c2d::LibraryTool &t)
{
    if (j.contains(QStringLiteral("lead_radius"))) j.insert(QStringLiteral("lead_radius"), 0.5 * t.diameter);
    if (j.contains(QStringLiteral("rest_diameter"))) j.insert(QStringLiteral("rest_diameter"), t.diameter);
    if (j.contains(QStringLiteral("helix_diameter"))) j.insert(QStringLiteral("helix_diameter"), t.diameter);
}
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

bool plausibleStamp(double modified, double now)
{
    return std::isfinite(modified) && modified <= now + kMaxAheadMs;
}

double nextStamp(double was, double now)
{
    if (now > was)
        return now;
    // Not just +1: a pad editing the same copy would pick that too, and
    // equal times count as the same copy.
    const double next = was + 1 + QRandomGenerator::global()->bounded(1000);
    // A copy stamped absurdly far ahead (a clock gone wrong) is not followed
    // there: this edit starts again from the clock, and the far one is
    // refused or ignored everywhere (Store::put, the pad's sync).
    return plausibleStamp(next, now) ? next : now;
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
            return fail(QStringLiteral("toolpath %1: tool #%2 is not in the tool library").arg(name, QString::number(number)));
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
        sizeToTool(j, *tool);
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
                          .arg(name, QString::number(depth - thickness, 'f', 2));
        tp.json = j;
        doc.addToolpath(tp);
    }
    // Lead-in, rest and helix follow each toolpath's own bit (sizeToTool);
    // files without this mark were built before that.
    // (save() writes shapes and toolpaths, not params: written after it.)
    if (!doc.save(c2dPath, &err)
        || !Document::writeParam(c2dPath, QStringLiteral("pad_sizing"), QStringLiteral("bit"), &err))
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
                                  QStringLiteral("tool"),
                                  // set false by a library pick, true on a new
                                  // toolpath here; nothing reads it
                                  QStringLiteral("automatic_parameters")};
    return k;
}

} // namespace

bool rebuildDocument(const QJsonObject &d, const QString &path, QString *err, const QString &keepFrom)
{
    const QString tmp = path + QStringLiteral(".pad-new");
    QStringList notes;
    if (!buildDocument(d, tmp, &notes, err)) {
        QFile::remove(tmp);
        return false;
    }
    // What the pad design does not carry is kept from the file it replaces
    // (or sits beside): every setting but the stock, material and toolpath
    // count the design sets, and the constraints, which name its old
    // geometry. So a retract height raised to clear clamps, the tiling and
    // the display units survive a pad's edit. And the file's permissions (a
    // shared, group-writable file stays so).
    const QString source = keepFrom.isEmpty() ? path : keepFrom;
    if (QFileInfo::exists(source)) {
        static const QStringList fromDesign = {
            QStringLiteral("width"), QStringLiteral("height"), QStringLiteral("thickness"),
            QStringLiteral("material"), QStringLiteral("num_toolpaths"), Parametric::paramKey()};
        Document before;
        if (before.load(source)) {
            const QHash<QString, QString> &params = before.params();
            for (auto it = params.cbegin(); it != params.cend(); ++it) {
                if (fromDesign.contains(it.key()))
                    continue;
                if (!Document::writeParam(tmp, it.key(), it.value(), err)) {
                    QFile::remove(tmp);
                    return false;
                }
            }
        }
        QFile::setPermissions(tmp, QFile::permissions(source)
                                       & ~(QFileDevice::ReadUser | QFileDevice::WriteUser | QFileDevice::ExeUser));
    }
    if (!QFileInfo::exists(path)) {
        if (QFile::rename(tmp, path))
            return true;
        QFile::remove(tmp);
        if (err) *err = QStringLiteral("could not write %1").arg(path);
        return false;
    }
    // The old file is moved aside, not deleted, until the new one is in.
    const QString old = path + QStringLiteral(".pad-old");
    QFile::remove(old);
    if (!QFile::rename(path, old)) {
        QFile::remove(tmp);
        if (err) *err = QStringLiteral("could not replace %1").arg(path);
        return false;
    }
    if (!QFile::rename(tmp, path)) {
        QFile::rename(old, path);
        QFile::remove(tmp);
        if (err) *err = QStringLiteral("could not replace %1").arg(path);
        return false;
    }
    QFile::remove(old);
    return true;
}

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
    const QJsonValue firstLayer = doc.elements().isEmpty() ? QJsonValue()
                                                           : doc.elements().first().raw.value(QStringLiteral("layer"));
    for (const Element &e : doc.elements()) {
        if (Element::isConstruction(e)) {
            lose(QStringLiteral("construction lines"));
            continue;
        }
        // The pad puts every shape on one layer, in no group.
        const QJsonValue group = e.raw.value(QStringLiteral("group_id"));
        if ((group.isArray() ? !group.toArray().isEmpty() : !group.toString().isEmpty())
            || e.raw.value(QStringLiteral("layer")) != firstLayer)
            lose(QStringLiteral("layers and groups"));
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
            // Within rounding: a uniform scale leaves 0.9999999999999999 behind.
            auto near = [&](int i, double v) { return std::abs(t.at(i).toDouble() - v) < 1e-9; };
            const bool plain = t.size() == 9 && near(0, 1) && near(1, 0) && near(3, 0) && near(4, 1);
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
    // Files built before sizeToTool() (no pad_sizing mark) sized the
    // lead-in, rest and helix from the previous toolpath's flat bit, else
    // #201: that size is what the pad built, not a setting made here. Only
    // that one size, and only in those files: any other is a desktop edit.
    const bool oldSizing = doc.params().value(QStringLiteral("pad_sizing")) != QLatin1String("bit");
    // Any flat bit earlier in the file (or #201): a bit changed here since
    // must not make the next toolpath's old size read as an edit.
    QVector<double> flatsBefore;
    if (const LibraryTool *t201 = lib.byNumber(201))
        flatsBefore << t201->diameter;
    for (const Toolpath &t : doc.toolpaths()) {
        const QString type = t.json.value(QStringLiteral("type")).toString();
        const QJsonObject thisTool = t.json.value(QStringLiteral("tool")).toObject();
        const QVector<double> oldSizes = flatsBefore;
        const int ccType = int(thisTool.value(QStringLiteral("type")).toDouble(0));
        if (thisTool.value(QStringLiteral("diameter")).toDouble() > 0 && ccType != 1 && ccType != 2
            && thisTool.value(QStringLiteral("angle")).toDouble(0) <= 0)
            flatsBefore << thisTool.value(QStringLiteral("diameter")).toDouble();
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
        sizeToTool(j, *tool);
        j.insert(QStringLiteral("end_depth"), depthString(doc, depth));
        if (type == QLatin1String("contour"))
            j.insert(QStringLiteral("ofset_dir"), dir);
        j.insert(QStringLiteral("enabled"), t.json.value(QStringLiteral("enabled")).toBool(true));
        QJsonObject have = t.json;
        auto oldBuild = [&](const QString &k, double scale) {
            if (!oldSizing || !have.contains(k) || !j.contains(k))
                return;
            for (double oldSize : oldSizes)
                if (oldSize > 0 && std::abs(have.value(k).toDouble() - oldSize * scale) < 1e-6) {
                    j.insert(k, have.value(k));
                    return;
                }
        };
        oldBuild(QStringLiteral("lead_radius"), 0.5);
        oldBuild(QStringLiteral("rest_diameter"), 1.0);
        oldBuild(QStringLiteral("helix_diameter"), 1.0);
        for (const QString &k : ignoredToolpathKeys()) {
            have.remove(k);
            j.remove(k);
        }
        // Read only with rest on (a bit change here leaves it at the old bit).
        if (!have.value(QStringLiteral("enable_rest")).toBool() && !j.value(QStringLiteral("enable_rest")).toBool()) {
            have.remove(QStringLiteral("rest_diameter"));
            j.remove(QStringLiteral("rest_diameter"));
        }
        if (have != j)
            lose(QStringLiteral("toolpath settings made here (feeds, passes, ramps, ...)"));
    }
    // Toolpaths still waiting for their shapes were left out of the document
    // (buildDocument); keep them, or a save here would take them off every pad.
    // One whose shapes were deleted here is in the document, and came out
    // above with none: not added again, or every save would add a copy.
    auto key = [](const QJsonObject &t) {
        return QStringLiteral("%1\n%2\n%3\n%4")
            .arg(t.value(QStringLiteral("name")).toString(), t.value(QStringLiteral("type")).toString(),
                 QString::number(t.value(QStringLiteral("tool")).toInt()),
                 QString::number(t.value(QStringLiteral("depth")).toDouble()));
    };
    QSet<QString> emptyHere;
    for (const QJsonValue &v : std::as_const(toolpaths))
        if (v.toObject().value(QStringLiteral("shapes")).toArray().isEmpty())
            emptyHere.insert(key(v.toObject()));
    for (const QJsonValue &v : base.value(QStringLiteral("toolpaths")).toArray()) {
        const QJsonObject t = v.toObject();
        if (t.value(QStringLiteral("shapes")).toArray().isEmpty() && toolpaths.size() < kMaxToolpaths
            && !emptyHere.contains(key(t)))
            toolpaths.append(v);
    }
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
    if (!f.exists() || f.size() == 0)
        return;
    // Links that cannot be read are kept as they are: writing what little was
    // understood over them would unlink every file for good.
    if (f.size() >= 64 * 1024 * 1024) {
        m_error = QStringLiteral("%1 is too big to read").arg(m_file);
        return;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        m_error = QStringLiteral("cannot read %1").arg(m_file);
        return;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        m_error = QStringLiteral("%1 is damaged").arg(m_file);
        return;
    }
    m_links = doc.object();
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

bool Links::changedOutside(const QString &c2dPath) const
{
    const QJsonObject o = m_links.value(canonical(c2dPath)).toObject();
    if (!o.contains(QStringLiteral("size")) || !o.contains(QStringLiteral("mtime")))
        return false;   // linked before the file's size and time were noted
    const QFileInfo fi(c2dPath);
    return fi.size() != qint64(o.value(QStringLiteral("size")).toDouble())
           || fi.lastModified().toMSecsSinceEpoch() != qint64(o.value(QStringLiteral("mtime")).toDouble());
}

bool Links::set(const QString &c2dPath, const QString &id, bool padOnly, double modified)
{
    if (!ok())
        return false;
    QJsonObject o{{QStringLiteral("id"), id}, {QStringLiteral("padOnly"), padOnly}};
    if (modified > 0)
        o.insert(QStringLiteral("modified"), modified);
    const QFileInfo fi(c2dPath);
    if (fi.exists()) {
        o.insert(QStringLiteral("size"), double(fi.size()));
        o.insert(QStringLiteral("mtime"), double(fi.lastModified().toMSecsSinceEpoch()));
    }
    m_links.insert(canonical(c2dPath), o);
    return write();
}

bool Links::remove(const QString &c2dPath)
{
    if (!ok())
        return false;
    m_links.remove(canonical(c2dPath));
    m_links.remove(c2dPath);
    return write();
}

QStringList followDesign(Links &links, const QJsonObject &design, const QString &except,
                         const std::function<bool(const QString &)> &leaveAlone)
{
    QStringList out;
    const QString id = design.value(QStringLiteral("id")).toString();
    if (!links.ok() || design.value(QStringLiteral("deleted")).toBool())
        return out;
    for (const QString &path : links.pathsFor(id)) {
        if (QFileInfo(path) == QFileInfo(except) || !QFileInfo::exists(path) || !links.padOnly(path)
            || links.changedOutside(path) || (leaveAlone && leaveAlone(path)))
            continue;
        QString err;
        if (rebuildDocument(design, path, &err)
            && links.set(path, id, true, design.value(QStringLiteral("modified")).toDouble()))
            out << path;
    }
    return out;
}

int Links::moveTo(const QString &id, double modified, const QString &toId)
{
    if (!ok())
        return -1;
    int moved = 0;
    for (auto it = m_links.begin(); it != m_links.end(); ++it) {
        QJsonObject o = it.value().toObject();
        if (o.value(QStringLiteral("id")).toString() != id
            || o.value(QStringLiteral("modified")).toDouble() != modified)
            continue;
        o.insert(QStringLiteral("id"), toId);
        it.value() = o;
        ++moved;
    }
    return moved == 0 || write() ? moved : -1;
}

bool Links::write()
{
    QDir().mkpath(QFileInfo(m_file).absolutePath());
    QSaveFile f(m_file);
    if (!f.open(QIODevice::WriteOnly)
        || f.write(QJsonDocument(m_links).toJson(QJsonDocument::Compact)) < 0 || !f.commit()) {
        m_error = QStringLiteral("cannot write %1").arg(m_file);
        return false;
    }
    return true;
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
                               {QStringLiteral("deleted"), d.value(QStringLiteral("deleted")).toBool()},
                               {QStringLiteral("vv"), d.value(QStringLiteral("vv"))}});
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

bool Store::write(const QJsonObject &design, QString *error)
{
    const QString id = design.value(QStringLiteral("id")).toString();
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

QString Store::forkIdFor(const QString &id, double modified, double base)
{
    static const QUuid ns(QStringLiteral("{6c1f4a2e-9b0d-4e57-8a3c-2f7d5e1b9c40}"));
    const QByteArray key = QStringLiteral("%1\n%2\n%3").arg(id).arg(modified, 0, 'f', 0).arg(base, 0, 'f', 0).toUtf8();
    return QUuid::createUuidV5(ns, key).toString(QUuid::WithoutBraces);
}

namespace {

// The vv as it is, believed or not, in `vv` (empty for a copy with none).
// False when it is malformed: then nothing can be told from it.
bool rawVv(const QJsonObject &design, QJsonObject *vv)
{
    const QJsonValue v = design.value(QStringLiteral("vv"));
    *vv = v.toObject();
    if (!v.isUndefined() && !v.isNull() && !v.isObject())
        return false;
    if (vv->size() > kMaxReplicas)
        return false;
    for (auto it = vv->constBegin(); it != vv->constEnd(); ++it)
        if (!validId(it.key()) || !finite(it.value(), 1, 1e15))
            return false;
    return true;
}

// What an older pad's or phobiCCCP's copy tells of the copy it was edited
// from: its vv, or for one with none (a design an older pad made), that
// nothing of the newer ones went into it. A deletion from an older pad
// keeps no vv, so it tells nothing.
bool olderVv(const QJsonObject &design, QJsonObject *vv)
{
    return rawVv(design, vv) && !(vv->isEmpty() && design.value(QStringLiteral("deleted")).toBool());
}

// The same text for the same vv on every computer (a QJsonObject's keys are sorted).
QString vvKey(const QJsonObject &vv)
{
    QString out;
    for (auto it = vv.constBegin(); it != vv.constEnd(); ++it)
        out += QStringLiteral("%1=%2;").arg(it.key()).arg(it.value().toDouble(), 0, 'f', 0);
    return out;
}

// No entry of `v` is later than `by`'s.
bool covered(const QJsonObject &v, const QJsonObject &by)
{
    for (auto it = v.constBegin(); it != v.constEnd(); ++it)
        if (it.value().toDouble() > by.value(it.key()).toDouble())
            return false;
    return true;
}

// How copy `x`, with a believed vv, stands to `y`, an older pad's or
// phobiCCCP's edit of the copy its vv names.
Order relate(const QJsonObject &vx, const QJsonObject &y)
{
    QJsonObject ry;
    if (!olderVv(y, &ry))
        return Order::Unknown;
    // Edited here (by a newer one) from y itself.
    if (vx.value(olderEditId(y)).toDouble() >= y.value(QStringLiteral("modified")).toDouble())
        return Order::After;
    // y was edited from x, or from a copy made from x.
    if (covered(vx, ry))
        return Order::Before;
    return Order::Concurrent;
}

} // namespace

QString olderEditId(const QJsonObject &design)
{
    // FNV-1a, twice (32 bits each), as the pad computes it (model.dart).
    QJsonObject vv;
    rawVv(design, &vv);
    const QByteArray key = QStringLiteral("%1\n%2")
                               .arg(design.value(QStringLiteral("modified")).toDouble(), 0, 'f', 0)
                               .arg(vvKey(vv))
                               .toUtf8();
    quint32 h1 = 2166136261u, h2 = 0x811c9dc5u ^ 0x5bd1e995u;
    for (const char c : key) {
        h1 = (h1 ^ quint8(c)) * 16777619u;
        h2 = (h2 ^ quint8(c)) * 16777619u;
    }
    return QStringLiteral("e-%1%2").arg(h1, 8, 16, QLatin1Char('0')).arg(h2, 8, 16, QLatin1Char('0'));
}

QJsonObject versionVector(const QJsonObject &design)
{
    QJsonObject vv;
    if (!rawVv(design, &vv) || vv.isEmpty())
        return {};
    double newest = 0;
    for (auto it = vv.constBegin(); it != vv.constEnd(); ++it)
        newest = std::max(newest, it.value().toDouble());
    // Stamped again by an older pad or phobiCCCP that kept the vv it got.
    if (newest != design.value(QStringLiteral("modified")).toDouble())
        return {};
    return vv;
}

QJsonObject ancestry(const QJsonObject &design)
{
    if (const QJsonObject vv = versionVector(design); !vv.isEmpty())
        return vv;
    QJsonObject vv;
    const double modified = design.value(QStringLiteral("modified")).toDouble();
    // A design new here (no time yet) has nothing to note.
    if (!olderVv(design, &vv) || !(modified >= 1))
        return {};
    vv.insert(olderEditId(design), modified);
    return vv;
}

Order order(const QJsonObject &a, const QJsonObject &b)
{
    const QJsonObject va = versionVector(a), vb = versionVector(b);
    if (va.isEmpty() || vb.isEmpty()) {
        if (a.value(QStringLiteral("modified")).toDouble() == b.value(QStringLiteral("modified")).toDouble())
            return Order::Same;
        // One from an older pad or phobiCCCP: it kept the vv of the copy it
        // edited, which tells whether that edit was made from the other.
        if (!va.isEmpty())
            return relate(va, b);
        if (!vb.isEmpty()) {
            const Order o = relate(vb, a);
            return o == Order::After ? Order::Before : o == Order::Before ? Order::After : o;
        }
        return Order::Unknown;
    }
    // A full vv dropped its oldest older-edit entries (Store::edited): an
    // entry it lacks at or below the oldest it still holds is one of those,
    // known to it, not an edit made apart.
    auto dropped = [](const QJsonObject &full) {
        double oldest = -1;
        if (full.size() >= kMaxReplicas)
            for (auto it = full.constBegin(); it != full.constEnd(); ++it)
                if (it.key().startsWith(QLatin1String("e-"))
                    && (oldest < 0 || it.value().toDouble() < oldest))
                    oldest = it.value().toDouble();
        return oldest;
    };
    const double droppedA = dropped(va), droppedB = dropped(vb);
    auto known = [](const QString &k, double t, double droppedUpTo) {
        return droppedUpTo >= 0 && k.startsWith(QLatin1String("e-")) && t <= droppedUpTo;
    };
    bool aAhead = false, bAhead = false;
    auto entry = [](const QJsonObject &v, const QString &k) { return v.value(k).toDouble(); };
    for (auto it = va.constBegin(); it != va.constEnd(); ++it) {
        const double t = it.value().toDouble();
        if (!vb.contains(it.key()) && known(it.key(), t, droppedB))
            continue;
        aAhead = aAhead || t > entry(vb, it.key());
        bAhead = bAhead || t < entry(vb, it.key());
    }
    for (auto it = vb.constBegin(); it != vb.constEnd(); ++it)
        bAhead = bAhead || (!va.contains(it.key()) && it.value().toDouble() > 0
                            && !known(it.key(), it.value().toDouble(), droppedA));
    return aAhead ? (bAhead ? Order::Concurrent : Order::After) : (bAhead ? Order::Before : Order::Same);
}

namespace {

// Of two copies edited apart, the one kept under the design's id: the later
// edit, so every computer settles the same pair the same way. A copy made
// from the winner is later still, so it beats the loser too.
bool wins(const QJsonObject &a, const QJsonObject &b)
{
    const double ma = a.value(QStringLiteral("modified")).toDouble();
    const double mb = b.value(QStringLiteral("modified")).toDouble();
    if (ma != mb)
        return ma > mb;
    return vvKey(versionVector(a)) > vvKey(versionVector(b));
}

} // namespace

QString Store::forkIdFor(const QJsonObject &copy)
{
    static const QUuid ns(QStringLiteral("{3e0b7c52-61a4-4f0e-9d27-8b5c1a6e4f93}"));
    const QByteArray key = QStringLiteral("%1\n%2\n%3")
                               .arg(copy.value(QStringLiteral("id")).toString())
                               .arg(copy.value(QStringLiteral("modified")).toDouble(), 0, 'f', 0)
                               .arg(vvKey(versionVector(copy)))
                               .toUtf8();
    return QUuid::createUuidV5(ns, key).toString(QUuid::WithoutBraces);
}

QString Store::replicaId() const
{
    const QString file = QDir(m_idDir).filePath(QStringLiteral(".replica"));
    // This computer, and the id's place on it.
    QString machine = QString::fromLatin1(QSysInfo::machineUniqueId().toHex());
    if (machine.isEmpty())
        machine = QSysInfo::machineHostName();
    const QString binding = QStringLiteral("%1 %2").arg(machine, QFileInfo(file).absoluteFilePath());
    QStringList here;
    if (QFile f(file); f.open(QIODevice::ReadOnly))
        here = QString::fromUtf8(f.read(4096)).split(QLatin1Char('\n'));
    if (here.size() > 1 && validId(here.first().trimmed()) && here.at(1) == binding)
        return here.first().trimmed();
    // Anything else is made again: an id tied to another computer or place
    // is a copy, and one from before ids were tied (kept in Documents by an
    // older phobiCCCP) may be one too, as nothing tells the computer it was
    // made on from one Documents was copied or synced to. A new id only adds
    // an entry to the versions of the designs edited here from now on.
    const QString legacy = QDir(m_dir).filePath(QStringLiteral(".replica"));
    if (QFileInfo(legacy) != QFileInfo(file))
        QFile::remove(legacy);
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QDir().mkpath(m_idDir);
    QSaveFile out(file);
    if (out.open(QIODevice::WriteOnly)) {
        out.write(QStringLiteral("%1\n%2\n").arg(id, binding).toUtf8());
        out.commit();
    }
    return id;
}

QJsonObject Store::edited(QJsonObject design, double modified) const
{
    // An older pad's or phobiCCCP's edit keeps its place in the versions, so
    // the edit made here is known to be made from it.
    QJsonObject vv = ancestry(design);
    vv.insert(replicaId(), modified);
    // Each older client's edit adds an entry; past the limit the versions
    // would not be believed and start over, so the oldest of those go first.
    while (vv.size() > kMaxReplicas) {
        QString oldest;
        for (auto it = vv.constBegin(); it != vv.constEnd(); ++it)
            if (it.key().startsWith(QLatin1String("e-"))
                && (oldest.isEmpty() || it.value().toDouble() < vv.value(oldest).toDouble()))
                oldest = it.key();
        if (oldest.isEmpty())
            break;
        vv.remove(oldest);
    }
    design.insert(QStringLiteral("modified"), modified);
    design.insert(QStringLiteral("vv"), vv);
    return design;
}

bool Store::put(const QJsonObject &design, QString *error, PutOutcome *outcome, double now)
{
    if (const QString e = check(design); !e.isEmpty()) {
        if (error) *error = e;
        return false;
    }
    if (now <= 0)
        now = double(QDateTime::currentMSecsSinceEpoch());
    const double modified = design.value(QStringLiteral("modified")).toDouble();
    if (!plausibleStamp(modified, now)) {
        if (error)
            *error = QStringLiteral("its time is years ahead of this computer's clock; check the "
                                    "clock of the device it was edited on");
        return false;
    }
    // What the sender's copy was made from is about this put only.
    QJsonObject d = design;
    const QJsonValue baseValue = d.take(QStringLiteral("base"));
    d.remove(QStringLiteral("desktopOnly"));
    const double base = baseValue.isDouble() && std::isfinite(baseValue.toDouble()) ? baseValue.toDouble() : 0;
    const QString id = d.value(QStringLiteral("id")).toString();
    const QJsonObject have = get(id);
    // A copy this build cannot read (made by a newer phobiCCCP, say) is not
    // known to be older: it is left as it is rather than written over.
    auto unreadable = [&](const QString &which) {
        if (error)
            *error = QStringLiteral("the copy of “%1” kept on this computer cannot be read here (made by a "
                                    "newer phobiCCCP?), so it is left as it is")
                         .arg(which);
        return false;
    };
    if (have.isEmpty() && QFile::exists(path(id)))
        return unreadable(d.value(QStringLiteral("name")).toString());
    if (have.isEmpty())
        return write(d, error);
    double haveModified = have.value(QStringLiteral("modified")).toDouble();
    // A copy kept from before such times were refused loses to any edit, or
    // it would win every sync until its far-off time.
    if (!plausibleStamp(haveModified, now))
        haveModified = 0;
    // Both with vvs: the one made from the other is kept; two edited apart
    // are both kept, the later under the id and the other as a fork.
    if (plausibleStamp(have.value(QStringLiteral("modified")).toDouble(), now)) {
        switch (order(d, have)) {
        case Order::Same:
        case Order::Before:
            return false;
        case Order::After:
            return write(d, error);
        case Order::Unknown:
            break;
        case Order::Concurrent: {
            const bool moved = wins(d, have);
            const QJsonObject &loser = moved ? have : d;
            if (loser.value(QStringLiteral("deleted")).toBool()) {
                // A deletion that lost is dropped, not kept as a design of its own.
                if (moved)
                    return write(d, error);
                if (outcome) outcome->conflict = true;
                return false;
            }
            const QString forkId = forkIdFor(loser);
            if (QFile::exists(path(forkId))) {
                // Made already (here, or elsewhere and brought by a pad): it
                // holds that copy, or one made from it since.
                if (get(forkId).isEmpty())
                    return unreadable(QStringLiteral("%1 (conflict)").arg(loser.value(QStringLiteral("name")).toString().left(60)));
            } else {
                QJsonObject fork = loser;
                fork.insert(QStringLiteral("id"), forkId);
                fork.insert(QStringLiteral("name"),
                            QStringLiteral("%1 (conflict)").arg(loser.value(QStringLiteral("name")).toString().left(60)));
                if (!write(fork, error))
                    return false;
            }
            if (moved && !write(d, error))
                return false;
            if (outcome) {
                outcome->forked = forkId;
                outcome->moved = moved;
            }
            return true;
        }
        }
    }
    // Changed here (or by another pad through here) since the copy the
    // sender edited: newest-wins would throw one of the two edits away. A
    // base newer than this copy only means this computer is behind.
    if (base > 0 && haveModified > base && haveModified != modified) {
        if (d.value(QStringLiteral("deleted")).toBool()) {
            if (outcome) outcome->conflict = true;
            return false;
        }
        // The same edit sent again (its answer was lost, or a send and a sync
        // both carried it) lands on the same fork rather than making another.
        const QString name = d.value(QStringLiteral("name")).toString();
        const QString forkId = forkIdFor(id, modified, base);
        if (QFile::exists(path(forkId))) {
            if (get(forkId).isEmpty())
                return unreadable(QStringLiteral("%1 (pad)").arg(name.left(60)));
            if (outcome) outcome->forked = forkId;
            return true;
        }
        d.insert(QStringLiteral("id"), forkId);
        d.insert(QStringLiteral("name"), QStringLiteral("%1 (pad)").arg(name.left(60)));
        if (!write(d, error))
            return false;
        if (outcome) outcome->forked = forkId;
        return true;
    }
    if (haveModified >= modified)
        return false;
    return write(d, error);
}

bool Pads::seen(const QJsonObject &pad, const QString &address, qint64 now)
{
    const QString id = pad.value(QStringLiteral("id")).toString();
    if (!validId(id))
        return false;
    QString name = pad.value(QStringLiteral("name")).toString().simplified().left(60);
    if (name.isEmpty())
        name = address;
    // Anyone on the network can claim to be a pad: only so many are kept.
    if (!m_pads.contains(id) && m_pads.size() >= 64) {
        auto oldest = m_pads.begin();
        for (auto it = m_pads.begin(); it != m_pads.end(); ++it)
            if (it->lastSeen < oldest->lastSeen)
                oldest = it;
        m_open.remove(oldest.key());
        m_pads.erase(oldest);
    }
    m_pads.insert(id, Pad{id, name, address, now});
    return true;
}

QList<Pads::Pad> Pads::recent(qint64 now) const
{
    QList<Pad> out;
    for (const Pad &p : m_pads)
        if (now - p.lastSeen <= kRecentMs)
            out.append(p);
    std::sort(out.begin(), out.end(), [](const Pad &a, const Pad &b) {
        return a.lastSeen != b.lastSeen ? a.lastSeen > b.lastSeen : a.id < b.id;
    });
    return out;
}

QString Pads::nameOf(const QString &padId) const
{
    return m_pads.value(padId).name;
}

qint64 Pads::sendOpen(const QString &padId, const QString &designId, const QString &name,
                      const QString &from, qint64 now)
{
    // Numbered from the clock, so a pad that remembers the last one it opened
    // from here does not take a request after a restart for that one.
    m_seq = std::max(m_seq + 1, now);
    m_open.insert(padId, Open{m_seq, now, designId, name, from});
    return m_seq;
}

QJsonObject Pads::pendingFor(const QString &padId, qint64 now) const
{
    const auto it = m_open.constFind(padId);
    if (it == m_open.cend() || now - it->at > kOpenMs)
        return {};
    return QJsonObject{{QStringLiteral("seq"), double(it->seq)},
                       {QStringLiteral("design"), it->design},
                       {QStringLiteral("name"), it->name},
                       {QStringLiteral("from"), it->from}};
}

bool Pads::opened(const QString &padId, qint64 seq, QString *name)
{
    const auto it = m_open.find(padId);
    if (it == m_open.end() || it->seq != seq)
        return false;
    if (name)
        *name = it->name;
    m_open.erase(it);
    return true;
}

bool Pads::waiting(const QString &padId, qint64 seq) const
{
    const auto it = m_open.constFind(padId);
    return it != m_open.cend() && it->seq == seq;
}

} // namespace c2d::padlink
