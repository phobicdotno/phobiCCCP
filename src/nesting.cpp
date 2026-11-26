#include "nesting.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include <clipper2/clipper.h>
namespace C2 = Clipper2Lib;

namespace c2d::nest {

namespace {

C2::PathD toPath(const QPolygonF &poly)
{
    C2::PathD p;
    p.reserve(poly.size());
    for (const QPointF &q : poly)
        p.emplace_back(q.x(), q.y());
    return p;
}

QRectF boundsOf(const C2::PathsD &paths)
{
    const C2::RectD r = C2::GetBounds(paths);
    return QRectF(QPointF(r.left, r.top), QPointF(r.right, r.bottom)).normalized();
}

// One quarter turn of a part, the turn applied about the origin.
struct Pose {
    double deg = 0;
    QTransform turn;
    QRectF raw;          // bounds of the turned outline
    C2::PathsD grown;    // turned outline grown by spacing/2
    QRectF grownBox;
};

struct Placed {
    C2::PathsD grown;
    QRectF box;
};

bool overlaps(const C2::PathsD &a, const QRectF &aBox, const QVector<Placed> &placed)
{
    for (const Placed &p : placed) {
        if (!aBox.intersects(p.box))
            continue;
        const C2::PathsD hit = C2::Intersect(a, p.grown, C2::FillRule::NonZero, 4);
        if (std::fabs(C2::Area(hit)) > 1e-3)
            return true;
    }
    return false;
}

// Sorted, de-duplicated values within [lo, hi].
QVector<double> candidates(QVector<double> v, double lo, double hi)
{
    QVector<double> out;
    std::sort(v.begin(), v.end());
    for (double x : v)
        if (x >= lo - 1e-9 && x <= hi + 1e-9 && (out.isEmpty() || x - out.last() > 1e-6))
            out.append(x);
    return out;
}

} // namespace

QVector<Placement> nestParts(const QVector<QPolygonF> &outlines, const QRectF &stock,
                             const Options &opt, const QVector<QPolygonF> &obstacles)
{
    QVector<Placement> out(outlines.size());
    const QRectF area = stock.normalized().adjusted(opt.margin, opt.margin, -opt.margin, -opt.margin);
    if (area.width() <= 0 || area.height() <= 0)
        return out;
    const double half = std::max(0.0, opt.spacing) / 2;

    QVector<int> order(outlines.size());
    std::iota(order.begin(), order.end(), 0);
    QVector<double> areas(outlines.size());
    for (int i = 0; i < outlines.size(); ++i)
        areas[i] = std::fabs(C2::Area(toPath(outlines.at(i))));
    std::stable_sort(order.begin(), order.end(),
                     [&](int a, int b) { return areas.at(a) > areas.at(b); });

    QVector<Placed> placed;
    for (const QPolygonF &poly : obstacles) {
        if (poly.size() < 3)
            continue;
        const C2::PathsD one{toPath(poly)};
        Placed p;
        p.grown = half > 0 ? C2::InflatePaths(one, half, C2::JoinType::Round,
                                              C2::EndType::Polygon, 2.0, 4)
                           : one;
        p.box = boundsOf(p.grown);
        placed.append(p);
    }
    for (int idx : order) {
        const QPolygonF &poly = outlines.at(idx);
        if (poly.size() < 3)
            continue;
        QVector<double> turns{0};
        if (opt.allowRotation)
            turns = {0, 90, 180, 270};
        bool found = false;
        double bestY = 0, bestX = 0, bestTop = 0;
        Pose best;
        QPointF bestAt;
        for (double deg : turns) {
            Pose pose;
            pose.deg = deg;
            pose.turn.rotate(deg);
            const QPolygonF turned = pose.turn.map(poly);
            pose.raw = turned.boundingRect();
            if (pose.raw.width() > area.width() + 1e-9 || pose.raw.height() > area.height() + 1e-9)
                continue;
            const C2::PathsD one{toPath(turned)};
            pose.grown = half > 0 ? C2::InflatePaths(one, half, C2::JoinType::Round,
                                                     C2::EndType::Polygon, 2.0, 4)
                                  : one;
            pose.grownBox = boundsOf(pose.grown);
            // Positions are for the turned outline's bottom-left corner.
            const double size = std::min(pose.raw.width(), pose.raw.height());
            double step = std::min(opt.maxStep, std::max(size / 4, 0.1));
            step = std::max(step, std::max(area.width(), area.height()) / 400);
            const double hiX = area.right() - pose.raw.width();
            const double hiY = area.bottom() - pose.raw.height();
            QVector<double> xs, ys;
            for (double x = area.left(); x <= hiX + 1e-9; x += step) xs.append(x);
            for (double y = area.top(); y <= hiY + 1e-9; y += step) ys.append(y);
            xs.append(hiX);
            ys.append(hiY);
            // Also snug against the parts already placed (grown boxes touching).
            for (const Placed &p : placed) {
                xs.append(p.box.right() - (pose.grownBox.left() - pose.raw.left()));
                ys.append(p.box.bottom() - (pose.grownBox.top() - pose.raw.top()));
            }
            xs = candidates(xs, area.left(), hiX);
            ys = candidates(ys, area.top(), hiY);
            for (double y : ys) {
                if (found && y > bestY + 1e-9)
                    break;
                bool hit = false;
                for (double x : xs) {
                    if (found && y >= bestY - 1e-9 && x >= bestX - 1e-9)
                        break;
                    const double dx = x - pose.raw.left(), dy = y - pose.raw.top();
                    const QRectF box = pose.grownBox.translated(dx, dy);
                    // Cheap test first: no placed box near, nothing to clip.
                    const C2::PathsD moved = C2::TranslatePaths(pose.grown, dx, dy);
                    if (overlaps(moved, box, placed))
                        continue;
                    const double top = y + pose.raw.height();
                    if (!found || y < bestY - 1e-9 || x < bestX - 1e-9
                        || (std::fabs(x - bestX) < 1e-9 && top < bestTop)) {
                        found = true;
                        bestY = y;
                        bestX = x;
                        bestTop = top;
                        best = pose;
                        bestAt = QPointF(dx, dy);
                    }
                    hit = true;
                    break;
                }
                if (hit)
                    break;
            }
        }
        if (!found)
            continue;
        Placement &pl = out[idx];
        pl.placed = true;
        pl.rotation = best.deg;
        pl.transform = best.turn * QTransform::fromTranslate(bestAt.x(), bestAt.y());
        Placed p;
        p.grown = C2::TranslatePaths(best.grown, bestAt.x(), bestAt.y());
        p.box = best.grownBox.translated(bestAt);
        placed.append(p);
    }
    return out;
}

} // namespace c2d::nest
