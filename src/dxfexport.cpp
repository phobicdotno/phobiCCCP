#include "dxfexport.h"

#include <QJsonObject>
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

// DXF layer names may not hold < > / \ " : ; ? * | = ` and the empty name is
// layer 0.
QString layerName(const Element &e)
{
    QString n = e.raw.value("layer").toObject().value("name").toString().trimmed();
    for (QChar &c : n)
        if (QStringLiteral("<>/\\\":;?*|=`").contains(c) || c.unicode() < 32)
            c = QLatin1Char('_');
    return n.isEmpty() ? QStringLiteral("0") : n;
}

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
    QStringList layers;

    for (const Element &e : elements) {
        if (Element::isConstruction(e) || e.painterPath.isEmpty()) {
            ++res.skipped;
            continue;
        }
        const QString layer = layerName(e);
        if (!layers.contains(layer))
            layers.append(layer);

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
            const bool closed = poly.size() > 2 && poly.isClosed();
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

    if (!layers.contains(QStringLiteral("0")))
        layers.prepend(QStringLiteral("0"));
    w.pair(0, QStringLiteral("SECTION"));
    w.pair(2, QStringLiteral("TABLES"));
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
