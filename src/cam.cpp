#include "cam.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QPolygonF>

#include <clipper2/clipper.h>

#include <algorithm>
#include <cmath>

namespace c2d {

using namespace Clipper2Lib;

// Clipper2 works in int64; 1000 units per mm keeps 1 um resolution.
static constexpr double kScale = 1000.0;

static Paths64 toPaths64(const QVector<QPainterPath> &shapes)
{
    Paths64 out;
    for (const QPainterPath &shape : shapes) {
        const QList<QPolygonF> polys = shape.toSubpathPolygons();
        for (const QPolygonF &poly : polys) {
            Path64 p;
            p.reserve(size_t(poly.size()));
            for (const QPointF &pt : poly)
                p.push_back(Point64(llround(pt.x() * kScale),
                                    llround(pt.y() * kScale)));
            if (p.size() >= 3)
                out.push_back(p);
        }
    }
    // Normalize orientation/overlap so offset deltas mean outward/inward.
    // EvenOdd matches Qt's default OddEvenFill (holes = reversed subpaths).
    return Union(out, FillRule::EvenOdd);
}

static Paths64 offsetPaths(const Paths64 &paths, double deltaMm)
{
    if (paths.empty())
        return {};
    return InflatePaths(paths, deltaMm * kScale, JoinType::Round,
                        EndType::Polygon, 2.0, kScale * 0.01);
}

// Depth passes per the build 853 convention: positive-down mm, stepdown
// slices from start toward end, always ending exactly at end. cutout stores
// its depth as a bare number (cut_depth / depth_per_pass) instead of the
// start/end strings the other types use.
static QVector<double> depthPasses(const QJsonObject &tp)
{
    const double start = tp.value("start_depth").toString().toDouble();
    double end         = tp.value("end_depth").toString().toDouble();
    double step        = tp.value("stepdown").toDouble();
    if (tp.contains(QStringLiteral("cut_depth")))
        end = tp.value("cut_depth").toDouble();
    if (tp.contains(QStringLiteral("depth_per_pass")))
        step = tp.value("depth_per_pass").toDouble();
    QVector<double> out;
    if (end <= start)
        return out;
    if (step <= 0)
        step = end - start;
    for (double d = start + step; d < end - 1e-9; d += step)
        out.append(d);
    out.append(end);
    return out;
}

struct Feeds {
    double feed = 0, plunge = 0;
    int rpm = 0, toolNumber = 0;
    double toolRadius = 0;
};

static Feeds feedsOf(const QJsonObject &tp)
{
    Feeds f;
    const QJsonObject speeds = tp.value("speeds").toObject();
    f.feed   = speeds.value("feedrate").toDouble();
    f.plunge = speeds.value("plungerate").toDouble();
    f.rpm    = int(speeds.value("rpm").toDouble());
    const QJsonObject tool = tp.value("tool").toObject();
    f.toolNumber = int(tool.value("number").toDouble());
    f.toolRadius = tool.value("diameter").toDouble() / 2.0;
    return f;
}

// Cut one closed polygon at one depth: rapid over the first vertex, plunge,
// feed around and close, then retract.
static void cutRing(QVector<Op> &ops, const Path64 &ring, double depth,
                    const Feeds &f)
{
    if (ring.empty())
        return;
    const double x0 = double(ring.front().x) / kScale;
    const double y0 = double(ring.front().y) / kScale;
    ops.append(Op::rapid(x0, y0, Cam::kRetractMm));
    ops.append(Op::feedTo(x0, y0, -depth, f.plunge));
    for (size_t i = 1; i < ring.size(); ++i)
        ops.append(Op::feedTo(double(ring[i].x) / kScale,
                              double(ring[i].y) / kScale, -depth, f.feed));
    ops.append(Op::feedTo(x0, y0, -depth, f.feed));
    ops.append(Op::rapid(x0, y0, Cam::kRetractMm));
}

static QVector<Op> contourOps(const QJsonObject &tp,
                              const QVector<QPainterPath> &shapes)
{
    const Feeds f = feedsOf(tp);
    // contour carries ofset_dir (CC's typo): -1 inside, 1 outside, 0 on the
    // line. cutout has no ofset_dir; it cuts outside unless flipped.
    int dir = int(tp.value("ofset_dir").toDouble());
    if (!tp.contains(QStringLiteral("ofset_dir")))
        dir = tp.value("flip_inside_outside").toBool() ? -1 : 1;
    const double stl = tp.value("stock_to_leave").toDouble();

    Paths64 outline = toPaths64(shapes);
    if (dir > 0)
        outline = offsetPaths(outline, f.toolRadius + stl);
    else if (dir < 0)
        outline = offsetPaths(outline, -(f.toolRadius + stl));

    QVector<Op> ops;
    for (double depth : depthPasses(tp))
        for (const Path64 &ring : outline)
            cutRing(ops, ring, depth, f);
    return ops;
}

static QVector<Op> pocketOps(const QJsonObject &tp,
                             const QVector<QPainterPath> &shapes)
{
    const Feeds f = feedsOf(tp);
    const double stl = tp.value("stock_to_leave").toDouble();
    double stepover = tp.value("stepover").toDouble();
    if (stepover <= 0)
        stepover = f.toolRadius;   // half the diameter

    // Ring sets from the boundary pass inward until the region vanishes.
    const Paths64 region = toPaths64(shapes);
    std::vector<Paths64> rings;
    for (double delta = f.toolRadius + stl;; delta += stepover) {
        Paths64 r = offsetPaths(region, -delta);
        if (r.empty())
            break;
        rings.push_back(std::move(r));
    }

    // Cut inside-out at each depth: innermost rings first avoids ramming the
    // full tool width into a corner on the first move.
    QVector<Op> ops;
    for (double depth : depthPasses(tp))
        for (auto it = rings.rbegin(); it != rings.rend(); ++it)
            for (const Path64 &ring : *it)
                cutRing(ops, ring, depth, f);
    return ops;
}

static QVector<Op> drillOps(const QJsonObject &tp,
                            const QVector<QPainterPath> &shapes)
{
    const Feeds f = feedsOf(tp);
    const QVector<double> passes = depthPasses(tp);

    QVector<Op> ops;
    for (const QPainterPath &shape : shapes) {
        const QPointF c = shape.boundingRect().center();
        ops.append(Op::rapid(c.x(), c.y(), Cam::kRetractMm));
        // Peck by stepdown: plunge each pass, retract between pecks.
        for (int i = 0; i < passes.size(); ++i) {
            ops.append(Op::feedTo(c.x(), c.y(), -passes.at(i), f.plunge));
            ops.append(Op::rapid(c.x(), c.y(), Cam::kRetractMm));
        }
    }
    return ops;
}

QVector<Op> Cam::toolpathOps(const Toolpath &tp,
                             const QVector<QPainterPath> &shapes,
                             QStringList *notes)
{
    const QJsonObject &j = tp.json;
    const Feeds f = feedsOf(j);

    QVector<Op> body;
    if (tp.type == QLatin1String("contour") || tp.type == QLatin1String("cutout")) {
        body = contourOps(j, shapes);
        // Holding tabs are not generated yet: a full-depth contour/cutout
        // frees the part. Surface that loudly rather than silently.
        if (notes && !j.value("ignore_tabs").toBool(false) &&
            j.value("tab_height").toDouble() > 0)
            notes->append(QStringLiteral(
                              "%1 [%2]: holding tabs NOT generated (unsupported); "
                              "the part will come loose at full depth")
                              .arg(j.value("name").toString(), tp.type));
    } else if (tp.type == QLatin1String("pocket_toolpath"))
        body = pocketOps(j, shapes);
    else if (tp.type == QLatin1String("drilling_toolpath"))
        body = drillOps(j, shapes);
    else {
        if (notes)
            notes->append(QStringLiteral("skipped %1 [%2]: type not supported")
                              .arg(j.value("name").toString(), tp.type));
        return {};
    }

    if (body.isEmpty()) {
        if (notes)
            notes->append(QStringLiteral("skipped %1 [%2]: no cuttable geometry")
                              .arg(j.value("name").toString(), tp.type));
        return {};
    }

    QVector<Op> ops;
    ops.append(Op::comment(j.value("name").toString()));
    ops.append(Op::tool(f.toolNumber));
    ops.append(Op::spindle(f.rpm));
    ops.append(body);
    return ops;
}

Cam::Result Cam::generate(const Document &doc)
{
    Result res;
    for (const Toolpath &tp : doc.toolpaths()) {
        if (!tp.json.value("enabled").toBool(true))
            continue;

        // Resolve element references to their outlines.
        QVector<QPainterPath> shapes;
        const QJsonArray refs = tp.json.value("elements").toArray();
        for (const QJsonValue &r : refs) {
            const QString uuid = r.toObject().value("uuid").toString();
            for (const Element &e : doc.elements())
                if (e.id == uuid) {
                    shapes.append(e.painterPath);
                    break;
                }
        }
        if (shapes.isEmpty()) {
            res.notes.append(QStringLiteral("skipped %1 [%2]: no elements")
                                 .arg(tp.json.value("name").toString(), tp.type));
            continue;
        }
        res.ops.append(toolpathOps(tp, shapes, &res.notes));
    }
    return res;
}

} // namespace c2d
