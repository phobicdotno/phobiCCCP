#include "toolpathtemplates.h"
#include "parametric.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>

namespace c2d {
namespace tptemplates {

QString folder()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                        + QStringLiteral("/templates");
    QDir().mkpath(dir);
    return dir;
}

QString fileFor(const QString &name)
{
    QString safe = name.trimmed();
    safe.replace(QRegularExpression(QStringLiteral("[/\\\\:*?\"<>|\\x00-\\x1f]")),
                 QStringLiteral("_"));
    if (safe.isEmpty() || safe.startsWith(QLatin1Char('.')))
        safe.prepend(QLatin1Char('_'));
    return folder() + QLatin1Char('/') + safe + QStringLiteral(".json");
}

QStringList names()
{
    QStringList out;
    const QDir dir(folder());
    for (const QString &f : dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
        QFile file(dir.filePath(f));
        QString n;
        if (file.open(QIODevice::ReadOnly))
            n = QJsonDocument::fromJson(file.readAll()).object().value("name").toString();
        out << (n.isEmpty() ? f.chopped(5) : n);
    }
    out.sort(Qt::CaseInsensitive);
    return out;
}

QJsonObject build(const QString &name, const QVector<Toolpath> &tps)
{
    QJsonArray list;
    for (const Toolpath &t : tps) {
        QJsonObject j = t.json;
        j.remove(QStringLiteral("uuid"));
        j.remove(QStringLiteral("elements"));
        j.remove(QStringLiteral("toolpath_group"));
        list.append(QJsonObject{{QStringLiteral("type"), t.type}, {QStringLiteral("json"), j}});
    }
    return QJsonObject{{QStringLiteral("name"), name.trimmed()},
                       {QStringLiteral("version"), 1},
                       {QStringLiteral("toolpaths"), list}};
}

bool save(const QJsonObject &tpl, QString *error)
{
    QSaveFile f(fileFor(tpl.value("name").toString()));
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    f.write(QJsonDocument(tpl).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
}

bool load(const QString &name, QJsonObject *tpl, QString *error)
{
    QFile f(fileFor(name));
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    QJsonParseError pe;
    const QJsonDocument d = QJsonDocument::fromJson(f.readAll(), &pe);
    if (!d.isObject() || !d.object().value("toolpaths").isArray()) {
        if (error) *error = pe.error != QJsonParseError::NoError
                                ? pe.errorString() : QStringLiteral("not a toolpath template");
        return false;
    }
    *tpl = d.object();
    return true;
}

bool remove(const QString &name)
{
    return QFile::remove(fileFor(name));
}

QVector<Toolpath> instantiate(const Document &doc, const QJsonObject &tpl,
                              const QStringList &elementIds, QStringList *notes)
{
    QSet<QString> params;
    for (const UserParameter &p : doc.parametric().params)
        params.insert(p.name);
    for (const QString &b : param::builtinNames())
        params.insert(b);
    // Named dimensions too (values() holds every name that works out here).
    for (const QString &n : param::values(doc, doc.parametric()).keys())
        params.insert(n);
    QSet<QString> taken;
    for (const Toolpath &t : doc.toolpaths())
        taken.insert(t.json.value("name").toString());
    QJsonArray refs;
    for (const QString &id : elementIds)
        refs.append(QJsonObject{{QStringLiteral("uuid"), id}});

    QVector<Toolpath> out;
    for (const QJsonValue &v : tpl.value("toolpaths").toArray()) {
        const QJsonObject o = v.toObject();
        Toolpath t;
        t.type = o.value("type").toString();
        t.json = o.value("json").toObject();
        if (t.type.isEmpty())
            t.type = t.json.value("type").toString();
        if (t.type.isEmpty())
            continue;
        t.uuid = QUuid::createUuid().toString();
        t.json.insert(QStringLiteral("uuid"), t.uuid);
        t.json.insert(QStringLiteral("type"), t.type);
        t.json.insert(QStringLiteral("elements"), refs);
        t.json.insert(QStringLiteral("toolpath_group"), doc.defaultToolpathGroup());
        QString name = t.json.value("name").toString();
        if (name.isEmpty())
            name = t.type;
        if (taken.contains(name)) {
            int n = 2;
            while (taken.contains(QStringLiteral("%1 (%2)").arg(name).arg(n)))
                ++n;
            name = QStringLiteral("%1 (%2)").arg(name).arg(n);
        }
        taken.insert(name);
        t.json.insert(QStringLiteral("name"), name);
        // Bindings stay when every name they use is in this design.
        QJsonObject binds = t.json.value("phobic_expr").toObject();
        if (!binds.isEmpty()) {
            for (const QString &key : binds.keys()) {
                const QString expr = binds.value(key).toObject().value("expr").toString();
                QStringList missing;
                for (const QString &n : param::namesIn(expr))
                    if (!params.contains(n))
                        missing << n;
                if (!missing.isEmpty()) {
                    binds.remove(key);
                    if (notes)
                        *notes << QStringLiteral("%1: %2 kept its number (no parameter %3 here)")
                                      .arg(name, key, missing.join(QStringLiteral(", ")));
                }
            }
            if (binds.isEmpty())
                t.json.remove(QStringLiteral("phobic_expr"));
            else
                t.json.insert(QStringLiteral("phobic_expr"), binds);
        }
        out << t;
    }
    // Work the kept bindings out against this design now, so a depth bound to
    // the stock follows this stock and not the one the template was saved on.
    Document scratch = doc;
    scratch.toolpathsRef() = out;
    param::applyToolpathBindings(scratch, notes);
    out = scratch.toolpaths();
    return out;
}

} // namespace tptemplates
} // namespace c2d
