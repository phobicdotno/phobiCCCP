#include "padpreview.h"

#include "c2ddocument.h"
#include "element.h"
#include "gcodeexport.h"
#include "jobview.h"
#include "padlink.h"
#include "post_grbl.h"

#include <QBuffer>
#include <QColor>
#include <QImage>
#include <QJsonArray>
#include <QPainter>
#include <QSet>
#include <QTemporaryDir>

#include <cmath>

namespace c2d::padlink {

namespace {

QJsonObject refuse(const QString &why)
{
    return QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), why}};
}

// What the pad's design asks of the export, counted before anything is built.
QString tooBig(const QJsonObject &d)
{
    const QJsonArray shapes = d.value(QStringLiteral("shapes")).toArray();
    const QJsonArray tps = d.value(QStringLiteral("toolpaths")).toArray();
    qsizetype points = 0;
    for (const QJsonValue &v : shapes)
        points += v.toObject().value(QStringLiteral("points")).toArray().size();
    if (shapes.size() > kMaxPreviewShapes || points > kMaxPreviewPoints
        || tps.size() > kMaxPreviewToolpaths)
        return QStringLiteral("the design is too big to preview from the pad (%1 shapes, %2 points, "
                              "%3 toolpaths; at most %4, %5 and %6): open it on the computer")
            .arg(shapes.size()).arg(points).arg(tps.size())
            .arg(kMaxPreviewShapes).arg(kMaxPreviewPoints).arg(kMaxPreviewToolpaths);
    return QString();
}

const QColor kCutColors[] = {QColor(0x4f, 0xc3, 0xf7), QColor(0xff, 0xb7, 0x4d), QColor(0x81, 0xc7, 0x84),
                             QColor(0xf0, 0x62, 0x92), QColor(0xba, 0x68, 0xc8), QColor(0xff, 0xf1, 0x76)};

// The stock from above, the shapes in light lines, and every toolpath's
// cutting moves in its own colour: a band the bit's width with the bit's
// centre line in it, drill holes as dots.
QString render(const Document &doc, const JobPlan &plan, double width, double height, int longSide)
{
    const double margin = 16;
    const double k = (longSide - 2 * margin) / std::max(1.0, std::max(width, height));
    const QSize size(int(std::ceil(width * k + 2 * margin)), int(std::ceil(height * k + 2 * margin)));
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(QColor(0x1b, 0x1e, 0x24));
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    QTransform t;   // mm, Y up -> pixels, Y down
    t.translate(margin, size.height() - margin);
    t.scale(k, -k);

    p.setPen(QPen(QColor(0x8d, 0x6e, 0x4c), 2));
    p.setBrush(QColor(0xc8, 0xa9, 0x7e));
    p.drawRect(t.mapRect(QRectF(0, 0, width, height)));

    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0x3a, 0x2e, 0x22), 1.5));
    for (const Element &e : doc.elements())
        p.drawPath(t.map(e.painterPath));

    QHash<QString, int> colorOf;   // toolpath name -> colour, one per toolpath
    for (const JobSection &s : plan.sections) {
        if (s.kind != JobSection::Toolpath)
            continue;
        const int ci = colorOf.contains(s.name) ? colorOf.value(s.name) : int(colorOf.size());
        colorOf.insert(s.name, ci);
        const QColor c = kCutColors[ci % std::size(kCutColors)];
        const double bit = s.bit.diameter > 0 ? s.bit.diameter * k : 2.0;
        QColor band = c;
        band.setAlpha(90);
        p.setBrush(Qt::NoBrush);
        if (!s.cut.isEmpty()) {
            p.setPen(QPen(band, std::max(1.0, bit), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPath(t.map(s.cut));
            p.setPen(QPen(c.darker(150), 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPath(t.map(s.cut));
        }
        p.setPen(Qt::NoPen);
        p.setBrush(c.darker(130));
        for (const QPointF &h : s.plunges)
            p.drawEllipse(t.map(h), std::max(3.0, bit / 2), std::max(3.0, bit / 2));
    }
    p.end();
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return QString::fromLatin1(png.toBase64());
}

}   // namespace

QJsonObject previewJson(const QJsonObject &design, int longSide)
{
    if (const QString e = check(design); !e.isEmpty())
        return refuse(e);
    if (const QString big = tooBig(design); !big.isEmpty())
        return refuse(big);
    QTemporaryDir tmp;
    if (!tmp.isValid())
        return refuse(QStringLiteral("no room for a scratch file here"));
    const QString path = tmp.filePath(QStringLiteral("preview.c2d"));
    QStringList notes;
    QString err;
    if (!buildDocument(design, path, &notes, &err))
        return refuse(err);
    Document doc;
    if (!doc.load(path, &err))
        return refuse(err);
    const GcodeResult r = exportGcode(doc);
    for (const QString &s : r.skipped)
        notes << QStringLiteral("left out: %1").arg(s);
    notes += r.notes;
    if (const QString reuse = toolReuseWarning(r.ops); !reuse.isEmpty())
        notes << reuse;
    if (r.done.isEmpty())
        notes << QStringLiteral("nothing to cut: no toolpath made any moves");

    // Times as the Machine page plans them (acceleration, corners).
    const JobStats stats = computeStats(r.ops);
    const QHash<QString, double> byName = timeByComment(r.ops);
    QJsonArray per;
    QSet<QString> seen;
    for (const Op &o : r.ops) {
        if (o.kind != Op::Comment || seen.contains(o.text))
            continue;
        seen.insert(o.text);
        per.append(QJsonObject{{QStringLiteral("name"), o.text},
                               {QStringLiteral("seconds"), std::round(byName.value(o.text))}});
    }

    const QStringList lines = r.gcode.split(QChar('\n'), Qt::SkipEmptyParts);
    const JobPlan plan = planJob(lines, r.ops, &doc);
    const QJsonObject st = design.value(QStringLiteral("stock")).toObject();
    const QString png = render(doc, plan, st.value(QStringLiteral("width")).toDouble(),
                               st.value(QStringLiteral("height")).toDouble(), qBound(128, longSide, 2048));
    return QJsonObject{{QStringLiteral("ok"), true},
                       {QStringLiteral("png"), png},
                       {QStringLiteral("seconds"), std::round(stats.timeSec)},
                       {QStringLiteral("perToolpath"), per},
                       {QStringLiteral("notes"), QJsonArray::fromStringList(notes)}};
}

}   // namespace c2d::padlink
