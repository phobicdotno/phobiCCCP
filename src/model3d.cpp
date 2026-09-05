#include "model3d.h"
#include "backgroundimage.h"
#include "c2ddocument.h"
#include "zlibutil.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>
#include <QtEndian>
#include <QtMath>

#include <algorithm>
#include <cstring>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

namespace c2d {

namespace {
constexpr double kMinCell = 0.1;
constexpr double kMaxCells = 4.0e6;
const float kNone = std::numeric_limits<float>::quiet_NaN();
inline bool none(float v) { return std::isnan(v); }
}

// ---------------------------------------------------------------------------
// names / JSON

QString ModelComponent::kindName(Kind k)
{
    switch (k) {
    case FromVectors:    return QStringLiteral("Vectors");
    case ImageHeightmap: return QStringLiteral("Image");
    case StlMesh:        return QStringLiteral("STL");
    case Texture:        return QStringLiteral("Texture");
    }
    return QString();
}

QString ModelComponent::shapeName(Shape s)
{
    switch (s) {
    case Flat:   return QStringLiteral("Flat");
    case Round:  return QStringLiteral("Round");
    case Angle:  return QStringLiteral("Angle");
    case Smooth: return QStringLiteral("Smooth");
    case Dome:   return QStringLiteral("Dome");
    }
    return QString();
}

QString ModelComponent::combineName(Combine c)
{
    switch (c) {
    case Add:      return QStringLiteral("Add");
    case Subtract: return QStringLiteral("Subtract");
    case Merge:    return QStringLiteral("Merge");
    case Multiply: return QStringLiteral("Multiply");
    }
    return QString();
}

QJsonObject ModelComponent::toJson() const
{
    QJsonObject o;
    o.insert("id", id);
    o.insert("name", name);
    o.insert("kind", int(kind));
    o.insert("enabled", enabled);
    o.insert("combine", int(combine));
    o.insert("height", height);
    o.insert("base_height", baseHeight);
    o.insert("vectors", QJsonArray::fromStringList(vectorIds));
    o.insert("shape", int(shape));
    o.insert("angle", angleDeg);
    o.insert("source_name", sourceName);
    o.insert("has_data", !data.isEmpty());
    o.insert("invert", invert);
    o.insert("blur", blurPx);
    o.insert("x", x);
    o.insert("y", y);
    o.insert("width", width);
    o.insert("tile_width", tileWidth);
    o.insert("units", int(units));
    return o;
}

ModelComponent ModelComponent::fromJson(const QJsonObject &o)
{
    ModelComponent c;
    c.id = o.value("id").toString();
    c.name = o.value("name").toString();
    c.kind = Kind(qBound(0, o.value("kind").toInt(), int(Texture)));
    c.enabled = o.value("enabled").toBool(true);
    c.combine = Combine(qBound(0, o.value("combine").toInt(), int(Multiply)));
    c.height = o.value("height").toDouble(5.0);
    c.baseHeight = o.value("base_height").toDouble(0.0);
    for (const QJsonValue &v : o.value("vectors").toArray())
        c.vectorIds << v.toString();
    c.shape = Shape(qBound(0, o.value("shape").toInt(int(Round)), int(Dome)));
    c.angleDeg = o.value("angle").toDouble(45.0);
    c.sourceName = o.value("source_name").toString();
    c.invert = o.value("invert").toBool(false);
    c.blurPx = o.value("blur").toDouble(0.0);
    c.x = o.value("x").toDouble(0.0);
    c.y = o.value("y").toDouble(0.0);
    c.width = o.value("width").toDouble(0.0);
    c.tileWidth = o.value("tile_width").toDouble(10.0);
    c.units = Units(qBound(0, o.value("units").toInt(), int(Inches)));
    return c;
}

bool Model3D::hasEnabled() const
{
    for (const ModelComponent &c : components)
        if (c.enabled)
            return true;
    return false;
}

int Model3D::indexOf(const QString &id) const
{
    for (int i = 0; i < components.size(); ++i)
        if (components.at(i).id == id)
            return i;
    return -1;
}

QString Model3D::newId()
{
    return QUuid::createUuid().toString(QUuid::WithBraces);
}

QJsonObject Model3D::toJson() const
{
    QJsonObject o;
    o.insert("version", 1);
    o.insert("resolution", resolution);
    QJsonArray arr;
    for (const ModelComponent &c : components)
        arr.append(c.toJson());
    o.insert("components", arr);
    return o;
}

Model3D Model3D::fromJson(const QJsonObject &o)
{
    Model3D m;
    m.resolution = o.value("resolution").toDouble(0.0);
    for (const QJsonValue &v : o.value("components").toArray()) {
        ModelComponent c = ModelComponent::fromJson(v.toObject());
        if (c.id.isEmpty())
            c.id = newId();
        m.components.append(c);
    }
    return m;
}

// ---------------------------------------------------------------------------
// persistence (sqlar rows, stored raw with sz == length like background.png)

namespace {
const QString kJsonRow = QStringLiteral("phobi_model3d.json");
const QString kBlobPrefix = QStringLiteral("phobi_model3d/");

QByteArray readSqlar(QSqlDatabase &db, const QString &name, bool *found)
{
    QSqlQuery s(db);
    s.prepare(QStringLiteral("SELECT sz, data FROM sqlar WHERE name=?"));
    s.addBindValue(name);
    s.exec();
    if (!s.next()) {
        if (found) *found = false;
        return QByteArray();
    }
    if (found) *found = true;
    const int sz = s.value(0).toInt();
    QByteArray blob = s.value(1).toByteArray();
    if (sz > blob.size() && !blob.isEmpty())
        blob = zlibInflate(blob, sz);
    return blob;
}
}

bool Model3D::loadFrom(const QString &c2dPath, QString *error)
{
    components.clear();
    resolution = 0;
    if (!QFileInfo::exists(c2dPath)) {
        if (error) *error = QStringLiteral("File not found: %1").arg(c2dPath);
        return false;
    }
    const QString conn = QStringLiteral("c2dm3_%1").arg(QUuid::createUuid().toString());
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
        bool found = false;
        const QByteArray json = readSqlar(db, kJsonRow, &found);
        if (found && !json.isEmpty()) {
            QJsonParseError pe;
            const QJsonDocument jd = QJsonDocument::fromJson(json, &pe);
            if (pe.error != QJsonParseError::NoError) {
                ok = false;
                if (error) *error = QStringLiteral("phobi_model3d.json: %1").arg(pe.errorString());
            } else {
                *this = fromJson(jd.object());
                for (ModelComponent &c : components) {
                    bool have = false;
                    const QByteArray blob = readSqlar(db, kBlobPrefix + c.id, &have);
                    if (have)
                        c.data = blob;
                }
            }
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(conn);
    return ok;
}

bool Model3D::saveTo(const QString &c2dPath, QString *error) const
{
    const QString conn = QStringLiteral("c2dm3w_%1").arg(QUuid::createUuid().toString());
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
        QSqlQuery mk(db);
        mk.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS sqlar("
                               "name TEXT PRIMARY KEY, mode INT, mtime INT, sz INT, data BLOB)"));
        QSqlQuery del(db);
        del.prepare(QStringLiteral("DELETE FROM sqlar WHERE name=? OR name LIKE ?"));
        del.addBindValue(kJsonRow);
        del.addBindValue(kBlobPrefix + QStringLiteral("%"));
        if (!del.exec()) {
            if (error) *error = del.lastError().text();
            ok = false;
        }
        const QString mtime = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        auto put = [&](const QString &name, const QByteArray &bytes) {
            QSqlQuery s(db);
            s.prepare(QStringLiteral(
                "INSERT OR REPLACE INTO sqlar(name, mode, mtime, sz, data) VALUES(?, 33188, ?, ?, ?)"));
            s.addBindValue(name);
            s.addBindValue(mtime);
            s.addBindValue(bytes.size());
            s.addBindValue(bytes);
            if (!s.exec()) {
                if (error) *error = s.lastError().text();
                ok = false;
            }
        };
        // A file without components carries no row at all (a plain document).
        if (ok && !components.isEmpty()) {
            put(kJsonRow, QJsonDocument(toJson()).toJson(QJsonDocument::Indented));
            for (const ModelComponent &c : components)
                if (ok && !c.data.isEmpty())
                    put(kBlobPrefix + c.id, c.data);
        }
        if (ok) db.commit(); else db.rollback();
        db.close();
    }
    QSqlDatabase::removeDatabase(conn);
    return ok;
}

// ---------------------------------------------------------------------------
// building blocks

double modelCellSize(double boardW, double boardH, double requested)
{
    double cell = requested > 0 ? requested : kMinCell;
    if (requested <= 0 && boardW > 0 && boardH > 0)
        cell = std::sqrt(boardW * boardH / kMaxCells);
    cell = std::max(cell, kMinCell);
    if (boardW > 0 && boardH > 0) {
        const double need = std::sqrt(boardW * boardH / kMaxCells);
        if (cell < need)
            cell = need;
    }
    return cell;
}

namespace {
// Felzenszwalb & Huttenlocher 1D squared distance transform.
void dt1d(const float *f, int n, float *d, std::vector<int> &v, std::vector<float> &z)
{
    v.resize(n);
    z.resize(n + 1);
    const float INF = std::numeric_limits<float>::infinity();
    int k = 0;
    v[0] = 0;
    z[0] = -INF;
    z[1] = INF;
    for (int q = 1; q < n; ++q) {
        float s;
        for (;;) {
            const int vk = v[k];
            s = ((f[q] + float(q) * q) - (f[vk] + float(vk) * vk)) / (2.0f * (q - vk));
            if (s <= z[k] && k > 0) { --k; continue; }
            if (s <= z[k] && k == 0) { s = -INF; }
            break;
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = INF;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < q) ++k;
        const float dq = float(q) - v[k];
        d[q] = dq * dq + f[v[k]];
    }
}
}

QVector<float> distanceTransform(const QVector<unsigned char> &inside, int cols, int rows)
{
    QVector<float> d(cols * rows, 0.0f);
    if (cols <= 0 || rows <= 0 || inside.size() != cols * rows)
        return d;
    // Border padding: the grid edge counts as outside, so use an (n+2) frame.
    const int W = cols + 2, H = rows + 2;
    const float INF = 1e20f;
    std::vector<float> g(size_t(W) * H, 0.0f);
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c)
            g[size_t(r + 1) * W + (c + 1)] = inside.at(r * cols + c) ? INF : 0.0f;
    std::vector<int> v;
    std::vector<float> z;
    std::vector<float> col(H), out(H);
    // columns
    for (int c = 0; c < W; ++c) {
        for (int r = 0; r < H; ++r) col[r] = g[size_t(r) * W + c];
        dt1d(col.data(), H, out.data(), v, z);
        for (int r = 0; r < H; ++r) g[size_t(r) * W + c] = out[r];
    }
    // rows
    std::vector<float> rowv(W), rowo(W);
    for (int r = 0; r < H; ++r) {
        for (int c = 0; c < W; ++c) rowv[c] = g[size_t(r) * W + c];
        dt1d(rowv.data(), W, rowo.data(), v, z);
        for (int c = 0; c < W; ++c) g[size_t(r) * W + c] = rowo[c];
    }
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c)
            d[r * cols + c] = inside.at(r * cols + c)
                                  ? std::sqrt(g[size_t(r + 1) * W + (c + 1)]) : 0.0f;
    return d;
}

GrayField grayField(const QImage &img, bool invert, double sigmaPx)
{
    GrayField g;
    if (img.isNull())
        return g;
    const QImage gray = img.convertToFormat(QImage::Format_Grayscale8);
    g.cols = gray.width();
    g.rows = gray.height();
    g.v.resize(g.cols * g.rows);
    for (int y = 0; y < g.rows; ++y) {
        const uchar *line = gray.constScanLine(g.rows - 1 - y);   // row 0 = bottom
        float *dst = g.v.data() + y * g.cols;
        for (int x = 0; x < g.cols; ++x) {
            const float t = line[x] / 255.0f;
            dst[x] = invert ? 1.0f - t : t;
        }
    }
    if (sigmaPx > 0.05) {
        const int rad = std::max(1, int(std::ceil(sigmaPx * 3)));
        std::vector<float> k(2 * rad + 1);
        float sum = 0;
        for (int i = -rad; i <= rad; ++i) {
            k[i + rad] = std::exp(-0.5f * float(i * i) / float(sigmaPx * sigmaPx));
            sum += k[i + rad];
        }
        for (float &w : k) w /= sum;
        QVector<float> tmp(g.v.size());
        // horizontal
        for (int y = 0; y < g.rows; ++y)
            for (int x = 0; x < g.cols; ++x) {
                float acc = 0;
                for (int i = -rad; i <= rad; ++i) {
                    const int xx = qBound(0, x + i, g.cols - 1);
                    acc += k[i + rad] * g.v[y * g.cols + xx];
                }
                tmp[y * g.cols + x] = acc;
            }
        // vertical
        for (int y = 0; y < g.rows; ++y)
            for (int x = 0; x < g.cols; ++x) {
                float acc = 0;
                for (int i = -rad; i <= rad; ++i) {
                    const int yy = qBound(0, y + i, g.rows - 1);
                    acc += k[i + rad] * tmp[yy * g.cols + x];
                }
                g.v[y * g.cols + x] = acc;
            }
    }
    return g;
}

namespace {
// Bilinear sample of a gray field at continuous pixel coordinates (0..cols,
// 0..rows, Y-up), clamped at the edges.
float sampleGray(const GrayField &g, double px, double py)
{
    const double fx = qBound(0.0, px - 0.5, double(g.cols - 1));
    const double fy = qBound(0.0, py - 0.5, double(g.rows - 1));
    const int c0 = int(fx), r0 = int(fy);
    const int c1 = std::min(c0 + 1, g.cols - 1), r1 = std::min(r0 + 1, g.rows - 1);
    const double tx = fx - c0, ty = fy - r0;
    const double a = g.at(c0, r0) * (1 - tx) + g.at(c1, r0) * tx;
    const double b = g.at(c0, r1) * (1 - tx) + g.at(c1, r1) * tx;
    return float(a * (1 - ty) + b * ty);
}
}

bool parseStl(const QByteArray &bytes, QVector<StlTriangle> *tris, QString *error)
{
    tris->clear();
    if (bytes.size() < 15) {
        if (error) *error = QStringLiteral("Not an STL file (too short)");
        return false;
    }
    // Binary: 80-byte header, uint32 count, 50 bytes per triangle. Size check
    // is the reliable discriminator; "solid" headers exist on binary files.
    bool binary = false;
    if (bytes.size() >= 84) {
        const quint32 n = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData() + 80));
        if (qint64(84) + qint64(n) * 50 == bytes.size())
            binary = true;
    }
    if (binary) {
        const quint32 n = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData() + 80));
        tris->reserve(int(n));
        const uchar *p = reinterpret_cast<const uchar *>(bytes.constData()) + 84;
        for (quint32 i = 0; i < n; ++i, p += 50) {
            StlTriangle t;
            for (int v = 0; v < 3; ++v)
                for (int k = 0; k < 3; ++k) {
                    quint32 bits = qFromLittleEndian<quint32>(p + 12 + v * 12 + k * 4);
                    float f;
                    std::memcpy(&f, &bits, 4);
                    t.v[v][k] = f;
                }
            tris->append(t);
        }
        return true;
    }
    // ASCII
    const QString text = QString::fromLatin1(bytes);
    if (!text.trimmed().startsWith(QLatin1String("solid"), Qt::CaseInsensitive)) {
        if (error) *error = QStringLiteral("Not an STL file (no binary size match, no 'solid' header)");
        return false;
    }
    StlTriangle cur{};
    int nv = 0;
    const QStringList lines = text.split(QChar('\n'), Qt::SkipEmptyParts);
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (!line.startsWith(QLatin1String("vertex"), Qt::CaseInsensitive))
            continue;
        const QStringList parts = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (parts.size() < 4)
            continue;
        for (int k = 0; k < 3; ++k)
            cur.v[nv][k] = parts.at(1 + k).toFloat();
        if (++nv == 3) {
            tris->append(cur);
            nv = 0;
        }
    }
    if (tris->isEmpty()) {
        if (error) *error = QStringLiteral("STL contains no triangles");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// compositing

namespace {

struct Work {
    int cols = 0, rows = 0;
    double cell = 1;
    QVector<float> h;          // relief height above the floor, mm; NaN = nothing modelled
    double cx(int c) const { return (c + 0.5) * cell; }
    double cy(int r) const { return (r + 0.5) * cell; }
};

QVector<unsigned char> rasterizeVectors(const Work &w, const QStringList &ids,
                                        const QVector<Element> &elements)
{
    QImage img(w.cols, w.rows, QImage::Format_Grayscale8);
    img.fill(0);
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::white);
        p.translate(0, w.rows);
        p.scale(1.0 / w.cell, -1.0 / w.cell);
        for (const QString &id : ids)
            for (const Element &e : elements)
                if (e.id == id && !e.painterPath.isEmpty()) {
                    p.drawPath(e.painterPath);
                    break;
                }
    }
    QVector<unsigned char> m(w.cols * w.rows, 0);
    for (int r = 0; r < w.rows; ++r) {
        const uchar *line = img.constScanLine(w.rows - 1 - r);
        for (int c = 0; c < w.cols; ++c)
            m[r * w.cols + c] = line[c] > 127 ? 1 : 0;
    }
    return m;
}

// Profile height 0..height as a function of the inside distance d (mm) and
// the region's inradius dmax (mm).
double profileAt(const ModelComponent &comp, double d, double dmax)
{
    if (comp.shape == ModelComponent::Flat)
        return comp.height;
    if (comp.shape == ModelComponent::Angle) {
        const double slope = std::tan(qDegreesToRadians(qBound(0.5, comp.angleDeg, 89.5)));
        return std::min(comp.height, d * slope);
    }
    const double t = dmax > 0 ? qBound(0.0, d / dmax, 1.0) : 1.0;
    switch (comp.shape) {
    case ModelComponent::Round:  return comp.height * std::sqrt(std::max(0.0, 2 * t - t * t));
    case ModelComponent::Smooth: return comp.height * (1 - std::cos(M_PI * t)) / 2;
    case ModelComponent::Dome:   return comp.height * (2 * t - t * t);
    default: break;
    }
    return comp.height;
}

// field: component surface (mm above the floor) per cell, NaN outside its region.
void combineInto(Work &w, const ModelComponent &comp, const QVector<float> &field)
{
    const int n = w.cols * w.rows;
    const double full = comp.baseHeight + std::fabs(comp.height);
    for (int i = 0; i < n; ++i) {
        const float f = field.at(i);
        if (none(f))
            continue;
        float &h = w.h[i];
        switch (comp.combine) {
        case ModelComponent::Add:
            h = (none(h) ? 0.0f : h) + f;
            break;
        case ModelComponent::Subtract:
            h = std::max(0.0f, (none(h) ? 0.0f : h) - f);
            break;
        case ModelComponent::Merge:
            h = none(h) ? f : std::max(h, f);
            break;
        case ModelComponent::Multiply:
            if (!none(h))
                h = float(h * (full > 0 ? qBound(0.0, double(f) / full, 1.0) : 1.0));
            break;
        }
    }
}

void applyVectors(Work &w, const ModelComponent &comp, const QVector<Element> &elements,
                  std::atomic<bool> *cancel)
{
    const QVector<unsigned char> mask = rasterizeVectors(w, comp.vectorIds, elements);
    const int n = w.cols * w.rows;
    QVector<float> field(n, kNone);
    if (comp.shape == ModelComponent::Flat) {
        for (int i = 0; i < n; ++i)
            if (mask.at(i))
                field[i] = float(comp.baseHeight + comp.height);
    } else {
        if (cancel && *cancel) return;
        const QVector<float> d = distanceTransform(mask, w.cols, w.rows);
        float dmaxCells = 0;
        for (int i = 0; i < n; ++i)
            dmaxCells = std::max(dmaxCells, d.at(i));
        // The boundary sits half a cell beyond the outermost inside cell.
        const double dmax = std::max(0.0, dmaxCells - 0.5) * w.cell;
        for (int i = 0; i < n; ++i)
            if (mask.at(i)) {
                const double dm = std::max(0.0, d.at(i) - 0.5) * w.cell;
                field[i] = float(comp.baseHeight + profileAt(comp, dm, dmax));
            }
    }
    combineInto(w, comp, field);
}

void applyImage(Work &w, const ModelComponent &comp)
{
    QImage img;
    if (!img.loadFromData(comp.data))
        return;
    const GrayField g = grayField(img, comp.invert, comp.blurPx);
    if (!g.valid())
        return;
    const double wmm = comp.width > 0 ? comp.width : g.cols * BackgroundImage::kMmPerPixel;
    const double hmm = wmm * g.rows / g.cols;
    const int n = w.cols * w.rows;
    QVector<float> field(n, kNone);
    for (int r = 0; r < w.rows; ++r) {
        const double y = w.cy(r);
        if (y < comp.y || y >= comp.y + hmm) continue;
        for (int c = 0; c < w.cols; ++c) {
            const double x = w.cx(c);
            if (x < comp.x || x >= comp.x + wmm) continue;
            const double px = (x - comp.x) / wmm * g.cols;
            const double py = (y - comp.y) / hmm * g.rows;
            field[r * w.cols + c] = float(comp.baseHeight + comp.height * sampleGray(g, px, py));
        }
    }
    combineInto(w, comp, field);
}

void applyTexture(Work &w, const ModelComponent &comp, const QVector<Element> &elements)
{
    QImage img;
    if (!img.loadFromData(comp.data))
        return;
    const GrayField g = grayField(img, comp.invert, comp.blurPx);
    if (!g.valid())
        return;
    const int n = w.cols * w.rows;
    QVector<unsigned char> mask;
    if (!comp.vectorIds.isEmpty()) {
        mask = rasterizeVectors(w, comp.vectorIds, elements);
    } else {
        mask.resize(n);
        for (int i = 0; i < n; ++i)
            mask[i] = none(w.h.at(i)) ? 0 : 1;
    }
    const double tw = comp.tileWidth > 0.01 ? comp.tileWidth : 10.0;
    const double th = tw * g.rows / g.cols;
    QVector<float> field(n, kNone);
    for (int r = 0; r < w.rows; ++r) {
        const double y = w.cy(r) - comp.y;
        for (int c = 0; c < w.cols; ++c) {
            if (!mask.at(r * w.cols + c)) continue;
            const double x = w.cx(c) - comp.x;
            double u = std::fmod(x, tw); if (u < 0) u += tw;
            double v = std::fmod(y, th); if (v < 0) v += th;
            field[r * w.cols + c] =
                float(comp.baseHeight + comp.height * sampleGray(g, u / tw * g.cols, v / th * g.rows));
        }
    }
    combineInto(w, comp, field);
}

void applyStl(Work &w, const ModelComponent &comp, std::atomic<bool> *cancel)
{
    QVector<StlTriangle> tris;
    if (!parseStl(comp.data, &tris))
        return;
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (const StlTriangle &t : tris)
        for (int v = 0; v < 3; ++v)
            for (int k = 0; k < 3; ++k) {
                mn[k] = std::min(mn[k], t.v[v][k]);
                mx[k] = std::max(mx[k], t.v[v][k]);
            }
    const double bw = mx[0] - mn[0], bd = mx[1] - mn[1], bz = mx[2] - mn[2];
    if (!(bw > 0) || !(bd > 0))
        return;
    double s = comp.units == ModelComponent::Inches ? 25.4 : 1.0;
    if (comp.width > 0)
        s = comp.width / bw;
    const double zExtent = comp.height > 0 ? comp.height : bz * s;
    const double zScale = bz > 0 ? zExtent / bz : 0;   // file z -> mm above the mesh floor

    const int n = w.cols * w.rows;
    QVector<float> zbuf(n, kNone);
    auto stamp = [&](int c, int r, float z) {
        if (c < 0 || r < 0 || c >= w.cols || r >= w.rows) return;
        float &cur = zbuf[r * w.cols + c];
        if (none(cur) || z > cur) cur = z;
    };
    int done = 0;
    for (const StlTriangle &t : tris) {
        // Count first: the atomic is only read once every 4096 triangles.
        if ((++done & 4095) == 0 && cancel && *cancel) return;
        double px[3], py[3], pz[3];
        for (int v = 0; v < 3; ++v) {
            px[v] = comp.x + (t.v[v][0] - mn[0]) * s;
            py[v] = comp.y + (t.v[v][1] - mn[1]) * s;
            pz[v] = (t.v[v][2] - mn[2]) * zScale;
        }
        // vertices always land (tiny triangles between cell centres)
        for (int v = 0; v < 3; ++v)
            stamp(int(std::floor(px[v] / w.cell)), int(std::floor(py[v] / w.cell)), float(pz[v]));
        const double det = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
        if (std::fabs(det) < 1e-12)
            continue;
        const int c0 = std::max(0, int(std::floor(std::min({px[0], px[1], px[2]}) / w.cell)));
        const int c1 = std::min(w.cols - 1, int(std::floor(std::max({px[0], px[1], px[2]}) / w.cell)));
        const int r0 = std::max(0, int(std::floor(std::min({py[0], py[1], py[2]}) / w.cell)));
        const int r1 = std::min(w.rows - 1, int(std::floor(std::max({py[0], py[1], py[2]}) / w.cell)));
        for (int r = r0; r <= r1; ++r) {
            const double y = w.cy(r);
            for (int c = c0; c <= c1; ++c) {
                const double x = w.cx(c);
                const double l1 = ((px[1] - x) * (py[2] - y) - (px[2] - x) * (py[1] - y)) / det;
                const double l2 = ((px[2] - x) * (py[0] - y) - (px[0] - x) * (py[2] - y)) / det;
                const double l3 = 1 - l1 - l2;
                const double eps = -1e-9;
                if (l1 < eps || l2 < eps || l3 < eps) continue;
                stamp(c, r, float(l1 * pz[0] + l2 * pz[1] + l3 * pz[2]));
            }
        }
    }
    for (int i = 0; i < n; ++i)
        if (!none(zbuf.at(i)))
            zbuf[i] += float(comp.baseHeight);
    combineInto(w, comp, zbuf);
}

} // namespace

HeightModel buildHeightModel(const Model3D &model, const QVector<Element> &elements,
                             double boardW, double boardH,
                             ModelProgressFn progress, std::atomic<bool> *cancel)
{
    HeightModel hm;
    if (!(boardW > 0) || !(boardH > 0) || !model.hasEnabled())
        return hm;
    Work w;
    w.cell = modelCellSize(boardW, boardH, model.resolution);
    w.cols = std::max(1, int(std::ceil(boardW / w.cell - 1e-9)));
    w.rows = std::max(1, int(std::ceil(boardH / w.cell - 1e-9)));
    w.h = QVector<float>(w.cols * w.rows, kNone);

    int enabled = 0;
    for (const ModelComponent &c : model.components)
        if (c.enabled) ++enabled;
    int k = 0;
    if (progress) progress(0);
    for (const ModelComponent &c : model.components) {
        if (!c.enabled)
            continue;
        if (cancel && *cancel)
            break;
        switch (c.kind) {
        case ModelComponent::FromVectors:    applyVectors(w, c, elements, cancel); break;
        case ModelComponent::ImageHeightmap: applyImage(w, c); break;
        case ModelComponent::StlMesh:        applyStl(w, c, cancel); break;
        case ModelComponent::Texture:        applyTexture(w, c, elements); break;
        }
        ++k;
        if (progress) progress(int(100.0 * k / std::max(1, enabled)));
    }

    // Normalise: peak at 0, floor at -maxH; untouched cells NoModel.
    float maxH = 0;
    bool any = false;
    for (float v : w.h)
        if (!none(v)) { maxH = std::max(maxH, v); any = true; }
    if (!any)
        return hm;
    hm.originX = 0;
    hm.originY = 0;
    hm.cell = w.cell;
    hm.resize(w.cols, w.rows, HeightModel::NoModel);
    hm.baseZ = -double(maxH);
    for (int i = 0; i < w.cols * w.rows; ++i) {
        const float v = w.h.at(i);
        hm.z[i] = none(v) ? HeightModel::NoModel : std::max(0.0f, v) - maxH;
    }
    if (progress) progress(100);
    return hm;
}

HeightModel buildHeightModel(const Model3D &model, const Document &doc,
                             ModelProgressFn progress, std::atomic<bool> *cancel)
{
    return buildHeightModel(model, doc.elements(), doc.boardWidth(), doc.boardHeight(),
                            std::move(progress), cancel);
}

// ---------------------------------------------------------------------------
// rendering

QImage renderHeightModel(const HeightModel &hm)
{
    if (!hm.valid())
        return QImage();
    QImage img(hm.cols, hm.rows, QImage::Format_RGB32);
    const QRgb bg = qRgb(0x22, 0x25, 0x2c);
    const double range = std::max(1e-6, -hm.baseZ);
    // light from the upper left (Y-up: -x, +y), fairly high
    const double lx = -0.45, ly = 0.45, lz = 0.77;
    auto zAt = [&](int c, int r) {
        const float v = hm.at(c, r);
        return v == HeightModel::NoModel ? hm.baseZ : double(v);
    };
    for (int r = 0; r < hm.rows; ++r) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(hm.rows - 1 - r));
        for (int c = 0; c < hm.cols; ++c) {
            const float v = hm.at(c, r);
            if (v == HeightModel::NoModel) { line[c] = bg; continue; }
            const double t = qBound(0.0, (v - hm.baseZ) / range, 1.0);
            const double dzdx = (zAt(c + 1, r) - zAt(c - 1, r)) / (2 * hm.cell);
            const double dzdy = (zAt(c, r + 1) - zAt(c, r - 1)) / (2 * hm.cell);
            double nx = -dzdx, ny = -dzdy, nz = 1;
            const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
            nx /= len; ny /= len; nz /= len;
            const double shade = 0.30 + 0.70 * std::max(0.0, nx * lx + ny * ly + nz * lz);
            // floor: deep umber, peak: warm cream
            const double R = (78 + (246 - 78) * t) * shade;
            const double G = (52 + (232 - 52) * t) * shade;
            const double B = (44 + (200 - 44) * t) * shade;
            line[c] = qRgb(int(qBound(0.0, R, 255.0)), int(qBound(0.0, G, 255.0)),
                           int(qBound(0.0, B, 255.0)));
        }
    }
    return img;
}

// ---------------------------------------------------------------------------
// provider / registry

struct ModelRegistry {
    QHash<const Document *, ModelStore *> byDoc;
    std::vector<std::unique_ptr<ModelStore>> owned;
    static ModelRegistry &instance()
    {
        static ModelRegistry r;
        return r;
    }
};

ModelStore::~ModelStore()
{
    detach();
}

void ModelStore::attach(const Document *doc)
{
    detach();
    m_doc = doc;
    m_dirty = true;
    if (doc)
        ModelRegistry::instance().byDoc.insert(doc, this);
}

void ModelStore::detach()
{
    if (m_doc) {
        auto &reg = ModelRegistry::instance().byDoc;
        if (reg.value(m_doc) == this)
            reg.remove(m_doc);
    }
    m_doc = nullptr;
}

ModelStore *ModelStore::forDocument(const Document &doc)
{
    ModelRegistry &reg = ModelRegistry::instance();
    ModelStore *s = reg.byDoc.value(&doc, nullptr);
    if (s && s->m_lazy && s->m_loadedFrom != doc.filePath()) {
        s->model.loadFrom(doc.filePath());
        s->m_loadedFrom = doc.filePath();
        s->m_dirty = true;
    }
    if (s)
        return s;
    auto owned = std::make_unique<ModelStore>();
    owned->m_lazy = true;
    owned->model.loadFrom(doc.filePath());
    owned->m_loadedFrom = doc.filePath();
    owned->attach(&doc);
    s = owned.get();
    reg.owned.push_back(std::move(owned));
    return s;
}

const HeightModel *ModelStore::heightModel()
{
    if (!m_doc)
        return nullptr;
    if (m_dirty) {
        m_hm = buildHeightModel(model, *m_doc);
        m_dirty = false;
    }
    return m_hm.valid() ? &m_hm : nullptr;
}

void ModelStore::setHeightModel(const HeightModel &hm)
{
    m_hm = hm;
    m_dirty = false;
}

void installModelProvider()
{
    static bool installed = false;
    if (installed)
        return;
    installed = true;
    setHeightModelProvider([](const Document &doc) -> const HeightModel * {
        ModelStore *s = ModelStore::forDocument(doc);
        return s ? s->heightModel() : nullptr;
    });
}

} // namespace c2d
