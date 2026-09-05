#include "simulation.h"

#include <QColor>
#include <QElapsedTimer>
#include <QThread>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <limits>

namespace c2d {

// ---------------------------------------------------------------------------
// HeightMap

bool HeightMap::cellOf(double x, double y, int *ix, int *iy) const
{
    if (isNull())
        return false;
    const int cx = int(std::floor((x - m_ox) / m_cell));
    const int cy = int(std::floor((y - m_oy) / m_cell));
    if (cx < 0 || cy < 0 || cx >= m_w || cy >= m_h)
        return false;
    *ix = cx;
    *iy = cy;
    return true;
}

double HeightMap::sample(double x, double y) const
{
    int ix, iy;
    if (!cellOf(x, y, &ix, &iy))
        return std::numeric_limits<double>::quiet_NaN();
    return at(ix, iy);
}

namespace {

// Tool cutting profile: height of the cutting surface above the tip at radial
// distance rho from the axis (rho <= radius), and the radius within which that
// surface is below the stock top for a tip at depth zc.
struct Profile {
    ToolGeom::Kind kind = ToolGeom::Flat;
    double r = 1;        // tool radius
    double c = 0;        // corner radius (ball: == r)
    double k = 0;        // V-bit: height per unit rho = 1 / tan(half angle)
    double flatR = 1;    // bull nose: radius of the flat bottom (r - c)

    bool flatBottom() const { return kind == ToolGeom::Flat && c <= 0; }

    double heightAt(double rho) const
    {
        switch (kind) {
        case ToolGeom::VBit:
            return rho * k;
        case ToolGeom::Ball:
            return r - std::sqrt(std::max(0.0, r * r - rho * rho));
        case ToolGeom::Flat:
        default: {
            if (c <= 0 || rho <= flatR)
                return 0;
            const double d = rho - flatR;
            return c - std::sqrt(std::max(0.0, c * c - d * d));
        }
        }
    }

    double cutRadius(double zc) const
    {
        const double depth = -zc;
        switch (kind) {
        case ToolGeom::VBit:
            return k > 0 ? std::min(r, depth / k) : r;
        case ToolGeom::Ball: {
            if (depth >= r)
                return r;
            const double a = r - depth;
            return std::sqrt(std::max(0.0, r * r - a * a));
        }
        case ToolGeom::Flat:
        default: {
            if (c <= 0 || depth >= c)
                return r;
            const double a = c - depth;
            return flatR + std::sqrt(std::max(0.0, c * c - a * a));
        }
        }
    }
};

Profile profileOf(const ToolGeom &g)
{
    Profile p;
    p.kind = g.kind;
    p.r = std::max(0.01, g.radius());
    switch (g.kind) {
    case ToolGeom::VBit: {
        const double half = qDegreesToRadians(qBound(5.0, g.angle > 0 ? g.angle : 60.0, 179.0) / 2);
        p.k = 1.0 / std::tan(half);
        p.c = 0;
        p.flatR = 0;
        break;
    }
    case ToolGeom::Ball:
        p.c = p.r;
        p.flatR = 0;
        break;
    case ToolGeom::Flat:
    default:
        p.c = qBound(0.0, g.cornerRadius, p.r);
        p.flatR = p.r - p.c;
        break;
    }
    return p;
}

// Stamps tool footprints into the map. A move is a capsule (segment ± radius)
// in the XY plane; each row of cells it touches is one contiguous span, found
// analytically (two end discs + the rectangle between them), so a flat end mill
// is a plain min() fill per row and only shaped tools evaluate a distance per
// cell.
struct Stamper {
    HeightMap &map;
    const int w, h;
    const double cell, ox, oy;
    float *const z;
    int stamped = 0;

    explicit Stamper(HeightMap &m)
        : map(m), w(m.width()), h(m.height()), cell(m.cellSize()),
          ox(m.originX()), oy(m.originY()), z(m.row(0)) {}

    void capsule(double ax, double ay, double bx, double by, double zc, const Profile &p)
    {
        if (zc >= -1e-9)
            return;
        const double r = p.cutRadius(zc);
        if (r <= 0)
            return;
        ++stamped;
        const bool flat = p.flatBottom();
        const double dx = bx - ax, dy = by - ay;
        const double L = std::hypot(dx, dy);
        const bool point = L < 1e-9;
        double qx[4] = {0, 0, 0, 0}, qy[4] = {0, 0, 0, 0};
        if (!point) {
            const double nx = -dy / L * r, ny = dx / L * r;
            qx[0] = ax + nx; qy[0] = ay + ny;
            qx[1] = bx + nx; qy[1] = by + ny;
            qx[2] = bx - nx; qy[2] = by - ny;
            qx[3] = ax - nx; qy[3] = ay - ny;
        }
        const double ymin = std::min(ay, by) - r, ymax = std::max(ay, by) + r;
        const int iy0 = std::max(0, int(std::ceil((ymin - oy) / cell - 0.5)));
        const int iy1 = std::min(h - 1, int(std::floor((ymax - oy) / cell - 0.5)));
        const float zf = float(zc);
        const double invL2 = point ? 0 : 1.0 / (L * L);

        for (int iy = iy0; iy <= iy1; ++iy) {
            const double yc = oy + (iy + 0.5) * cell;
            double xl = std::numeric_limits<double>::infinity(), xr = -xl;
            double d = yc - ay;
            if (std::fabs(d) <= r) {
                const double s = std::sqrt(r * r - d * d);
                xl = std::min(xl, ax - s); xr = std::max(xr, ax + s);
            }
            d = yc - by;
            if (std::fabs(d) <= r) {
                const double s = std::sqrt(r * r - d * d);
                xl = std::min(xl, bx - s); xr = std::max(xr, bx + s);
            }
            if (!point) {
                for (int e = 0; e < 4; ++e) {
                    const int j = (e + 1) & 3;
                    const double y0 = qy[e], y1 = qy[j];
                    if ((y0 - yc) * (y1 - yc) > 0)
                        continue;
                    if (y0 == y1) {
                        xl = std::min(xl, std::min(qx[e], qx[j]));
                        xr = std::max(xr, std::max(qx[e], qx[j]));
                    } else {
                        const double x = qx[e] + (yc - y0) * (qx[j] - qx[e]) / (y1 - y0);
                        xl = std::min(xl, x); xr = std::max(xr, x);
                    }
                }
            }
            if (xl > xr)
                continue;
            const int ix0 = std::max(0, int(std::ceil((xl - ox) / cell - 0.5)));
            const int ix1 = std::min(w - 1, int(std::floor((xr - ox) / cell - 0.5)));
            if (ix0 > ix1)
                continue;
            float *row = z + qsizetype(iy) * w;
            if (flat) {
                for (int ix = ix0; ix <= ix1; ++ix)
                    if (row[ix] > zf)
                        row[ix] = zf;
            } else {
                for (int ix = ix0; ix <= ix1; ++ix) {
                    const double xc = ox + (ix + 0.5) * cell;
                    double rho;
                    if (point) {
                        rho = std::hypot(xc - ax, yc - ay);
                    } else {
                        double t = ((xc - ax) * dx + (yc - ay) * dy) * invL2;
                        t = t < 0 ? 0 : (t > 1 ? 1 : t);
                        rho = std::hypot(xc - (ax + t * dx), yc - (ay + t * dy));
                    }
                    const float hh = float(zc + p.heightAt(rho));
                    if (hh < row[ix])
                        row[ix] = hh;
                }
            }
        }
    }
};

} // namespace

// ---------------------------------------------------------------------------
// simulate()

SimResult simulate(const QVector<Op> &ops, const QHash<int, ToolGeom> &tools,
                   const SimSettings &s, std::atomic<bool> *cancel,
                   const SimProgress &progress)
{
    QElapsedTimer timer;
    timer.start();
    SimResult res;

    // Stock extents; fall back to the program's XY bounds (+ margin) when the
    // document has no usable stock size.
    double W = s.stockW, H = s.stockH, ox = 0, oy = 0;
    if (!(W > 0) || !(H > 0)) {
        bool any = false;
        double x0 = 0, x1 = 0, y0 = 0, y1 = 0;
        for (const Op &op : ops) {
            if (op.kind != Op::Rapid && op.kind != Op::Feed && op.kind != Op::Arc)
                continue;
            if (!any) { x0 = x1 = op.x; y0 = y1 = op.y; any = true; }
            x0 = std::min(x0, op.x); x1 = std::max(x1, op.x);
            y0 = std::min(y0, op.y); y1 = std::max(y1, op.y);
        }
        if (!any)
            return res;
        ox = x0 - 5; oy = y0 - 5;
        W = (x1 - x0) + 10; H = (y1 - y0) + 10;
    }
    const double minCell = std::max(0.01, s.minCell);
    const int maxCells = std::max(1000, s.maxCells);
    const double cell = std::max(minCell, std::sqrt(W * H / maxCells));
    const int w = std::max(1, int(std::ceil(W / cell - 1e-9)));
    const int h = std::max(1, int(std::ceil(H / cell - 1e-9)));
    res.map = HeightMap(w, h, cell, ox, oy);
    Stamper st(res.map);

    const double maxDz = std::max(0.005, s.maxDz);
    const double chord = std::max(0.01, s.chord);

    ToolGeom defTool;
    Profile prof = profileOf(defTool);
    double x = 0, y = 0, z = 0;
    bool have = false;

    // One straight tip move a→b: split sloped moves so each piece is stamped
    // at its lowest Z (error ≤ maxDz), plunges collapse to a single disc.
    auto piece = [&](double ax, double ay, double az, double bx, double by, double bz) {
        if (std::min(az, bz) >= -1e-9)
            return;
        const double dz = std::fabs(bz - az);
        const int n = dz > maxDz ? int(std::ceil(dz / maxDz)) : 1;
        for (int k = 0; k < n; ++k) {
            const double t0 = double(k) / n, t1 = double(k + 1) / n;
            const double zc = std::min(az + (bz - az) * t0, az + (bz - az) * t1);
            st.capsule(ax + (bx - ax) * t0, ay + (by - ay) * t0,
                       ax + (bx - ax) * t1, ay + (by - ay) * t1, zc, prof);
        }
    };

    const int n = ops.size();
    int lastPct = -1;
    for (int i = 0; i < n; ++i) {
        if ((i & 255) == 0) {
            if (cancel && cancel->load()) {
                res.cancelled = true;
                break;
            }
            if (progress) {
                const int pct = int(100.0 * i / std::max(1, n));
                if (pct != lastPct) {
                    lastPct = pct;
                    progress(pct);
                }
            }
        }
        const Op &op = ops.at(i);
        switch (op.kind) {
        case Op::Tool: {
            auto it = tools.constFind(op.ival);
            if (it != tools.constEnd()) {
                prof = profileOf(*it);
            } else {
                prof = profileOf(defTool);
                if (!res.missingTools.contains(op.ival))
                    res.missingTools.append(op.ival);
            }
            break;
        }
        case Op::Rapid:
            x = op.x; y = op.y; z = op.z; have = true;
            break;
        case Op::Feed:
            if (have) {
                piece(x, y, z, op.x, op.y, op.z);
                ++res.cutOps;
            }
            x = op.x; y = op.y; z = op.z; have = true;
            break;
        case Op::Arc: {
            if (!have) { x = op.x; y = op.y; z = op.z; have = true; break; }
            ++res.cutOps;
            const double cx = x + op.ci, cy = y + op.cj;
            const double r = std::hypot(x - cx, y - cy);
            const double a0 = std::atan2(y - cy, x - cx);
            double a1 = std::atan2(op.y - cy, op.x - cx);
            // G2 = clockwise in Y-up space = decreasing angle. start==end → full circle.
            if (op.cw) { while (a1 >= a0 - 1e-12) a1 -= 2 * M_PI; }
            else       { while (a1 <= a0 + 1e-12) a1 += 2 * M_PI; }
            const int steps = std::max(1, int(std::ceil(std::fabs(a1 - a0) * r / chord)));
            const double z0 = z;
            for (int k = 1; k <= steps; ++k) {
                const double a = a0 + (a1 - a0) * k / steps;
                const double nz = z0 + (op.z - z0) * k / steps;
                double nx = cx + r * std::cos(a), ny = cy + r * std::sin(a);
                if (k == steps) { nx = op.x; ny = op.y; }
                piece(x, y, z, nx, ny, nz);
                x = nx; y = ny; z = nz;
            }
            x = op.x; y = op.y; z = op.z;
            break;
        }
        default:
            break;
        }
    }

    float mz = 0;
    const QVector<float> &d = res.map.data();
    for (float v : d)
        if (v < mz)
            mz = v;
    res.minZ = mz;
    res.throughCut = s.stockT > 0 && mz <= -s.stockT + 1e-6;
    res.segments = st.stamped;
    res.elapsedMs = timer.nsecsElapsed() / 1e6;
    if (progress && !res.cancelled)
        progress(100);
    return res;
}

// ---------------------------------------------------------------------------
// Rendering

namespace {
const QColor kUncut(0xd9, 0xc5, 0x9e);     // light stock
const QColor kDeep(0x5c, 0x41, 0x24);      // full-depth floor
const QColor kThrough(0x2b, 0x3d, 0x5a);   // slate: nothing left, looking at the spoilboard
}

QColor simColorUncut()   { return kUncut; }
QColor simColorDeep()    { return kDeep; }
QColor simColorThrough() { return kThrough; }

QImage renderHeightMap(const HeightMap &map, double stockT)
{
    if (map.isNull())
        return QImage();
    const int w = map.width(), h = map.height();
    QImage img(w, h, QImage::Format_RGB32);

    float mz = 0;
    for (float v : map.data())
        if (v < mz)
            mz = v;
    const double T = stockT > 0 ? stockT : std::max(1e-3, double(-mz));
    const double thr = stockT > 0 ? -stockT + 1e-6 : -1e30;
    const double cell = map.cellSize();

    // Light from the upper left (−X, +Y), fairly low so walls read clearly.
    double lx = -1, ly = 1, lz = 0.6;
    const double ll = std::sqrt(lx * lx + ly * ly + lz * lz);
    lx /= ll; ly /= ll; lz /= ll;
    const double flatDot = lz;

    const int ur = kUncut.red(), ug = kUncut.green(), ub = kUncut.blue();
    const int dr = kDeep.red(), dg = kDeep.green(), db = kDeep.blue();
    const QRgb through = kThrough.rgb();

    for (int iy = 0; iy < h; ++iy) {
        QRgb *out = reinterpret_cast<QRgb *>(img.scanLine(h - 1 - iy));
        const float *row = map.row(iy);
        const float *rowN = map.row(std::min(h - 1, iy + 1));
        const float *rowS = map.row(std::max(0, iy - 1));
        const double dyDen = (std::min(h - 1, iy + 1) - std::max(0, iy - 1)) * cell;
        for (int ix = 0; ix < w; ++ix) {
            const float zc = row[ix];
            if (zc <= thr) {
                out[ix] = through;
                continue;
            }
            const int xe = std::min(w - 1, ix + 1), xw = std::max(0, ix - 1);
            const double dxDen = (xe - xw) * cell;
            const double dzdx = dxDen > 0 ? (row[xe] - row[xw]) / dxDen : 0;
            const double dzdy = dyDen > 0 ? (rowN[ix] - rowS[ix]) / dyDen : 0;
            double nx = -dzdx, ny = -dzdy, nz = 1;
            const double nl = std::sqrt(nx * nx + ny * ny + 1);
            nx /= nl; ny /= nl; nz /= nl;
            const double ndl = nx * lx + ny * ly + nz * lz;
            double shade = 0.35 + 0.65 * qBound(0.0, ndl / flatDot, 1.35);

            const bool cut = zc < -1e-4;
            const double f = cut ? 0.12 + 0.88 * qBound(0.0, double(-zc) / T, 1.0) : 0.0;
            const double r = (ur + (dr - ur) * f) * shade;
            const double g = (ug + (dg - ug) * f) * shade;
            const double b = (ub + (db - ub) * f) * shade;
            out[ix] = qRgb(qBound(0, int(r + 0.5), 255), qBound(0, int(g + 0.5), 255),
                           qBound(0, int(b + 0.5), 255));
        }
    }
    return img;
}

// ---------------------------------------------------------------------------
// SimulationJob

SimulationJob::SimulationJob(QObject *parent)
    : QObject(parent)
{
}

SimulationJob::~SimulationJob()
{
    m_cancel = true;
    if (m_thread)
        m_thread->wait();
}

void SimulationJob::start(const QVector<Op> &ops, const QHash<int, ToolGeom> &tools,
                          const SimSettings &settings)
{
    if (m_running)
        return;
    m_cancel = false;
    m_running = true;
    m_result = SimResult();
    QThread *t = QThread::create([this, ops, tools, settings] {
        m_result = simulate(ops, tools, settings, &m_cancel,
                            [this](int p) { emit progress(p); });
    });
    t->setParent(this);
    m_thread = t;
    connect(t, &QThread::finished, this, [this, t] {
        m_running = false;
        if (m_thread == t)
            m_thread = nullptr;
        t->deleteLater();
        emit finished(m_result.cancelled);
    });
    t->start();
}

} // namespace c2d
