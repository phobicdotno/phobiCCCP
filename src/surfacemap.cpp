#include "surfacemap.h"
#include "grblstreamer.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>

namespace c2d {

bool SurfaceMap::isValid() const
{
    if (cols < 1 || rows < 1 || cols * rows < 2 || z.size() != cols * rows)
        return false;
    if ((cols > 1 && !(x1 > x0)) || (rows > 1 && !(y1 > y0)))
        return false;
    for (double v : z)
        if (!std::isfinite(v))
            return false;
    return true;
}

double SurfaceMap::xAt(int col) const
{
    return cols > 1 ? x0 + (x1 - x0) * col / (cols - 1) : x0;
}

double SurfaceMap::yAt(int row) const
{
    return rows > 1 ? y0 + (y1 - y0) * row / (rows - 1) : y0;
}

// Cell index and fraction along one axis, clamped to the grid.
static void locate(double v, double lo, double hi, int n, int *i, double *t)
{
    if (n <= 1 || !(hi > lo)) {
        *i = 0;
        *t = 0;
        return;
    }
    const double u = std::clamp((v - lo) / (hi - lo), 0.0, 1.0) * (n - 1);
    *i = std::min(int(std::floor(u)), n - 2);
    *t = u - *i;
}

double SurfaceMap::heightAt(double x, double y) const
{
    if (!isValid())
        return 0;
    int c, r;
    double tx, ty;
    locate(x, x0, x1, cols, &c, &tx);
    locate(y, y0, y1, rows, &r, &ty);
    auto at = [&](int cc, int rr) {
        return z.at(std::min(rr, rows - 1) * cols + std::min(cc, cols - 1));
    };
    const double a = at(c, r) + (at(c + 1, r) - at(c, r)) * tx;
    const double b = at(c, r + 1) + (at(c + 1, r + 1) - at(c, r + 1)) * tx;
    return a + (b - a) * ty;
}

double SurfaceMap::offsetAt(double x, double y) const
{
    return isValid() ? heightAt(x, y) - heightAt(0, 0) : 0;
}

void SurfaceMap::offsetRange(double *lo, double *hi) const
{
    *lo = *hi = 0;
    if (!isValid())
        return;
    const double ref = heightAt(0, 0);
    *lo = *hi = z.first() - ref;
    for (double v : z) {
        *lo = std::min(*lo, v - ref);
        *hi = std::max(*hi, v - ref);
    }
}

bool SurfaceMap::covers(double x, double y, double tol) const
{
    return x >= x0 - tol && x <= x1 + tol && y >= y0 - tol && y <= y1 + tol;
}

QString SurfaceMap::toJson() const
{
    QJsonObject o;
    o[QStringLiteral("x0")] = x0;
    o[QStringLiteral("y0")] = y0;
    o[QStringLiteral("x1")] = x1;
    o[QStringLiteral("y1")] = y1;
    o[QStringLiteral("cols")] = cols;
    o[QStringLiteral("rows")] = rows;
    QJsonArray a;
    for (double v : z)
        a.append(v);
    o[QStringLiteral("z")] = a;
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

SurfaceMap SurfaceMap::fromJson(const QString &json)
{
    SurfaceMap m;
    const QJsonDocument d = QJsonDocument::fromJson(json.toUtf8());
    if (!d.isObject())
        return {};
    const QJsonObject o = d.object();
    m.x0 = o.value(QStringLiteral("x0")).toDouble();
    m.y0 = o.value(QStringLiteral("y0")).toDouble();
    m.x1 = o.value(QStringLiteral("x1")).toDouble();
    m.y1 = o.value(QStringLiteral("y1")).toDouble();
    m.cols = o.value(QStringLiteral("cols")).toInt();
    m.rows = o.value(QStringLiteral("rows")).toInt();
    const QJsonArray a = o.value(QStringLiteral("z")).toArray();
    for (const QJsonValue &v : a) {
        if (!v.isDouble())
            return {};
        m.z.append(v.toDouble());
    }
    return m.isValid() ? m : SurfaceMap{};
}

QString SurfaceMap::summary() const
{
    if (!isValid())
        return QStringLiteral("no map");
    double lo, hi;
    offsetRange(&lo, &hi);
    return QStringLiteral("%1 × %2 over %3 × %4 mm, %5 … %6 mm")
        .arg(cols).arg(rows)
        .arg(x1 - x0, 0, 'f', 0).arg(y1 - y0, 0, 'f', 0)
        .arg(lo, 0, 'f', 2).arg(hi >= 0 ? QStringLiteral("+%1").arg(hi, 0, 'f', 2)
                                        : QString::number(hi, 'f', 2));
}

QVector<GridPoint> surfaceMapProbeOrder(double x0, double y0, double x1, double y1,
                                       int cols, int rows)
{
    QVector<GridPoint> out;
    if (cols < 1 || rows < 1)
        return out;
    SurfaceMap g;
    g.x0 = x0; g.y0 = y0; g.x1 = x1; g.y1 = y1; g.cols = cols; g.rows = rows;
    for (int r = 0; r < rows; ++r)
        for (int k = 0; k < cols; ++k) {
            const int c = (r % 2 == 0) ? k : cols - 1 - k;
            out.append({g.xAt(c), g.yAt(r), r * cols + c});
        }
    return out;
}

// Highest offset under a straight XY move. Bilinear is not linear inside a
// cell, so the ends alone can miss a high spot in between: sample at a
// quarter of the smallest cell (or `step`, if finer) as well.
static double maxOffsetAlong(const SurfaceMap &map, double ax, double ay, double bx, double by,
                             double step)
{
    if (map.cols > 1)
        step = std::min(step, (map.x1 - map.x0) / (map.cols - 1) / 4.0);
    if (map.rows > 1)
        step = std::min(step, (map.y1 - map.y0) / (map.rows - 1) / 4.0);
    step = std::max(0.1, step);
    double hi = std::max(map.offsetAt(ax, ay), map.offsetAt(bx, by));
    const int n = int(std::ceil(std::hypot(bx - ax, by - ay) / step));
    for (int k = 1; k < n; ++k) {
        const double t = double(k) / n;
        hi = std::max(hi, map.offsetAt(ax + (bx - ax) * t, ay + (by - ay) * t));
    }
    return hi;
}

QVector<Op> applySurfaceMap(const QVector<Op> &ops, const SurfaceMap &map, double maxSegment)
{
    if (!map.isValid())
        return ops;
    maxSegment = std::max(0.1, maxSegment);
    QVector<Op> out;
    out.reserve(ops.size() * 2);
    bool known = false;
    double px = 0, py = 0, pz = 0;   // program position, before the offset

    auto feedTo = [&](double x, double y, double z, double f) {
        out.append(Op::feedTo(x, y, z + map.offsetAt(x, y), f));
    };

    for (const Op &op : ops) {
        switch (op.kind) {
        case Op::Rapid: {
            // A rapid travels at its Z plus the highest surface under the
            // whole move, not just under its end: a long move at low
            // clearance would otherwise cross a high spot. With no known
            // start (program start, after a tool change) that is the highest
            // point of the map. Then it drops to the offset at its end, so
            // what follows starts where the program expects.
            const double end = map.offsetAt(op.x, op.y);
            double lift = end;
            if (!known) {
                double lo, hi;
                map.offsetRange(&lo, &hi);
                lift = std::max(end, hi);
            } else if (op.x != px || op.y != py) {
                lift = maxOffsetAlong(map, px, py, op.x, op.y, maxSegment);
            }
            Op o = op;
            o.z += lift;
            out.append(o);
            if (lift > end + 1e-6)
                out.append(Op::rapid(op.x, op.y, op.z + end));
            break;
        }
        case Op::Feed: {
            const double len = known ? std::hypot(op.x - px, op.y - py) : 0;
            const int n = std::max(1, int(std::ceil(len / maxSegment - 1e-9)));
            for (int k = 1; k <= n; ++k) {
                const double t = double(k) / n;
                if (k == n)
                    feedTo(op.x, op.y, op.z, op.feed);
                else
                    feedTo(px + (op.x - px) * t, py + (op.y - py) * t,
                           pz + (op.z - pz) * t, op.feed);
            }
            break;
        }
        case Op::Arc: {
            if (!known) {
                Op o = op;
                o.z += map.offsetAt(op.x, op.y);
                out.append(o);
                break;
            }
            const double cx = px + op.ci, cy = py + op.cj;
            const double r0 = std::hypot(px - cx, py - cy);
            const double r1 = std::hypot(op.x - cx, op.y - cy);
            const double a0 = std::atan2(py - cy, px - cx);
            const double a1 = std::atan2(op.y - cy, op.x - cx);
            const double twoPi = 2.0 * M_PI;
            double sweep = op.cw ? a0 - a1 : a1 - a0;   // positive, in the arc's direction
            while (sweep <= 1e-9)
                sweep += twoPi;
            while (sweep > twoPi + 1e-9)
                sweep -= twoPi;
            const double r = std::max(r0, r1);
            const double tol = 0.005;
            const double step = r > tol ? 2.0 * std::acos(1.0 - tol / r) : sweep;
            const int n = std::max({1, int(std::ceil(sweep / step)),
                                    int(std::ceil(sweep * r / maxSegment))});
            const double dir = op.cw ? -1.0 : 1.0;
            for (int k = 1; k <= n; ++k) {
                const double t = double(k) / n;
                if (k == n) {
                    feedTo(op.x, op.y, op.z, op.feed);
                } else {
                    const double a = a0 + dir * sweep * t;
                    const double rr = r0 + (r1 - r0) * t;
                    feedTo(cx + rr * std::cos(a), cy + rr * std::sin(a),
                           pz + (op.z - pz) * t, op.feed);
                }
            }
            break;
        }
        case Op::Tool:
            known = false;
            out.append(op);
            continue;
        default:
            out.append(op);
            continue;
        }
        known = true;
        px = op.x;
        py = op.y;
        pz = op.z;
    }
    return out;
}

static QString mm(double v) { return QString::number(v, 'f', 3); }

QStringList surfaceMapMoveLines(const SurfaceMapPlan &plan, double x, double y)
{
    return {QStringLiteral("G90"),
            QStringLiteral("G0 Z%1").arg(mm(plan.travelZ())),
            QStringLiteral("G0 X%1 Y%2").arg(mm(x), mm(y))};
}

QStringList surfaceMapProbeLines(const SurfaceMapPlan &plan)
{
    const QString fast = QString::number(plan.fastFeed, 'f', 0);
    const QString slow = QString::number(plan.slowFeed, 'f', 0);
    return {QStringLiteral("G91"),
            QStringLiteral("G38.2 Z-%1 F%2").arg(mm(plan.clearance + plan.maxDeviation), fast),
            QStringLiteral("G0 Z%1").arg(mm(plan.retract)),
            QStringLiteral("G38.2 Z-%1 F%2").arg(mm(plan.retract + 2.0), slow),
            QStringLiteral("G90"),
            QStringLiteral("G0 Z%1").arg(mm(plan.travelZ()))};
}

// ---- prober -----------------------------------------------------------------

SurfaceMapProber::SurfaceMapProber(GrblStreamer *grbl, QObject *parent)
    : QObject(parent), m_grbl(grbl)
{
    connect(m_grbl, &GrblStreamer::probeResult, this, &SurfaceMapProber::onProbe);
    connect(m_grbl, &GrblStreamer::macroFinished, this, &SurfaceMapProber::onMacroFinished);
    connect(m_grbl, &GrblStreamer::disconnected, this, [this] {
        if (isRunning())
            fail(QStringLiteral("the controller disconnected"));
    });
}

// How far from its grid point a touch may be and still count for it.
static const double kOnPointTol = 0.5;

void SurfaceMapProber::start(const SurfaceMapPlan &plan)
{
    m_plan = plan;
    m_order = surfaceMapProbeOrder(plan.x0, plan.y0, plan.x1, plan.y1, plan.cols, plan.rows);
    m_map = SurfaceMap{};
    m_map.x0 = plan.x0; m_map.y0 = plan.y0; m_map.x1 = plan.x1; m_map.y1 = plan.y1;
    m_map.cols = plan.cols;
    m_map.rows = plan.rows;
    m_map.z = QVector<double>(plan.cols * plan.rows, 0.0);
    m_current = 0;
    if (m_order.size() < 2) {
        fail(QStringLiteral("a map needs at least two points"));
        return;
    }
    moveToCurrent();
}

void SurfaceMapProber::moveToCurrent()
{
    const GridPoint &p = m_order.at(m_current);
    m_stage = Stage::Moving;
    m_grbl->startMacro(surfaceMapMoveLines(m_plan, p.x, p.y));
}

void SurfaceMapProber::probeNext()
{
    if (m_stage != Stage::Waiting)
        return;
    // Only when someone has just put the plate down: straight after a move the
    // last status report can still be from before it. G38.2 refuses to start
    // on a closed input anyway (ALARM:4), so the unattended run stays safe.
    if (m_plan.pauseEachPoint && m_grbl->status().valid && m_grbl->status().probePin) {
        emit failed(QStringLiteral("the probe input is already closed: is the tool resting "
                                   "on the plate, or the magnet on the plate itself?"));
        return;   // still waiting: fix it and press again
    }
    // The head may have been jogged sideways while waiting (to get over the
    // plate). A touch there would be filed under the planned point, which
    // the map then holds as the height at the wrong place. Go back over the
    // point instead (the move lifts first) and wait again.
    if (m_plan.pauseEachPoint && m_grbl->status().valid) {
        const GridPoint &p = m_order.at(m_current);
        const double off = std::hypot(m_grbl->status().wx - p.x, m_grbl->status().wy - p.y);
        if (off > kOnPointTol) {
            emit failed(QStringLiteral("the head is %1 mm off point %2: going back over it — "
                                       "put the plate under the tool there and probe again")
                            .arg(off, 0, 'f', 1).arg(m_current + 1));
            moveToCurrent();
            return;
        }
    }
    m_hits.clear();
    m_hitValid = false;
    m_stage = Stage::Probing;
    emit probing(m_current + 1, m_order.size());
    m_grbl->startMacro(surfaceMapProbeLines(m_plan));
}

void SurfaceMapProber::cancel()
{
    m_stage = Stage::Idle;
    m_hits.clear();
}

void SurfaceMapProber::fail(const QString &why)
{
    m_stage = Stage::Idle;
    m_hits.clear();
    emit failed(why);
}

void SurfaceMapProber::onProbe(double mx, double my, double mz, bool ok)
{
    if (m_stage != Stage::Probing || !ok)
        return;
    m_hits.append(mz);
    // Where the touch really was, in work XY. GRBL reports probe hits in
    // machine coordinates; the last status gives the work offset.
    const MachineStatus &st = m_grbl->status();
    m_hitValid = st.valid;
    m_hitX = st.valid ? mx - (st.mx - st.wx) : 0;
    m_hitY = st.valid ? my - (st.my - st.wy) : 0;
}

void SurfaceMapProber::onMacroFinished(bool ok)
{
    const int total = m_order.size();
    switch (m_stage) {
    case Stage::Idle:
    case Stage::Waiting:
        return;
    case Stage::Moving:
        if (!ok) {
            fail(QStringLiteral("could not move over point %1").arg(m_current + 1));
            return;
        }
        m_stage = Stage::Waiting;
        if (m_plan.pauseEachPoint)
            emit waitingForPlate(m_current + 1, total);
        else
            probeNext();
        return;
    case Stage::Probing: {
        if (!ok || m_hits.size() < 2) {
            fail(QStringLiteral("no contact at point %1").arg(m_current + 1));
            return;
        }
        const GridPoint &p = m_order.at(m_current);
        const double stock = m_hits.last() - m_plan.plate;
        // Report the point where the probe actually touched, and never file a
        // touch taken elsewhere under this grid point.
        const double hx = m_hitValid ? m_hitX : p.x, hy = m_hitValid ? m_hitY : p.y;
        const double off = std::hypot(hx - p.x, hy - p.y);
        if (off > kOnPointTol) {
            const QString why = QStringLiteral("point %1 was probed %2 mm away from where it belongs")
                                    .arg(m_current + 1).arg(off, 0, 'f', 1);
            // Unattended nobody can have moved the head, so trying again
            // would only repeat whatever went wrong.
            if (!m_plan.pauseEachPoint) {
                fail(why);
                return;
            }
            emit failed(why + QStringLiteral(": not used — going back over it"));
            moveToCurrent();
            return;
        }
        m_map.z[p.index] = stock;
        emit pointMeasured(m_current + 1, total, hx, hy, stock);
        if (++m_current < total) {
            moveToCurrent();
            return;
        }
        m_stage = Stage::Idle;
        emit finished(m_map);
        return;
    }
    }
}

} // namespace c2d
