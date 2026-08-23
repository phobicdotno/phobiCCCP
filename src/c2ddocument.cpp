#include "c2ddocument.h"
#include "zlibutil.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QUuid>
#include <QVariant>

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

bool Document::save(const QString &destPath, QString *error)
{
    if (m_path.isEmpty()) {
        if (error) *error = QStringLiteral("No source document loaded to save from.");
        return false;
    }

    // 1) Clone the source container so every structural detail CC expects
    //    (schema, layer, model, toolpaths, params) is inherited verbatim.
    if (QFileInfo(destPath) != QFileInfo(m_path)) {
        QFile::remove(destPath);
        if (!QFile::copy(m_path, destPath)) {
            if (error) *error = QStringLiteral("Could not copy container to %1").arg(destPath);
            return false;
        }
        QFile::setPermissions(destPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                        QFileDevice::ReadGroup | QFileDevice::ReadOther);
    }

    const QString conn = QStringLiteral("c2dw_%1").arg(QUuid::createUuid().toString());
    bool ok = true;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(destPath);
        if (!db.open()) {
            if (error) *error = db.lastError().text();
            QSqlDatabase::removeDatabase(conn);
            return false;
        }

        db.transaction();

        // 2) Drop existing element rows (toolpaths/layer/model untouched).
        QSqlQuery del(db);
        if (!del.exec(QStringLiteral("DELETE FROM items WHERE type='element'"))) {
            if (error) *error = del.lastError().text();
            ok = false;
        }

        // 3) Re-insert each element as zlib(J1 JSON); sz = uncompressed length.
        if (ok) {
            QSqlQuery ins(db);
            ins.prepare(QStringLiteral(
                "INSERT INTO items(uuid,name,type,version,sz,data) "
                "VALUES(?,?,?,?,?,?)"));
            for (const Element &e : m_elements) {
                const QByteArray json = e.toJson();
                const QByteArray comp = zlibDeflate(json);
                ins.addBindValue(e.id);
                ins.addBindValue(e.geometryType);       // items.name mirrors the type
                ins.addBindValue(QStringLiteral("element"));
                ins.addBindValue(QStringLiteral("J1"));
                ins.addBindValue(json.size());
                ins.addBindValue(comp);
                if (!ins.exec()) {
                    if (error) *error = ins.lastError().text();
                    ok = false;
                    break;
                }
            }
        }

        // 4) Blank stale renders / g-code so CC regenerates them on next open.
        if (ok) {
            QSqlQuery blank(db);
            blank.exec(QStringLiteral(
                "UPDATE sqlar SET sz=0, data=x'' "
                "WHERE name IN ('all.svg','preview.svg','gcode.egc')"));
        }

        if (ok)
            db.commit();
        else
            db.rollback();

        db.close();
    }
    QSqlDatabase::removeDatabase(conn);
    return ok;
}

} // namespace c2d
