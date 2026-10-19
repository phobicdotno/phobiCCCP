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
