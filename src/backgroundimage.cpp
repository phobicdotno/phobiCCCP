#include "backgroundimage.h"
#include "zlibutil.h"

#include <QBuffer>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

namespace c2d {

void BackgroundImage::setWidthMm(double w)
{
    if (image.width() <= 0 || w <= 0)
        return;
    scale = w / (image.width() * kMmPerPixel);
}

QRectF BackgroundImage::rectMm() const
{
    return QRectF(x, y, widthMm(), heightMm());
}

QTransform BackgroundImage::pixelToMm() const
{
    // pixel (px, py) -> (x + s*px, y + s*(H - py)), then rotate about the
    // rectangle centre. Built right-to-left with QTransform's post-multiply.
    const double s = mmPerPixel();
    const QRectF r = rectMm();
    QTransform t;
    t.translate(r.center().x(), r.center().y());
    t.rotate(rotationDeg);
    t.translate(-r.center().x(), -r.center().y());
    t.translate(x, y);
    t.scale(s, -s);
    t.translate(0, -image.height());
    return t;
}

bool BackgroundImage::setImageFile(const QString &path, QString *error)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QImage img = reader.read();
    if (img.isNull()) {
        if (error) *error = reader.errorString().isEmpty()
                ? QStringLiteral("Could not read image %1").arg(path)
                : reader.errorString();
        return false;
    }
    setImage(img);
    if (QFileInfo(path).suffix().compare(QLatin1String("png"), Qt::CaseInsensitive) == 0) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly))
            pngData = f.readAll();   // keep the original bytes verbatim
    }
    return true;
}

void BackgroundImage::setImage(const QImage &img)
{
    image = img;
    pngData.clear();
    if (!img.isNull()) {
        QBuffer buf(&pngData);
        buf.open(QIODevice::WriteOnly);
        img.save(&buf, "PNG");
    }
    m_cache = QPixmap();
    m_cacheKey = 0;
}

void BackgroundImage::clear()
{
    image = QImage();
    pngData.clear();
    visible = false;
    x = y = 0;
    scale = 1.0;
    rotationDeg = 0;
    opacity = 0.5;
    locked = false;
    m_cache = QPixmap();
    m_cacheKey = 0;
}

// ---- container persistence -------------------------------------------------

bool BackgroundImage::loadFrom(const QString &c2dPath, QString *error)
{
    clear();
    if (!QFileInfo::exists(c2dPath)) {
        if (error) *error = QStringLiteral("File not found: %1").arg(c2dPath);
        return false;
    }
    const QString conn = QStringLiteral("c2dbg_%1").arg(QUuid::createUuid().toString());
    bool ok = true;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(c2dPath);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (!db.open()) {
            if (error) *error = db.lastError().text();
            QSqlDatabase::removeDatabase(conn);
            return false;
        }
        QSqlQuery q(db);
        q.exec(QStringLiteral("SELECT key, value FROM params WHERE key LIKE 'background_%'"));
        while (q.next()) {
            const QString k = q.value(0).toString();
            const QString v = q.value(1).toString();
            if (k == QLatin1String("background_visible"))         visible = (v.trimmed() == QLatin1String("1") || v.trimmed().compare(QLatin1String("true"), Qt::CaseInsensitive) == 0);
            else if (k == QLatin1String("background_scale"))      scale = v.toDouble();
            else if (k == QLatin1String("background_position_x")) x = v.toDouble();
            else if (k == QLatin1String("background_position_y")) y = v.toDouble();
            else if (k == QLatin1String("background_rotation"))   rotationDeg = v.toDouble();
            else if (k == QLatin1String("background_opacity"))    opacity = v.toDouble();
            else if (k == QLatin1String("background_locked"))     locked = (v.trimmed() == QLatin1String("1"));
        }
        if (scale <= 0)
            scale = 1.0;
        opacity = qBound(0.0, opacity, 1.0);

        QSqlQuery s(db);
        s.exec(QStringLiteral("SELECT sz, data FROM sqlar WHERE name='background.png'"));
        if (s.next()) {
            const int sz = s.value(0).toInt();
            QByteArray blob = s.value(1).toByteArray();
            if (sz > blob.size() && !blob.isEmpty())
                blob = zlibInflate(blob, sz);     // sqlar convention: compressed row
            if (!blob.isEmpty()) {
                QImage img;
                if (img.loadFromData(blob)) {
                    image = img;
                    pngData = blob;
                } else {
                    ok = false;
                    if (error) *error = QStringLiteral("background.png could not be decoded");
                }
            }
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(conn);
    if (image.isNull())
        visible = false;
    return ok;
}

bool BackgroundImage::saveTo(const QString &c2dPath, QString *error) const
{
    const QString conn = QStringLiteral("c2dbgw_%1").arg(QUuid::createUuid().toString());
    bool ok = true;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(c2dPath);
        if (!db.open()) {
            if (error) *error = db.lastError().text();
            QSqlDatabase::removeDatabase(conn);
            return false;
        }
        db.transaction();

        QSqlQuery p(db);
        p.prepare(QStringLiteral("INSERT OR REPLACE INTO params(key, value) VALUES(?, ?)"));
        const QVector<QPair<QString, QString>> kv = {
            {QStringLiteral("background_visible"),    (visible && !image.isNull()) ? QStringLiteral("1") : QStringLiteral("0")},
            {QStringLiteral("background_scale"),      QString::number(scale, 'g', 12)},
            {QStringLiteral("background_position_x"), QString::number(x, 'g', 12)},
            {QStringLiteral("background_position_y"), QString::number(y, 'g', 12)},
            {QStringLiteral("background_rotation"),   QString::number(rotationDeg, 'g', 12)},
            {QStringLiteral("background_opacity"),    QString::number(opacity, 'g', 12)},
            {QStringLiteral("background_locked"),     locked ? QStringLiteral("1") : QStringLiteral("0")},
        };
        for (const auto &e : kv) {
            p.addBindValue(e.first);
            p.addBindValue(e.second);
            if (!p.exec()) {
                if (error) *error = p.lastError().text();
                ok = false;
                break;
            }
        }

        if (ok) {
            // sqlar row stored raw (sz == length), mtime in CC's text form.
            const QByteArray data = image.isNull() ? QByteArray() : pngData;
            const QString mtime = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
            QSqlQuery s(db);
            s.prepare(QStringLiteral(
                "INSERT OR REPLACE INTO sqlar(name, mode, mtime, sz, data) "
                "VALUES('background.png', 33188, ?, ?, ?)"));
            s.addBindValue(mtime);
            s.addBindValue(data.size());
            s.addBindValue(data);
            if (!s.exec()) {
                if (error) *error = s.lastError().text();
                ok = false;
            }
        }

        if (ok) db.commit(); else db.rollback();
        db.close();
    }
    QSqlDatabase::removeDatabase(conn);
    return ok;
}

// ---- painting -------------------------------------------------------------

void BackgroundImage::paint(QPainter *p) const
{
    if (!visible || image.isNull())
        return;
    if (m_cache.isNull() || m_cacheKey != image.cacheKey()) {
        m_cache = QPixmap::fromImage(image);
        m_cacheKey = image.cacheKey();
    }
    p->save();
    p->setOpacity(qBound(0.0, opacity, 1.0));
    p->setRenderHint(QPainter::SmoothPixmapTransform, true);
    p->setTransform(pixelToMm(), true);
    p->drawPixmap(0, 0, m_cache);
    p->restore();
}

} // namespace c2d
