#include "toolcollections.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>
#include "stockstamp.h"

namespace c2d {

static const char *kSettingsKey = "tools/collections";

ToolCollections ToolCollections::defaults()
{
    ToolCollections c;
    c.add(QStringLiteral("My Tools"));
    c.add(QStringLiteral("Unused"));
    return c;
}

int ToolCollections::indexOf(const QString &name) const
{
    for (int i = 0; i < m_list.size(); ++i)
        if (m_list[i].name == name)
            return i;
    return -1;
}

const ToolCollection *ToolCollections::byName(const QString &name) const
{
    const int i = indexOf(name);
    return i >= 0 ? &m_list[i] : nullptr;
}

bool ToolCollections::add(const QString &name)
{
    const QString n = name.trimmed();
    if (n.isEmpty() || indexOf(n) >= 0)
        return false;
    m_list.append({n, {}});
    return true;
}

bool ToolCollections::rename(const QString &from, const QString &to)
{
    const int i = indexOf(from);
    const QString n = to.trimmed();
    if (i < 0 || n.isEmpty() || (n != from && indexOf(n) >= 0))
        return false;
    m_list[i].name = n;
    return true;
}

void ToolCollections::remove(const QString &name)
{
    const int i = indexOf(name);
    if (i >= 0)
        m_list.remove(i);
}

void ToolCollections::addTool(const QString &name, int number)
{
    const int i = indexOf(name);
    if (i >= 0 && !m_list[i].tools.contains(number))
        m_list[i].tools.append(number);
}

void ToolCollections::removeTool(const QString &name, int number)
{
    const int i = indexOf(name);
    if (i >= 0)
        m_list[i].tools.removeAll(number);
}

void ToolCollections::merge(const ToolCollections &other)
{
    for (const ToolCollection &c : other.m_list) {
        const int i = indexOf(c.name);
        if (i >= 0)
            m_list[i] = c;
        else
            m_list.append(c);
    }
}

QByteArray ToolCollections::toJson() const
{
    QJsonArray arr;
    for (const ToolCollection &c : m_list) {
        QJsonArray tools;
        for (int n : c.tools)
            tools.append(n);
        arr.append(QJsonObject{{QStringLiteral("name"), c.name},
                               {QStringLiteral("tools"), tools}});
    }
    return QJsonDocument(QJsonObject{{QStringLiteral("collections"), arr}})
        .toJson(QJsonDocument::Indented);
}

bool ToolCollections::fromJson(const QByteArray &json, ToolCollections *out, QString *err)
{
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (!doc.isObject() || !doc.object().value(QStringLiteral("collections")).isArray()) {
        if (err)
            *err = pe.error != QJsonParseError::NoError
                ? pe.errorString()
                : QStringLiteral("not a tool collections file");
        return false;
    }
    ToolCollections c;
    for (const QJsonValue &v : doc.object().value(QStringLiteral("collections")).toArray()) {
        const QJsonObject o = v.toObject();
        const QString name = o.value(QStringLiteral("name")).toString().trimmed();
        if (!c.add(name))
            continue;                       // unnamed or a repeated name
        for (const QJsonValue &n : o.value(QStringLiteral("tools")).toArray())
            if (n.isDouble())
                c.addTool(name, n.toInt());
    }
    *out = c;
    return true;
}

bool ToolCollections::saveFile(const QString &path, QString *err) const
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(toJson()) < 0 || !f.commit()) {
        if (err)
            *err = f.errorString();
        return false;
    }
    return true;
}

bool ToolCollections::loadFile(const QString &path, ToolCollections *out, QString *err)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err)
            *err = f.errorString();
        return false;
    }
    return fromJson(f.readAll(), out, err);
}

ToolCollections ToolCollections::loadSettings()
{
    const QByteArray json = QSettings().value(QLatin1String(kSettingsKey)).toByteArray();
    ToolCollections c;
    if (json.isEmpty() || !fromJson(json, &c))
        return defaults();
    return c;
}

void ToolCollections::saveSettings() const
{
    QSettings().setValue(QLatin1String(kSettingsKey), toJson());
    stampCollections();
}

} // namespace c2d
