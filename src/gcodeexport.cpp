#include "gcodeexport.h"
#include "c2ddocument.h"
#include "post_grbl.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QPolygonF>

#include <clipper2/clipper.h>

#include <cmath>

namespace c2d {

using namespace Clipper2Lib;

// Clipper2 works in int64; 1000 units per mm keeps 1 um resolution.
static constexpr double kScale = 1000.0;

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

// Referenced closed vectors as a normalized Clipper2 region. EvenOdd matches
// Qt's default OddEvenFill, so holes (reversed subpaths) survive.
static Paths64 regionOf(const QVector<const Element *> &elems)
{
    Paths64 raw;
    for (const Element *e : elems) {
        for (const QPolygonF &poly : e->painterPath.toSubpathPolygons()) {
            Path64 p;
            p.reserve(size_t(poly.size()));
            for (const QPointF &pt : poly)
                p.push_back(Point64(llround(pt.x() * kScale),
                                    llround(pt.y() * kScale)));
            if (p.size() >= 3)
                raw.push_back(p);
        }
    }
    return Union(raw, FillRule::EvenOdd);
}

// Robust polygon offset: delta > 0 outsets, < 0 insets; empty when the
// region vanishes.
static QList<QPolygonF> offsetRings(const Paths64 &region, double deltaMm)
{
    QList<QPolygonF> out;
    if (region.empty())
        return out;
    const Paths64 shifted =
        InflatePaths(region, deltaMm * kScale, JoinType::Round,
                     EndType::Polygon, 2.0, kScale * 0.01);
    for (const Path64 &ring : shifted) {
        QPolygonF p;
        p.reserve(int(ring.size()));
        for (const Point64 &pt : ring)
            p.append(QPointF(double(pt.x) / kScale, double(pt.y) / kScale));
        if (p.size() > 2)
            out.append(p);
    }
    return out;
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
        const bool cutout = (t.type == QLatin1String("cutout"));
        const bool pocket = (t.type == QLatin1String("pocket_toolpath"));
        const bool drilling = (t.type == QLatin1String("drilling_toolpath"));
        if (!contour && !cutout && !pocket && !drilling) {
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
        // cutout stores its depth as bare numbers, not the start/end strings.
        const double zBot = cutout && j.contains(QStringLiteral("cut_depth"))
            ? -qAbs(j.value("cut_depth").toDouble())
            : depthToZ(j.value("end_depth"));
        double stepdown = j.value("stepdown").toDouble(1.0);
        if (cutout && j.contains(QStringLiteral("depth_per_pass")))
            stepdown = j.value("depth_per_pass").toDouble(stepdown);
        stepdown = qMax(0.05, stepdown);

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

        if (contour || cutout || pocket) {
            const double toolR =
                j.value("tool").toObject().value("diameter").toDouble(6) / 2.0;
            const double stepover = qMax(0.1, j.value("stepover").toDouble(toolR));
            const double stl = j.value("stock_to_leave").toDouble(0);
            // contour: ofset_dir -1 inside / 0 follow / 1 outside.
            // cutout has no ofset_dir: outside unless flipped.
            int dir = j.value("ofset_dir").toInt(0);
            if (cutout)
                dir = j.value("flip_inside_outside").toBool(false) ? -1 : 1;

            // Collect the loops to cut (identical at every depth pass).
            QList<QPolygonF> rings;
            if ((contour || cutout) && dir == 0) {
                // Follow each vector as-is: closed subpaths already end at
                // their start point; open polylines are engraved open.
                for (const Element *e : elems)
                    for (const QPolygonF &p : e->painterPath.toSubpathPolygons())
                        if (p.size() >= 2)
                            rings.append(p);
            } else if (contour || cutout) {
                rings = offsetRings(regionOf(elems),
                                    dir > 0 ? toolR + stl : -(toolR + stl));
                for (QPolygonF &p : rings)
                    closeLoop(p);
            } else { // pocket: inset by tool radius, then step inward until dry
                const Paths64 region = regionOf(elems);
                QList<QList<QPolygonF>> shells;
                double delta = toolR + stl;
                while (shells.size() < 500) {
                    const QList<QPolygonF> s = offsetRings(region, -delta);
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

            // Holding tabs are not generated yet: warn when the toolpath
            // expects them, because the part comes loose at full depth.
            if ((contour || cutout) && !j.value("ignore_tabs").toBool(false)
                && j.value("tab_height").toDouble() > 0)
                res.warnings << QStringLiteral(
                                    "%1: holding tabs NOT generated; the part "
                                    "will come loose at full depth")
                                    .arg(name);

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
