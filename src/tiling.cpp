#include "tiling.h"
#include "c2ddocument.h"
#include "gcodeexport.h"

#include <QFile>
#include <QtMath>
#include <cmath>

namespace c2d {

namespace {

struct P3 {
    double x = 0, y = 0, z = 0;
};

static bool same(const P3 &a, const P3 &b)
{
    return qAbs(a.x - b.x) < 1e-6 && qAbs(a.y - b.y) < 1e-6 && qAbs(a.z - b.z) < 1e-6;
}

// Points along an arc op from `from` (exclusive) to its endpoint (inclusive),
// chord error <= 0.01 mm, Z interpolated linearly. Coincident endpoints mean
// a full circle, as in the post.
static QVector<P3> arcPoints(const P3 &from, const Op &op)
{
    const double cx = from.x + op.ci, cy = from.y + op.cj;
    const double r = std::hypot(op.ci, op.cj);
    const double a0 = std::atan2(from.y - cy, from.x - cx);
    const double a1 = std::atan2(op.y - cy, op.x - cx);
    double sweep = op.cw ? a0 - a1 : a1 - a0;
    while (sweep <= 1e-9) sweep += 2 * M_PI;
    while (sweep > 2 * M_PI) sweep -= 2 * M_PI;
    const double tol = 0.01;
    const double step = r > tol ? 2 * std::acos(1 - tol / r) : M_PI / 8;
    const int n = qMax(4, int(std::ceil(sweep / qMax(step, 1e-3))));
    QVector<P3> out;
    out.reserve(n);
    for (int i = 1; i <= n; ++i) {
        const double a = a0 + (op.cw ? -sweep : sweep) * i / n;
        P3 p;
        p.x = cx + r * std::cos(a);
        p.y = cy + r * std::sin(a);
        p.z = from.z + (op.z - from.z) * i / n;
        if (i == n) { p.x = op.x; p.y = op.y; p.z = op.z; }   // exact endpoint
        out.append(p);
    }
    return out;
}

static P3 lerp(const P3 &a, const P3 &b, double t)
{
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

// Clip segment a->b to the band lo <= y < hi (lo may be -inf). Returns false
// when nothing of positive length remains inside.
static bool clipToBand(const P3 &a, const P3 &b, double lo, double hi, P3 *ca, P3 *cb)
{
    const double eps = 1e-9;
    const double dy = b.y - a.y;
    if (qAbs(dy) < eps) {
        if (a.y < lo - eps || a.y >= hi - eps)
            return false;
        *ca = a;
        *cb = b;
    } else {
        double t0 = 0, t1 = 1;
        const double tLo = (lo - a.y) / dy, tHi = (hi - a.y) / dy;
        if (std::isfinite(tLo)) {
            if (dy > 0) t0 = qMax(t0, tLo); else t1 = qMin(t1, tLo);
        }
        if (dy > 0) t1 = qMin(t1, tHi); else t0 = qMax(t0, tHi);
        if (t1 - t0 <= 1e-9)
            return false;
        *ca = lerp(a, b, t0);
        *cb = lerp(a, b, t1);
        // A piece lying entirely on the upper boundary belongs to the next tile.
        if (ca->y >= hi - eps && cb->y >= hi - eps)
            return false;
    }
    const double len = std::hypot(std::hypot(cb->x - ca->x, cb->y - ca->y), cb->z - ca->z);
    return len > 1e-9;
}

struct TileState {
    QVector<Op> ops;
    bool havePos = false;
    P3 pos;          // last emitted position, untranslated
    int motions = 0;
};

// Comments, tool changes and spindle commands are mirrored into every tile as
// they are seen, because a tile's own cuts may come later. Afterwards, keep
// only the blocks that actually cut here: a block runs from one toolpath's
// preamble (its comment / tool change / spindle start) to the next, and is
// dropped whole when it contains no feed or arc — otherwise a tile would
// change tools and spin the spindle up for a toolpath it never machines.
static QVector<Op> dropUncutBlocks(const QVector<Op> &ops)
{
    auto isPreamble = [](const Op &o) {
        return o.kind == Op::Comment || o.kind == Op::Tool || o.kind == Op::Spindle;
    };
    QVector<Op> out, block;
    bool blockCuts = false, inPreamble = false;
    auto flush = [&] {
        if (blockCuts)
            out += block;
        block.clear();
        blockCuts = false;
    };
    for (const Op &o : ops) {
        // A spindle stop closes the block that just cut; it never opens one.
        const bool stop = (o.kind == Op::Spindle && o.ival <= 0);
        if (isPreamble(o) && !stop) {
            if (!inPreamble) {      // first preamble op after content: new block
                flush();
                inPreamble = true;
            }
        } else {
            // Content, or the spindle stop that closes this block: either way
            // the next tool change / comment starts a new block.
            inPreamble = false;
            if (o.kind == Op::Feed || o.kind == Op::Arc)
                blockCuts = true;
        }
        block.append(o);
    }
    flush();
    return out;
}

} // namespace

int tileCount(const QVector<Op> &ops, double tileHeight)
{
    if (tileHeight <= 0)
        return 1;
    double maxY = 0;
    P3 cur;
    for (const Op &op : ops) {
        if (op.kind == Op::Rapid || op.kind == Op::Feed) {
            maxY = qMax(maxY, op.y);
            cur = {op.x, op.y, op.z};
        } else if (op.kind == Op::Arc) {
            for (const P3 &p : arcPoints(cur, op))
                maxY = qMax(maxY, p.y);
            cur = {op.x, op.y, op.z};
        }
    }
    return qMax(1, int(std::floor((maxY - 1e-6) / tileHeight)) + 1);
}

QVector<QVector<Op>> tileOps(const QVector<Op> &ops, double tileHeight, double safeZ)
{
    const int n = tileCount(ops, tileHeight);
    QVector<TileState> tiles(n);
    auto lo = [&](int k) { return k == 0 ? -std::numeric_limits<double>::infinity() : k * tileHeight; };
    auto hi = [&](int k) { return (k + 1) * tileHeight; };

    P3 cur{0, 0, safeZ};
    double plungeFeed = 0;

    // Emit one cutting piece a->b into tile k, reconnecting through safe Z
    // when the tile's last position is not where the piece starts.
    auto emitPiece = [&](int k, const P3 &a, const P3 &b, const Op &src, bool asArc) {
        TileState &t = tiles[k];
        const double dy = -k * tileHeight;
        if (!t.havePos || !same(t.pos, a)) {
            if (t.havePos && t.pos.z < safeZ - 1e-9)
                t.ops.append(Op::rapid(t.pos.x, t.pos.y + dy, safeZ));
            t.ops.append(Op::rapid(a.x, a.y + dy, safeZ));
            if (a.z < safeZ - 1e-9)
                t.ops.append(Op::feedTo(a.x, a.y + dy, a.z,
                                        plungeFeed > 0 ? qMin(plungeFeed, src.feed) : src.feed));
        }
        if (asArc)
            t.ops.append(Op::arcTo(b.x, b.y + dy, b.z, src.ci, src.cj, src.cw, src.feed));
        else
            t.ops.append(Op::feedTo(b.x, b.y + dy, b.z, src.feed));
        t.pos = b;
        t.havePos = true;
        ++t.motions;
    };

    auto emitSegment = [&](const P3 &a, const P3 &b, const Op &src) {
        for (int k = 0; k < n; ++k) {
            P3 ca, cb;
            if (clipToBand(a, b, lo(k), hi(k), &ca, &cb))
                emitPiece(k, ca, cb, src, false);
        }
    };

    for (const Op &op : ops) {
        switch (op.kind) {
        case Op::Comment:
        case Op::Spindle:
        case Op::Tool:
            for (TileState &t : tiles)
                t.ops.append(op);
            break;
        case Op::Rapid:
            // Rapids are not copied: each tile re-links its pieces through
            // safe Z itself. Only the position is tracked.
            cur = {op.x, op.y, op.z};
            break;
        case Op::Feed: {
            const P3 to{op.x, op.y, op.z};
            if (qAbs(to.x - cur.x) < 1e-9 && qAbs(to.y - cur.y) < 1e-9 && to.z < cur.z)
                plungeFeed = op.feed;
            emitSegment(cur, to, op);
            cur = to;
            break;
        }
        case Op::Arc: {
            const QVector<P3> pts = arcPoints(cur, op);
            double minY = cur.y, maxY = cur.y;
            for (const P3 &p : pts) {
                minY = qMin(minY, p.y);
                maxY = qMax(maxY, p.y);
            }
            // Whole arc inside one band: keep it as an arc.
            int band = -1;
            for (int k = 0; k < n; ++k)
                if (minY >= lo(k) - 1e-9 && maxY < hi(k) - 1e-9) {
                    band = k;
                    break;
                }
            if (band < 0 && maxY <= hi(n - 1) + 1e-9 && minY >= lo(n - 1) - 1e-9)
                band = n - 1;   // touching the top of the last tile
            if (band >= 0) {
                emitPiece(band, cur, {op.x, op.y, op.z}, op, true);
            } else {
                P3 a = cur;
                for (const P3 &p : pts) {
                    emitSegment(a, p, op);
                    a = p;
                }
            }
            cur = {op.x, op.y, op.z};
            break;
        }
        }
    }

    QVector<QVector<Op>> out;
    out.reserve(n);
    for (int k = 0; k < n; ++k) {
        TileState &t = tiles[k];
        // Comments, tool changes and spindle commands were mirrored into every
        // tile as they were seen. A tile that ended up with no motion at all
        // would otherwise be a program that changes tools and spins up the
        // spindle without cutting anything; and a tile that does cut must not
        // carry the tool changes of toolpaths that missed it entirely.
        if (t.motions == 0) {
            out.append(QVector<Op>());
            continue;
        }
        QVector<Op> kept = dropUncutBlocks(t.ops);
        // Retract last, so the closing rapid is not what keeps a dead block
        // alive (and is still emitted for the blocks we did keep).
        if (!kept.isEmpty() && t.havePos && t.pos.z < safeZ - 1e-9)
            kept.append(Op::rapid(t.pos.x, t.pos.y - k * tileHeight, safeZ));
        out.append(kept);
    }
    return out;
}

TiledExport exportTiled(Document &doc, const QString &outBase, double tileHeight)
{
    TiledExport r;
    r.tileHeight = tileHeight > 0 ? tileHeight
                                  : doc.params().value("tile_height", "508.0").toDouble();
    if (r.tileHeight <= 0)
        r.tileHeight = 508.0;
    const double safeZ = doc.params().value("retract", "2.54").toDouble();

    const GcodeResult g = exportGcode(doc);
    r.done = g.done;
    r.skipped = g.skipped;
    if (g.done.isEmpty()) {
        r.error = QStringLiteral("no exportable toolpaths");
        return r;
    }
    const QVector<QVector<Op>> tiles = tileOps(g.ops, r.tileHeight, safeZ);
    for (int k = 0; k < tiles.size(); ++k) {
        QVector<Op> ops;
        ops.append(Op::comment(QStringLiteral("tile %1/%2").arg(k + 1).arg(tiles.size())));
        ops.append(tiles.at(k));
        const QString gcode = GrblPost(true).generate(ops);
        const QString path = QStringLiteral("%1_tile%2.nc").arg(outBase).arg(k + 1);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            r.error = QStringLiteral("%1: %2").arg(path, f.errorString());
            return r;
        }
        f.write(gcode.toUtf8());
        r.files << path;
        r.gcode << gcode;
    }
    return r;
}

} // namespace c2d
