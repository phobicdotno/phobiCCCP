#include "dxfexport.h"

#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QPolygonF>
#include <QStringList>
#include <QTransform>

#include <cmath>

namespace c2d {

namespace {

// Same trick as gcodeexport/vectorops: Qt flattens beziers at a fixed 0.5
// path-unit chord error, so scale up first to get `tol` mm.
QList<QPolygonF> finePolygons(const QPainterPath &path, double tol)
{
    const double k = 0.5 / (tol > 1e-9 ? tol : 0.01);
    QList<QPolygonF> out = path.toSubpathPolygons(QTransform::fromScale(k, k));
    for (QPolygonF &poly : out)
        for (QPointF &pt : poly)
            pt /= k;
    return out;
}

// The document's own layer name ("" for none).
QString sourceLayer(const Element &e)
{
    return e.raw.value("layer").toObject().value("name").toString().trimmed();
}

// FNV-1a: the same suffix for the same name in every run (qHash is seeded).
QString nameHash(const QString &name)
{
    quint32 h = 2166136261u;
    for (const char c : name.toUtf8()) {
        h ^= quint8(c);
        h *= 16777619u;
    }
    return QString::number(h & 0xffffffu, 16).rightJustified(6, QLatin1Char('0')).toUpper();
}

// DXF R12 layer names: at most 31 of A-Z 0-9 _ $ -, compared without regard
// to case; the empty name is layer 0. Every other character becomes '_'.
// Names this maps onto one another ("Cut: outside" and "cut; outside", or two
// long names alike in their first 31 characters) are told apart by a short
// hash of the name, so different layers stay different layers; the first one
// met keeps the plain name.
class LayerNames
{
public:
    QString dxfName(const QString &name)
    {
        const QString source = name.isEmpty() ? QStringLiteral("0") : name;   // no layer: layer 0
        const auto known = m_bySource.constFind(source);
        if (known != m_bySource.constEnd())
            return known.value();
        QString n;
        for (const QChar c : source) {
            const char16_t u = c.toUpper().unicode();
            const bool ok = (u >= u'A' && u <= u'Z') || (u >= u'0' && u <= u'9') || u == u'_'
                            || u == u'$' || u == u'-';
            n += ok ? QChar(u) : QChar(u'_');
        }
        const bool cut = n.size() > 31;
        n.truncate(31);
        if (cut || m_used.contains(n))
            n = n.left(24) + QLatin1Char('_') + nameHash(source);
        for (int i = 2; m_used.contains(n); ++i) {   // a hash clash: not seen in practice
            const QString tail = QStringLiteral("-%1").arg(i);
            n = n.left(31 - int(tail.size())) + tail;
        }
        m_used.insert(n);
        m_order.append(n);
        m_bySource.insert(source, n);
        return n;
    }
    QStringList all() const { return m_order; }

private:
    QHash<QString, QString> m_bySource;
    QSet<QString> m_used;               // upper case, so case-blind
    QStringList m_order;
};

class Writer
{
public:
    void pair(int code, const QString &value)
    {
        m_out += QByteArray::number(code) + '\n' + value.toUtf8() + '\n';
    }
    void pair(int code, int value) { pair(code, QString::number(value)); }
    void num(int code, double v)
    {
        if (std::fabs(v) < 5e-7)
            v = 0;   // no "-0"
        QString s = QString::number(v, 'f', 6);
        while (s.endsWith(QLatin1Char('0')))
            s.chop(1);
        if (s.endsWith(QLatin1Char('.')))
            s.chop(1);
        pair(code, s);
    }
    QByteArray take() { return std::move(m_out); }

private:
    QByteArray m_out;
};

} // namespace

DxfExportResult exportDxf(const QVector<Element> &elements, double tolerance)
{
    DxfExportResult res;
    Writer ents;
    LayerNames layerNames;

    for (const Element &e : elements) {
        if (Element::isConstruction(e) || e.painterPath.isEmpty()) {
            ++res.skipped;
            continue;
        }
        const QString layer = layerNames.dxfName(sourceLayer(e));

        if (e.behavior == Element::Circle) {
            const QRectF b = e.painterPath.boundingRect();
            if (std::fabs(b.width() - b.height()) < 1e-6 && b.width() > 0) {
                ents.pair(0, QStringLiteral("CIRCLE"));
                ents.pair(8, layer);
                ents.num(10, b.center().x());
                ents.num(20, b.center().y());
                ents.num(30, 0);
                ents.num(40, b.width() / 2);
                ++res.entities;
                continue;
            }
        }

        for (QPolygonF poly : finePolygons(e.painterPath, tolerance)) {
            if (poly.size() < 2)
                continue;
            // A ring has three corners at least: there and back again
            // (A, B, A) stays an open polyline, or dropping its last point
            // would leave a closed one of two vertices.
            const bool closed = poly.size() > 3 && poly.isClosed();
            if (closed)
                poly.removeLast();   // flag 70 = 1 closes it
            ents.pair(0, QStringLiteral("POLYLINE"));
            ents.pair(8, layer);
            ents.pair(66, 1);
            ents.num(10, 0);
            ents.num(20, 0);
            ents.num(30, 0);
            ents.pair(70, closed ? 1 : 0);
            for (const QPointF &p : poly) {
                ents.pair(0, QStringLiteral("VERTEX"));
                ents.pair(8, layer);
                ents.num(10, p.x());
                ents.num(20, p.y());
                ents.num(30, 0);
            }
            ents.pair(0, QStringLiteral("SEQEND"));
            ents.pair(8, layer);
            ++res.entities;
        }
    }

    Writer w;
    w.pair(0, QStringLiteral("SECTION"));
    w.pair(2, QStringLiteral("HEADER"));
    w.pair(9, QStringLiteral("$ACADVER"));
    w.pair(1, QStringLiteral("AC1009"));
    w.pair(9, QStringLiteral("$INSUNITS"));
    w.pair(70, 4);   // millimetres
    w.pair(0, QStringLiteral("ENDSEC"));

    QStringList layers = layerNames.all();
    if (!layers.contains(QStringLiteral("0")))
        layers.prepend(QStringLiteral("0"));
    w.pair(0, QStringLiteral("SECTION"));
    w.pair(2, QStringLiteral("TABLES"));
    // The line type every layer names: R12 readers look it up here.
    w.pair(0, QStringLiteral("TABLE"));
    w.pair(2, QStringLiteral("LTYPE"));
    w.pair(70, 1);
    w.pair(0, QStringLiteral("LTYPE"));
    w.pair(2, QStringLiteral("CONTINUOUS"));
    w.pair(70, 0);
    w.pair(3, QStringLiteral("Solid line"));
    w.pair(72, 65);
    w.pair(73, 0);
    w.num(40, 0);
    w.pair(0, QStringLiteral("ENDTAB"));
    w.pair(0, QStringLiteral("TABLE"));
    w.pair(2, QStringLiteral("LAYER"));
    w.pair(70, int(layers.size()));
    for (const QString &l : layers) {
        w.pair(0, QStringLiteral("LAYER"));
        w.pair(2, l);
        w.pair(70, 0);
        w.pair(62, 7);
        w.pair(6, QStringLiteral("CONTINUOUS"));
    }
    w.pair(0, QStringLiteral("ENDTAB"));
    w.pair(0, QStringLiteral("ENDSEC"));

    w.pair(0, QStringLiteral("SECTION"));
    w.pair(2, QStringLiteral("ENTITIES"));
    res.data = w.take() + ents.take();
    Writer tail;
    tail.pair(0, QStringLiteral("ENDSEC"));
    tail.pair(0, QStringLiteral("EOF"));
    res.data += tail.take();
    return res;
}

} // namespace c2d
