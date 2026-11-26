#include "boxgen.h"
#include <algorithm>
#include <cmath>

namespace c2d {

namespace {

enum { Bottom = 0, Front, Back, LeftEnd, RightEnd, Lid };

// Point on edge k of a w x h part, `s` mm along the edge (counter-clockwise)
// and `d` mm in from it.
QPointF onEdge(int k, double w, double h, double s, double d)
{
    switch (k) {
    case 0: return {s, d};
    case 1: return {w - d, s};
    case 2: return {w - s, h - d};
    default: return {d, h - s};
    }
}

double edgeLength(const BoxPanel &p, int k) { return (k % 2) ? p.height : p.width; }

// Closed outline of one part from its edges' fingers and its corner blocks.
QVector<QPointF> outlineOf(const BoxPanel &p, double t, double c)
{
    const double half = c / 2;
    struct Bounds { QVector<bool> f; QVector<double> b; };
    Bounds e[4];
    for (int k = 0; k < 4; ++k) {
        e[k].f = p.edges[k].fingers.isEmpty() ? QVector<bool>{true} : p.edges[k].fingers;
        const int n = e[k].f.size();
        const double L = edgeLength(p, k);
        const double step = (L - 2 * t) / n;
        const bool cs = p.corners[(k + 3) % 4], ce = p.corners[k];
        for (int j = 0; j <= n; ++j) {
            const bool left = j == 0 ? cs : e[k].f[j - 1];
            const bool right = j == n ? ce : e[k].f[j];
            double s = t + j * step;
            if (left && !right) s -= half;       // the finger gives up half the gap
            else if (!left && right) s += half;
            e[k].b.append(s);
        }
    }
    QVector<QPointF> pts;
    for (int k = 0; k < 4; ++k) {
        const int n = e[k].f.size();
        for (int i = 0; i < n; ++i) {
            const double d = e[k].f[i] ? 0 : t;
            pts << onEdge(k, p.width, p.height, e[k].b[i], d)
                << onEdge(k, p.width, p.height, e[k].b[i + 1], d);
        }
        const int k2 = (k + 1) % 4;
        const double L = edgeLength(p, k);
        if (p.corners[k]) {
            pts << onEdge(k, p.width, p.height, e[k].b[n], 0)
                << onEdge(k, p.width, p.height, L, 0)
                << onEdge(k2, p.width, p.height, e[k2].b[0], 0);
        } else {
            const double ek = e[k].f.last() ? half : 0;
            const double ek2 = e[k2].f.first() ? half : 0;
            pts << onEdge(k, p.width, p.height, L - t - ek, t + ek2);
        }
    }
    // Drop repeated and in-line points.
    auto same = [](QPointF a, QPointF b) { return std::fabs(a.x() - b.x()) < 1e-9 && std::fabs(a.y() - b.y()) < 1e-9; };
    QVector<QPointF> u;
    for (const QPointF &q : pts)
        if (u.isEmpty() || !same(u.last(), q))
            u << q;
    while (u.size() > 1 && same(u.first(), u.last()))
        u.removeLast();
    bool changed = true;
    while (changed && u.size() > 3) {
        changed = false;
        for (int i = 0; i < u.size() && u.size() > 3; ++i) {
            const QPointF a = u[(i + u.size() - 1) % u.size()], b = u[i], cc = u[(i + 1) % u.size()];
            const double cross = (b.x() - a.x()) * (cc.y() - a.y()) - (b.y() - a.y()) * (cc.x() - a.x());
            if (std::fabs(cross) < 1e-9) { u.removeAt(i); changed = true; --i; }
        }
    }
    return u;
}

} // namespace

int boxFingerCount(double length, double wanted)
{
    if (length <= 0 || wanted <= 0)
        return 1;
    int n = std::max(1, int(std::lround(length / wanted)));
    if (n % 2 == 0) {
        const double lo = std::fabs(length / (n - 1) - wanted);
        const double hi = std::fabs(length / (n + 1) - wanted);
        n = lo <= hi ? n - 1 : n + 1;
    }
    return n;
}

double boxDefaultFinger(double thickness)
{
    return std::max(1.0, std::round(thickness * 3 * 10) / 10);
}

BoxResult makeFingerBox(const BoxSpec &s)
{
    BoxResult r;
    const double t = s.thickness;
    if (!(t > 0)) { r.error = QStringLiteral("Material thickness must be more than 0."); return r; }
    double W = s.width, D = s.depth, H = s.height;
    if (s.insideSize) { W += 2 * t; D += 2 * t; H += t + (s.lid ? t : 0); }
    r.outerW = W; r.outerD = D; r.outerH = H;
    const double minInner = std::min({W, D, H}) - 2 * t;
    if (minInner <= 2 * s.clearance + 0.5) {
        r.error = QStringLiteral("The box is too small for this material: each side needs to be well over twice the thickness.");
        return r;
    }
    if (!(s.fingerWidth > 2 * s.clearance)) {
        r.error = QStringLiteral("Fingers must be wider than twice the fit gap.");
        return r;
    }
    if (s.clearance < 0) { r.error = QStringLiteral("The fit gap can not be negative."); return r; }

    const int count = s.lid ? 6 : 5;
    r.panels.resize(count);
    const QString names[6] = {QStringLiteral("Bottom"), QStringLiteral("Front"), QStringLiteral("Back"),
                              QStringLiteral("Left end"), QStringLiteral("Right end"), QStringLiteral("Lid")};
    for (int i = 0; i < count; ++i)
        r.panels[i].name = names[i];
    auto size = [&](int i, double w, double h) { r.panels[i].width = w; r.panels[i].height = h; };
    size(Bottom, W, D); size(Front, W, H); size(Back, W, H);
    size(LeftEnd, D, H); size(RightEnd, D, H);
    if (s.lid) size(Lid, W, D);

    // Joints: (panel a, edge a, panel b, edge b, a has the fingers at the ends).
    struct Joint { int a, ea, b, eb; bool aEnds; };
    QVector<Joint> joints = {
        {Bottom, 0, Front, 0, true}, {Bottom, 1, RightEnd, 0, true},
        {Bottom, 2, Back, 0, true}, {Bottom, 3, LeftEnd, 0, true},
        {Front, 1, RightEnd, 3, true}, {Front, 3, LeftEnd, 1, true},
        {Back, 1, LeftEnd, 3, true}, {Back, 3, RightEnd, 1, true},
    };
    if (s.lid)
        joints += QVector<Joint>{{Lid, 0, Front, 2, true}, {Lid, 1, RightEnd, 2, true},
                                 {Lid, 2, Back, 2, true}, {Lid, 3, LeftEnd, 2, true}};
    for (const Joint &j : joints) {
        BoxPanel &a = r.panels[j.a];
        BoxPanel &b = r.panels[j.b];
        const int n = boxFingerCount(edgeLength(a, j.ea) - 2 * t, s.fingerWidth);
        QVector<bool> fa(n), fb(n);
        for (int i = 0; i < n; ++i) {
            fa[i] = ((i % 2) == 0) == j.aEnds;
            fb[i] = !fa[i];
        }
        a.edges[j.ea] = {j.b, j.eb, fa};
        b.edges[j.eb] = {j.a, j.ea, fb};
    }
    // Corner blocks: the bottom's and the lid's where they touch; the top
    // corners of an open box go to the front and back.
    for (int c = 0; c < 4; ++c) {
        r.panels[Bottom].corners[c] = true;
        if (s.lid) r.panels[Lid].corners[c] = true;
    }
    for (int p : {int(Front), int(Back)}) {
        r.panels[p].corners[1] = r.panels[p].corners[2] = !s.lid;
    }

    // Laid out left to right, a row at a time.
    const double gap = std::max(2 * s.bitDiameter, 1.0);
    double x = gap, y = gap, rowH = 0;
    for (BoxPanel &p : r.panels) {
        if (s.stockWidth > 0 && p.width + 2 * gap > s.stockWidth) {
            r.error = QStringLiteral("The %1 (%2 mm wide) does not fit across the %3 mm stock.")
                          .arg(p.name.toLower()).arg(p.width, 0, 'f', 1).arg(s.stockWidth, 0, 'f', 1);
            r.panels.clear();
            return r;
        }
        if (s.stockWidth > 0 && x > gap && x + p.width + gap > s.stockWidth) {
            x = gap; y += rowH + gap; rowH = 0;
        }
        p.origin = QPointF(x, y);
        p.outline = outlineOf(p, t, s.clearance);
        for (QPointF &q : p.outline) q += p.origin;
        x += p.width + gap;
        rowH = std::max(rowH, p.height);
    }
    return r;
}

} // namespace c2d
