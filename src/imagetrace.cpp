#include "imagetrace.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QLineF>
#include <QPainter>
#include <QUuid>
#include <QtMath>

#include <vector>

namespace c2d {

// ---- raster stage --------------------------------------------------------

// Luminance image with any transparency composited over white (a transparent
// PNG logo traces as "ink on paper", not "ink on black").
static QImage toGray(const QImage &src)
{
    QImage in = src;
    if (in.hasAlphaChannel()) {
        QImage flat(in.size(), QImage::Format_RGB32);
        flat.fill(Qt::white);
        QPainter p(&flat);
        p.drawImage(0, 0, in);
        p.end();
        in = flat;
    }
    return in.convertToFormat(QImage::Format_Grayscale8);
}

// Separable box blur with a running window; O(pixels) per pass.
static QImage boxBlur(const QImage &gray, int r)
{
    if (r <= 0)
        return gray;
    const int w = gray.width(), h = gray.height();
    QImage tmp(w, h, QImage::Format_Grayscale8);
    QImage out(w, h, QImage::Format_Grayscale8);
    const int win = 2 * r + 1;
    // horizontal
    for (int y = 0; y < h; ++y) {
        const uchar *s = gray.constScanLine(y);
        uchar *d = tmp.scanLine(y);
        int sum = 0;
        for (int x = -r; x <= r; ++x)
            sum += s[qBound(0, x, w - 1)];
        for (int x = 0; x < w; ++x) {
            d[x] = uchar(sum / win);
            sum += s[qMin(w - 1, x + r + 1)] - s[qMax(0, x - r)];
        }
    }
    // vertical: running column sums walked row by row (cache-friendly).
    std::vector<int> sum(size_t(w), 0);
    auto rowAt = [&](int y) { return tmp.constScanLine(qBound(0, y, h - 1)); };
    for (int y = -r; y <= r; ++y) {
        const uchar *s = rowAt(y);
        for (int x = 0; x < w; ++x) sum[size_t(x)] += s[x];
    }
    for (int y = 0; y < h; ++y) {
        uchar *d = out.scanLine(y);
        for (int x = 0; x < w; ++x) d[x] = uchar(sum[size_t(x)] / win);
        const uchar *add = rowAt(y + r + 1), *sub = rowAt(y - r);
        for (int x = 0; x < w; ++x) sum[size_t(x)] += add[x] - sub[x];
    }
    return out;
}

QImage traceMask(const QImage &src, const TraceOptions &o)
{
    if (src.isNull())
        return QImage();
    const QImage g = boxBlur(toGray(src), o.blurRadius);
    const int w = g.width(), h = g.height();
    QImage mask(w, h, QImage::Format_Grayscale8);
    const int thr = qBound(0, o.threshold, 255);
    for (int y = 0; y < h; ++y) {
        const uchar *s = g.constScanLine(y);
        uchar *d = mask.scanLine(y);
        for (int x = 0; x < w; ++x) {
            const bool ink = o.invert ? (s[x] >= thr) : (s[x] < thr);
            d[x] = ink ? 255 : 0;
        }
    }
    return mask;
}

// ---- contour extraction ----------------------------------------------------
//
// Walk the "cracks" between ink and non-ink pixels keeping ink on the left
// (image coordinates, Y-down). Lattice vertex (i,j) is the top-left corner of
// pixel (i,j); the four pixels around it are TL=(i-1,j-1) TR=(i,j-1)
// BL=(i-1,j) BR=(i,j). A directed edge is valid when the pixel on its left
// is ink and the one on its right is not, so every undirected crack edge has
// exactly one valid direction. At saddle vertices we prefer the left turn,
// which keeps diagonally touching ink pixels in separate loops (4-connected
// ink, simple polygons — no self-touching output).

namespace {

struct Grid {
    int w, h;
    const uchar *px;
    int stride;
    bool ink(int x, int y) const {
        return x >= 0 && y >= 0 && x < w && y < h && px[y * stride + x] != 0;
    }
};

enum Dir { R = 0, D = 1, L = 2, U = 3 };
static const int DX[4] = {1, 0, -1, 0};
static const int DY[4] = {0, 1, 0, -1};

// Is the directed edge starting at vertex (i,j) heading `d` a valid boundary
// edge (ink on the left, non-ink on the right)?
static inline bool valid(const Grid &g, int i, int j, int d)
{
    switch (d) {
    case R: return g.ink(i, j - 1) && !g.ink(i, j);
    case D: return g.ink(i, j) && !g.ink(i - 1, j);
    case L: return g.ink(i - 1, j) && !g.ink(i - 1, j - 1);
    default: return g.ink(i - 1, j - 1) && !g.ink(i, j - 1);
    }
}

} // namespace

static QVector<QPolygonF> extractLoops(const QImage &mask, std::vector<double> *signedAreaPx)
{
    QVector<QPolygonF> loops;
    const int w = mask.width(), h = mask.height();
    if (w <= 0 || h <= 0)
        return loops;
    const Grid g{w, h, mask.constBits(), int(mask.bytesPerLine())};

    // Visited flags per undirected crack edge. Horizontal edge (i,j)->(i+1,j):
    // index j*w + i over (w × (h+1)). Vertical edge (i,j)->(i,j+1):
    // index j*(w+1) + i over ((w+1) × h).
    std::vector<uchar> visH(size_t(w) * (h + 1), 0), visV(size_t(w + 1) * h, 0);
    auto markEdge = [&](int i, int j, int d) {
        switch (d) {
        case R: visH[size_t(j) * w + i] = 1; break;
        case L: visH[size_t(j) * w + (i - 1)] = 1; break;
        case D: visV[size_t(j) * (w + 1) + i] = 1; break;
        default: visV[size_t(j - 1) * (w + 1) + i] = 1; break;
        }
    };

    QPolygonF loop;
    for (int j = 0; j <= h; ++j) {
        const uchar *rowAbove = j > 0 ? g.px + size_t(j - 1) * g.stride : nullptr;
        const uchar *rowBelow = j < h ? g.px + size_t(j) * g.stride : nullptr;
        const uchar *vis = visH.data() + size_t(j) * w;
        for (int i = 0; i < w; ++i) {
            // Horizontal crack between pixel rows j-1 and j at column i.
            const bool above = rowAbove && rowAbove[i], below = rowBelow && rowBelow[i];
            if (above == below || vis[i])
                continue;
            // Every loop crosses a horizontal edge, so scanning those finds all.
            int ci = i, cj = j, d = above ? R : L;
            if (d == L) { ci = i + 1; }   // edge (i+1,j)->(i,j)
            const int si = ci, sj = cj, sd = d;
            loop.clear();
            double area2 = 0;
            for (;;) {
                markEdge(ci, cj, d);
                const int ni = ci + DX[d], nj = cj + DY[d];
                area2 += double(ci) * nj - double(ni) * cj;
                // Emit a vertex only when the direction changes (drops the
                // collinear lattice points up front).
                int nd = -1;
                const int prefs[3] = {(d + 3) & 3, d, (d + 1) & 3};   // left, straight, right
                for (int k = 0; k < 3; ++k)
                    if (valid(g, ni, nj, prefs[k])) { nd = prefs[k]; break; }
                if (nd != d)
                    loop.append(QPointF(ni, nj));
                ci = ni; cj = nj;
                if (nd < 0)
                    break;   // cannot happen on a consistent mask
                d = nd;
                if (ci == si && cj == sj && d == sd)
                    break;
            }
            if (loop.size() >= 3) {
                loops.append(loop);
                if (signedAreaPx)
                    signedAreaPx->push_back(area2 * 0.5);
            }
        }
    }
    return loops;
}

// ---- simplification ----------------------------------------------------

static double segDist(const QPointF &p, const QPointF &a, const QPointF &b)
{
    const QPointF ab = b - a;
    const double len2 = ab.x() * ab.x() + ab.y() * ab.y();
    if (len2 <= 1e-18)
        return QLineF(p, a).length();
    double t = ((p.x() - a.x()) * ab.x() + (p.y() - a.y()) * ab.y()) / len2;
    t = qBound(0.0, t, 1.0);
    return QLineF(p, a + t * ab).length();
}

// Iterative Douglas–Peucker over pts[first..last] (inclusive ends kept).
static void dpOpen(const QPolygonF &pts, int first, int last, double tol, std::vector<char> &keep)
{
    std::vector<QPair<int, int>> stack;
    stack.push_back({first, last});
    while (!stack.empty()) {
        const auto seg = stack.back();
        stack.pop_back();
        const int a = seg.first, b = seg.second;
        if (b - a < 2)
            continue;
        double best = -1;
        int bi = -1;
        for (int i = a + 1; i < b; ++i) {
            const double d = segDist(pts[i], pts[a], pts[b]);
            if (d > best) { best = d; bi = i; }
        }
        if (best > tol && bi >= 0) {
            keep[bi] = 1;
            stack.push_back({a, bi});
            stack.push_back({bi, b});
        }
    }
}

QPolygonF simplifyClosed(const QPolygonF &ring, double tol)
{
    const int n = ring.size();
    if (n < 4 || tol <= 0)
        return ring;
    // Split the ring at the vertex farthest from vertex 0 so both halves are
    // open polylines with fixed ends.
    int far = 1;
    double fd = -1;
    for (int i = 1; i < n; ++i) {
        const double d = QLineF(ring[0], ring[i]).length();
        if (d > fd) { fd = d; far = i; }
    }
    std::vector<char> keep(n, 0);
    keep[0] = keep[far] = 1;
    dpOpen(ring, 0, far, tol, keep);
    // second half: far..n-1 plus the wrap back to 0
    QPolygonF wrap;
    wrap.reserve(n - far + 1);
    for (int i = far; i < n; ++i) wrap.append(ring[i]);
    wrap.append(ring[0]);
    std::vector<char> keep2(wrap.size(), 0);
    keep2.front() = keep2.back() = 1;
    dpOpen(wrap, 0, wrap.size() - 1, tol, keep2);
    for (int k = 1; k + 1 < int(keep2.size()); ++k)
        if (keep2[k]) keep[far + k] = 1;

    QPolygonF out;
    for (int i = 0; i < n; ++i)
        if (keep[i]) out.append(ring[i]);
    if (out.size() < 3)
        return ring;
    return out;
}

static double ringArea(const QPolygonF &r)
{
    double a = 0;
    const int n = r.size();
    for (int i = 0; i < n; ++i) {
        const QPointF &p = r[i], &q = r[(i + 1) % n];
        a += p.x() * q.y() - q.x() * p.y();
    }
    return a * 0.5;
}

// Direction change at each vertex vs. the corner threshold.
static QVector<bool> findCorners(const QPolygonF &r, double cornerAngleDeg)
{
    const int n = r.size();
    QVector<bool> c(n, false);
    if (n < 3)
        return c;
    const double thr = qDegreesToRadians(cornerAngleDeg);
    for (int i = 0; i < n; ++i) {
        const QPointF a = r[(i + n - 1) % n], b = r[i], d = r[(i + 1) % n];
        const QPointF u = b - a, v = d - b;
        const double lu = std::hypot(u.x(), u.y()), lv = std::hypot(v.x(), v.y());
        if (lu < 1e-12 || lv < 1e-12) { c[i] = true; continue; }
        const double cosang = qBound(-1.0, (u.x() * v.x() + u.y() * v.y()) / (lu * lv), 1.0);
        c[i] = std::acos(cosang) >= thr;
    }
    return c;
}

// ---- driver ----------------------------------------------------------------

TraceResult traceImage(const QImage &src, const TraceOptions &o)
{
    TraceResult res;
    QElapsedTimer timer;
    timer.start();
    const QImage mask = traceMask(src, o);
    if (mask.isNull())
        return res;
    res.width = mask.width();
    res.height = mask.height();
    for (int y = 0; y < mask.height(); ++y) {
        const uchar *s = mask.constScanLine(y);
        for (int x = 0; x < mask.width(); ++x)
            res.inkPixels += s[x] ? 1 : 0;
    }

    std::vector<double> areaPx;
    const QVector<QPolygonF> loops = extractLoops(mask, &areaPx);

    const double s = o.mmPerPixel > 0 ? o.mmPerPixel : 1.0;
    const double tolPx = o.simplifyTolMm > 0 ? o.simplifyTolMm / s : 0;
    const double minAreaPx = o.minAreaMm2 > 0 ? o.minAreaMm2 / (s * s) : 0;
    const double H = mask.height();

    for (int k = 0; k < loops.size(); ++k) {
        const double aPx = areaPx[size_t(k)];       // negative = outer (Y-down walk)
        if (std::fabs(aPx) < minAreaPx)
            continue;
        QPolygonF ring = simplifyClosed(loops[k], tolPx);
        // pixel lattice -> mm, Y-up, offset to the origin
        for (QPointF &p : ring)
            p = QPointF(o.origin.x() + p.x() * s, o.origin.y() + (H - p.y()) * s);
        TraceContour c;
        c.hole = aPx > 0;
        c.area = std::fabs(ringArea(ring));
        if (c.area < 1e-12)
            continue;
        if (o.smooth)
            c.corner = findCorners(ring, o.cornerAngleDeg);
        c.pts = ring;
        res.contours.append(c);
    }
    res.elapsedMs = timer.elapsed();
    return res;
}

// ---- CC element output ----------------------------------------------------

static QJsonArray xy(const QPointF &p) { return QJsonArray{p.x(), p.y()}; }

// Catmull-Rom tangents at smooth vertices; zero-length tangents at corners so
// the curve enters/leaves them straight and the corner stays sharp.
static QVector<QPointF> tangents(const QPolygonF &r, const QVector<bool> &corner)
{
    const int n = r.size();
    QVector<QPointF> t(n, QPointF(0, 0));
    for (int i = 0; i < n; ++i) {
        if (i < corner.size() && corner[i])
            continue;
        const QPointF a = r[(i + n - 1) % n], b = r[(i + 1) % n];
        t[i] = (b - a) * 0.5;
        // Keep the handle no longer than the shorter adjacent edge / 3, so
        // the curve cannot loop past a neighbouring vertex.
        const double lp = QLineF(r[i], a).length(), ln = QLineF(r[i], b).length();
        const double lim = qMin(lp, ln);
        const double lt = std::hypot(t[i].x(), t[i].y());
        if (lt > lim && lt > 1e-12)
            t[i] *= lim / lt;
    }
    return t;
}

Element traceContourElement(const TraceContour &c, bool smooth, const QJsonObject &layer)
{
    const QPolygonF &v = c.pts;
    const int n = v.size();
    if (n < 3 || !smooth || c.corner.size() != n)
        return Element::makePath(QVector<QPointF>(v.begin(), v.end()), true, layer);

    // Rows: v[0] (type 0), v[1..n-1] (cubic arriving), v[0] again (cubic
    // arriving from v[n-1]), then the close row (type 4) — makePath's shape
    // with cubic rows where CC expects cp1/cp2 = the two handles of the
    // segment arriving at the row's anchor (absolute mm, position [0,0]).
    const QVector<QPointF> t = tangents(v, c.corner);
    QJsonArray points, cp1, cp2, ptype, sm;
    auto addRow = [&](const QPointF &p, const QPointF &h1, const QPointF &h2, int type, int smoothFlag) {
        points.append(xy(p)); cp1.append(xy(h1)); cp2.append(xy(h2));
        ptype.append(type); sm.append(smoothFlag);
    };
    addRow(v[0], v[0], v[0], 0, c.corner[0] ? 0 : 1);
    for (int i = 1; i <= n; ++i) {
        const int prev = i - 1, cur = i % n;
        const bool straight = c.corner[prev] && c.corner[cur];
        if (straight)
            addRow(v[cur], v[prev], v[cur], 1, 0);
        else
            addRow(v[cur], v[prev] + t[prev] / 3.0, v[cur] - t[cur] / 3.0, 3,
                   c.corner[cur] ? 0 : 1);
    }
    addRow(v[0], v[0], v[0], 4, 1);

    QJsonObject o;
    o.insert("behavior", 0);
    o.insert("geometryType", QStringLiteral("path"));
    o.insert("group_id", QJsonArray());
    o.insert("id", QUuid::createUuid().toString());
    o.insert("layer", layer);
    o.insert("position", xy(QPointF(0, 0)));
    o.insert("tabs", QJsonArray());
    o.insert("points", points);
    o.insert("cp1", cp1);
    o.insert("cp2", cp2);
    o.insert("point_type", ptype);
    o.insert("smooth", sm);
    return Element::fromJson(o);
}

QPainterPath traceContourPath(const TraceContour &c, bool smooth)
{
    return traceContourElement(c, smooth, QJsonObject()).painterPath;
}

} // namespace c2d
