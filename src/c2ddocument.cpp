#include "c2ddocument.h"
#include "zlibutil.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QUuid>

namespace c2d {

bool Document::load(const QString &path, QString *error)
{
    m_params.clear();
    m_elements.clear();
    m_toolpaths.clear();
    m_path = path;

    if (!QFileInfo::exists(path)) {
        if (error) *error = QStringLiteral("File not found: %1").arg(path);
        return false;
    }

    // Unique connection name so multiple documents can be open at once.
    const QString conn = QStringLiteral("c2d_%1").arg(QUuid::createUuid().toString());
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(path);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (!db.open()) {
            if (error) *error = db.lastError().text();
            QSqlDatabase::removeDatabase(conn);
            return false;
        }

        // params
        {
            QSqlQuery q(db);
            q.exec(QStringLiteral("SELECT key, value FROM params"));
            while (q.next())
                m_params.insert(q.value(0).toString(), q.value(1).toString());
        }

        // items: elements + toolpaths
        {
            QSqlQuery q(db);
            q.exec(QStringLiteral(
                "SELECT type, sz, data FROM items WHERE type IN ('element','toolpath')"));
            while (q.next()) {
                const QString type = q.value(0).toString();
                const int sz = q.value(1).toInt();
                const QByteArray blob = q.value(2).toByteArray();
                const QByteArray json = zlibInflate(blob, sz);
                const QJsonObject obj = QJsonDocument::fromJson(json).object();
                if (obj.isEmpty())
                    continue;

                if (type == QLatin1String("element")) {
                    m_elements.append(Element::fromJson(obj));
                } else {
                    Toolpath tp;
                    tp.uuid = obj.value("uuid").toString();
                    tp.type = obj.value("type").toString();
                    tp.json = obj;
                    m_toolpaths.append(tp);
                }
            }
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(conn);
    return true;
}

} // namespace c2d
