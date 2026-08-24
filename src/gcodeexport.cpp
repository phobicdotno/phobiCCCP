#include "gcodeexport.h"
#include "c2ddocument.h"
#include "post_grbl.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QPainterPathStroker>
#include <QPolygonF>

namespace c2d {

// Depth values are strings whose sign convention flipped between CC builds
// (843 stores "-19.126", 853 stores "19.126" for the same cut). Machine Z is
// negative below stock top either way.
static double depthToZ(const QJsonValue &v)
{
    return -qAbs(v.isString() ? v.toString().toDouble() : v.toDouble());
}

static QVector<const Element *> referenced(Document &doc, const QJsonObject &tp)
{
    QVector<const Element *> out;
    for (const QJsonValue &ev : tp.value("elements").toArray()) {
        const QString id = ev.toObject().value("uuid").toString();
        if (const Element *e = doc.elementById(id))
            out.append(e);
    }
    return out;
}

static void closeLoop(QPolygonF &poly)
{
    if (poly.size() > 2 && poly.first() != poly.last())
        poly.append(poly.first());
}

// Geometric offsetting with Qt path booleans: stroking the region boundary
// with width 2·delta gives a band from -delta..+delta around it; subtracting
// the band insets the region, uniting it outsets. Round joins approximate the
// true tool-radius offset.
static QPainterPath offsetBand(const QPainterPath &region, double delta)
{
    QPainterPathStroker st;
    st.setWidth(2 * delta);
    st.setJoinStyle(Qt::RoundJoin);
    st.setCapStyle(Qt::RoundCap);
    return st.createStroke(region);
}

static QList<QPolygonF> insetRings(const QPainterPath &region, double delta)
{
    QList<QPolygonF> out;
    const QPainterPath inner =
        region.subtracted(offsetBand(region, delta)).simplified();
    for (QPolygonF p : inner.toSubpathPolygons())
        if (p.size() > 2)
            out.append(p);
    return out;
}

static QList<QPolygonF> outsetRings(const QPainterPath &region, double delta)
{
    QList<QPolygonF> out;
    const QPainterPath outer =
        region.united(offsetBand(region, delta)).simplified();
    for (QPolygonF p : outer.toSubpathPolygons())
        if (p.size() > 2)
            out.append(p);
    return out;
}

// Union of the referenced closed vectors as a filled region.
static QPainterPath regionOf(const QVector<const Element *> &elems)
{
    QPainterPath r;
    for (const Element *e : elems) {
        QPainterPath p = e->painterPath;
        r = r.isEmpty() ? p : r.united(p);
    }
    return r.simplified();
}

GcodeResult exportGcode(Document &doc)
{
    GcodeResult res;
    QVector<Op> ops;
    const double safeZ = doc.params().value("retract", "2.54").toDouble();

    int lastTool = -1;
    for (const Toolpath &t : doc.toolpaths()) {
        const QJsonObject j = t.json;
        if (!j.value("enabled").toBool(true))
            continue;
        const QString name = j.value("name").toString();

        const bool contour = (t.type == QLatin1String("contour"));
        const bool pocket = (t.type == QLatin1String("pocket_toolpath"));
        const bool drilling = (t.type == QLatin1String("drilling_toolpath"));
        if (!contour && !pocket && !drilling) {
            res.skipped << QStringLiteral("%1 (%2 not supported yet)")
                               .arg(name, t.type);
            continue;
        }

        const QJsonObject speeds = j.value("speeds").toObject();
        const double feed = speeds.value("feedrate").toDouble(500);
        const double plunge = speeds.value("plungerate").toDouble(100);
        const int rpm = int(speeds.value("rpm").toDouble(10000));
        const int toolNo = int(j.value("tool").toObject().value("number").toDouble(0));
        const double zTop = depthToZ(j.value("start_depth"));
        const double zBot = depthToZ(j.value("end_depth"));
        const double stepdown = qMax(0.05, j.value("stepdown").toDouble(1.0));

        const QVector<const Element *> elems = referenced(doc, j);
        if (elems.isEmpty()) {
            res.skipped << QStringLiteral("%1 (no vectors)").arg(name);
            continue;
        }

        if (toolNo != lastTool) {
            ops.append(Op::tool(toolNo));
            lastTool = toolNo;
        }
        ops.append(Op::comment(name));
        ops.append(Op::spindle(rpm));

        if (contour || pocket) {
            const double toolR =
                j.value("tool").toObject().value("diameter").toDouble(6) / 2.0;
            const double stepover = qMax(0.1, j.value("stepover").toDouble(toolR));
            const int dir = j.value("ofset_dir").toInt(0);

            // Collect the loops to cut (identical at every depth pass).
            QList<QPolygonF> rings;
            if (contour && dir == 0) {
                // Follow each vector as-is: closed subpaths already end at
                // their start point; open polylines are engraved open.
                for (const Element *e : elems)
                    for (const QPolygonF &p : e->painterPath.toSubpathPolygons())
                        if (p.size() >= 2)
                            rings.append(p);
            } else if (contour) {
                const QPainterPath region = regionOf(elems);
                rings = dir > 0 ? outsetRings(region, toolR)
                                : insetRings(region, toolR);
                for (QPolygonF &p : rings)
                    closeLoop(p);
            } else { // pocket: inset by tool radius, then step inward until dry
                const QPainterPath region = regionOf(elems);
                QList<QList<QPolygonF>> shells;
                double delta = toolR;
                while (shells.size() < 500) {
                    const QList<QPolygonF> s = insetRings(region, delta);
                    if (s.isEmpty())
                        break;
                    shells.append(s);
                    delta += stepover;
                }
                // Cut innermost material first, finishing at the boundary.
                for (int k = shells.size() - 1; k >= 0; --k)
                    for (QPolygonF p : shells.at(k)) {
                        closeLoop(p);
                        rings.append(p);
                    }
            }
            if (rings.isEmpty()) {
                res.skipped << QStringLiteral("%1 (no cuttable loops)").arg(name);
                ops.removeLast();   // spindle on
                ops.removeLast();   // comment
                continue;
            }

            double z = zTop;
            bool more = true;
            while (more) {
                z = qMax(zBot, z - stepdown);
                more = z > zBot + 1e-9;
                for (const QPolygonF &poly : rings) {
                    const QPointF p0 = poly.first();
                    ops.append(Op::rapid(p0.x(), p0.y(), safeZ));
                    ops.append(Op::feedTo(p0.x(), p0.y(), z, plunge));
                    for (int i = 1; i < poly.size(); ++i)
                        ops.append(Op::feedTo(poly.at(i).x(), poly.at(i).y(), z, feed));
                    ops.append(Op::rapid(poly.last().x(), poly.last().y(), safeZ));
                }
            }
        } else { // drilling
            const double peck = j.value("peck_distance").toDouble(0);
            for (const Element *e : elems) {
                const QJsonArray c = e->raw.value("center").toArray();
                const QPointF ctr = c.size() == 2
                    ? QPointF(c.at(0).toDouble(), c.at(1).toDouble())
                    : e->painterPath.boundingRect().center();
                ops.append(Op::rapid(ctr.x(), ctr.y(), safeZ));
                if (peck > 0.01) {
                    double z = zTop;
                    while (z > zBot + 1e-9) {
                        z = qMax(zBot, z - peck);
                        ops.append(Op::feedTo(ctr.x(), ctr.y(), z, plunge));
                        ops.append(Op::rapid(ctr.x(), ctr.y(), safeZ));   // chip clear
                    }
                } else {
                    ops.append(Op::feedTo(ctr.x(), ctr.y(), zBot, plunge));
                    ops.append(Op::rapid(ctr.x(), ctr.y(), safeZ));
                }
            }
        }
        ops.append(Op::spindle(0));
        res.done << name;
    }

    res.gcode = GrblPost(true).generate(ops);
    return res;
}

} // namespace c2d
