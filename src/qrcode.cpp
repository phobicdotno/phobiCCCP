/*
 * QR code encoder, byte mode, versions 1 to 10.
 *
 * The encoding steps follow Project Nayuki's QR Code generator library:
 * Copyright (c) Project Nayuki. (MIT License)
 * https://www.nayuki.io/page/qr-code-generator-library
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 * - The above copyright notice and this permission notice shall be included in
 *   all copies or substantial portions of the Software.
 * - The Software is provided "as is", without warranty of any kind, express or
 *   implied, including but not limited to the warranties of merchantability,
 *   fitness for a particular purpose and noninfringement. In no event shall the
 *   authors or copyright holders be liable for any claim, damages or other
 *   liability, whether in an action of contract, tort or otherwise, arising from,
 *   out of or in connection with the Software or the use or other dealings in the
 *   Software.
 */
#include "qrcode.h"
#include "vectorops.h"
#include <QPainterPath>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace c2d {

namespace {

constexpr int kMaxVersion = 10;

// [ecc L,M,Q,H][version], index 0 unused.
const int kEccPerBlock[4][kMaxVersion + 1] = {
    {-1, 7, 10, 15, 20, 26, 18, 20, 24, 30, 18},
    {-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26},
    {-1, 13, 22, 18, 26, 18, 24, 18, 22, 20, 24},
    {-1, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28},
};
const int kNumBlocks[4][kMaxVersion + 1] = {
    {-1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4},
    {-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5},
    {-1, 1, 1, 2, 2, 4, 4, 6, 6, 8, 8},
    {-1, 1, 1, 2, 4, 4, 4, 5, 6, 8, 8},
};
// Format-info value of each level (L=01, M=00, Q=11, H=10).
const int kEccFormat[4] = {1, 0, 3, 2};

int rawDataModules(int ver)
{
    int result = (16 * ver + 128) * ver + 64;
    if (ver >= 2) {
        const int numAlign = ver / 7 + 2;
        result -= (25 * numAlign - 10) * numAlign - 55;
        if (ver >= 7)
            result -= 36;
    }
    return result;
}

int dataCodewords(int ver, QrEcc ecc)
{
    const int e = int(ecc);
    return rawDataModules(ver) / 8 - kEccPerBlock[e][ver] * kNumBlocks[e][ver];
}

std::vector<int> alignmentPositions(int ver)
{
    if (ver == 1)
        return {};
    const int numAlign = ver / 7 + 2;
    const int size = ver * 4 + 17;
    const int step = (ver * 4 + numAlign * 2 + 1) / (numAlign * 2 - 2) * 2;
    std::vector<int> result(numAlign);
    result[0] = 6;
    for (int i = numAlign - 1, pos = size - 7; i >= 1; --i, pos -= step)
        result[i] = pos;
    return result;
}

// GF(2^8) with the polynomial 0x11D.
uint8_t gfMul(uint8_t x, uint8_t y)
{
    int z = 0;
    for (int i = 7; i >= 0; --i) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return uint8_t(z);
}

std::vector<uint8_t> rsDivisor(int degree)
{
    std::vector<uint8_t> result(degree, 0);
    result[degree - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; ++i) {
        for (int j = 0; j < degree; ++j) {
            result[j] = gfMul(result[j], root);
            if (j + 1 < degree)
                result[j] ^= result[j + 1];
        }
        root = gfMul(root, 0x02);
    }
    return result;
}

std::vector<uint8_t> rsRemainder(const std::vector<uint8_t> &data, const std::vector<uint8_t> &div)
{
    std::vector<uint8_t> result(div.size(), 0);
    for (uint8_t b : data) {
        const uint8_t factor = b ^ result[0];
        result.erase(result.begin());
        result.push_back(0);
        for (size_t i = 0; i < result.size(); ++i)
            result[i] ^= gfMul(div[i], factor);
    }
    return result;
}

struct Builder {
    int ver, size;
    QrEcc ecc;
    std::vector<bool> mod, fn;

    Builder(int v, QrEcc e) : ver(v), size(v * 4 + 17), ecc(e),
        mod(size_t(size * size), false), fn(size_t(size * size), false) {}

    bool get(int x, int y) const { return mod[size_t(y * size + x)]; }
    void setFn(int x, int y, bool dark)
    {
        mod[size_t(y * size + x)] = dark;
        fn[size_t(y * size + x)] = true;
    }

    void drawFinder(int x, int y)
    {
        for (int dy = -4; dy <= 4; ++dy)
            for (int dx = -4; dx <= 4; ++dx) {
                const int dist = std::max(std::abs(dx), std::abs(dy));
                const int xx = x + dx, yy = y + dy;
                if (xx >= 0 && xx < size && yy >= 0 && yy < size)
                    setFn(xx, yy, dist != 2 && dist != 4);
            }
    }
    void drawAlignment(int x, int y)
    {
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx)
                setFn(x + dx, y + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
    }
    void drawFormat(int mask)
    {
        const int bits = qrFormatBits(ecc, mask);
        auto bit = [bits](int i) { return ((bits >> i) & 1) != 0; };
        for (int i = 0; i <= 5; ++i)
            setFn(8, i, bit(i));
        setFn(8, 7, bit(6));
        setFn(8, 8, bit(7));
        setFn(7, 8, bit(8));
        for (int i = 9; i < 15; ++i)
            setFn(14 - i, 8, bit(i));
        for (int i = 0; i < 8; ++i)
            setFn(size - 1 - i, 8, bit(i));
        for (int i = 8; i < 15; ++i)
            setFn(8, size - 15 + i, bit(i));
        setFn(8, size - 8, true);   // the always-dark module
    }
    void drawVersion()
    {
        if (ver < 7)
            return;
        int rem = ver;
        for (int i = 0; i < 12; ++i)
            rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
        const long bits = long(ver) << 12 | rem;
        for (int i = 0; i < 18; ++i) {
            const bool b = ((bits >> i) & 1) != 0;
            const int a = size - 11 + i % 3, c = i / 3;
            setFn(a, c, b);
            setFn(c, a, b);
        }
    }
    void drawFunctionPatterns()
    {
        for (int i = 0; i < size; ++i) {
            setFn(6, i, i % 2 == 0);
            setFn(i, 6, i % 2 == 0);
        }
        drawFinder(3, 3);
        drawFinder(size - 4, 3);
        drawFinder(3, size - 4);
        const std::vector<int> pos = alignmentPositions(ver);
        const int n = int(pos.size());
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j)
                if (!((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0)))
                    drawAlignment(pos[size_t(i)], pos[size_t(j)]);
        drawFormat(0);   // reserved; redrawn once the mask is chosen
        drawVersion();
    }
    void drawCodewords(const std::vector<uint8_t> &data)
    {
        size_t i = 0;
        for (int right = size - 1; right >= 1; right -= 2) {
            if (right == 6)
                right = 5;
            for (int vert = 0; vert < size; ++vert)
                for (int j = 0; j < 2; ++j) {
                    const int x = right - j;
                    const bool upward = ((right + 1) & 2) == 0;
                    const int y = upward ? size - 1 - vert : vert;
                    if (!fn[size_t(y * size + x)] && i < data.size() * 8) {
                        mod[size_t(y * size + x)] = ((data[i >> 3] >> (7 - int(i & 7))) & 1) != 0;
                        ++i;
                    }
                }
        }
    }
    void applyMask(int m)
    {
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                bool inv = false;
                switch (m) {
                case 0: inv = (x + y) % 2 == 0; break;
                case 1: inv = y % 2 == 0; break;
                case 2: inv = x % 3 == 0; break;
                case 3: inv = (x + y) % 3 == 0; break;
                case 4: inv = (x / 3 + y / 2) % 2 == 0; break;
                case 5: inv = x * y % 2 + x * y % 3 == 0; break;
                case 6: inv = (x * y % 2 + x * y % 3) % 2 == 0; break;
                default: inv = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
                }
                const size_t k = size_t(y * size + x);
                if (inv && !fn[k])
                    mod[k] = !mod[k];
            }
    }
    // ISO penalty rules N1..N4. Out-of-bounds counts as light for the
    // finder-like pattern search (the quiet zone).
    long penalty() const
    {
        long result = 0;
        auto line = [&](bool rows, int k, int i) { return rows ? get(i, k) : get(k, i); };
        static const bool pat[7] = {true, false, true, true, true, false, true};
        for (int pass = 0; pass < 2; ++pass) {
            const bool rows = pass == 0;
            for (int k = 0; k < size; ++k) {
                int run = 1;
                for (int i = 1; i <= size; ++i) {
                    if (i < size && line(rows, k, i) == line(rows, k, i - 1)) {
                        ++run;
                        continue;
                    }
                    if (run >= 5)
                        result += 3 + (run - 5);
                    run = 1;
                }
                auto at = [&](int i) { return i >= 0 && i < size && line(rows, k, i); };
                for (int i = 0; i + 7 <= size; ++i) {
                    bool match = true;
                    for (int j = 0; j < 7 && match; ++j)
                        match = at(i + j) == pat[j];
                    if (!match)
                        continue;
                    bool lightBefore = true, lightAfter = true;
                    for (int j = 1; j <= 4; ++j) {
                        lightBefore = lightBefore && !at(i - j);
                        lightAfter = lightAfter && !at(i + 6 + j);
                    }
                    if (lightBefore)
                        result += 40;
                    if (lightAfter)
                        result += 40;
                }
            }
        }
        for (int y = 0; y + 1 < size; ++y)
            for (int x = 0; x + 1 < size; ++x) {
                const bool c = get(x, y);
                if (c == get(x + 1, y) && c == get(x, y + 1) && c == get(x + 1, y + 1))
                    result += 3;
            }
        long dark = 0;
        for (bool b : mod)
            dark += b ? 1 : 0;
        const long total = long(size) * size;
        const long k = (std::labs(dark * 20 - total * 10) + total - 1) / total - 1;
        result += k * 10;
        return result;
    }
};

std::vector<uint8_t> addEccAndInterleave(const std::vector<uint8_t> &data, int ver, QrEcc ecc)
{
    const int e = int(ecc);
    const int numBlocks = kNumBlocks[e][ver];
    const int blockEccLen = kEccPerBlock[e][ver];
    const int rawCodewords = rawDataModules(ver) / 8;
    const int numShortBlocks = numBlocks - rawCodewords % numBlocks;
    const int shortBlockLen = rawCodewords / numBlocks;

    std::vector<std::vector<uint8_t>> blocks;
    const std::vector<uint8_t> div = rsDivisor(blockEccLen);
    size_t k = 0;
    for (int i = 0; i < numBlocks; ++i) {
        const size_t len = size_t(shortBlockLen - blockEccLen + (i < numShortBlocks ? 0 : 1));
        std::vector<uint8_t> dat(data.begin() + long(k), data.begin() + long(k + len));
        k += len;
        const std::vector<uint8_t> ecc = rsRemainder(dat, div);
        if (i < numShortBlocks)
            dat.push_back(0);
        dat.insert(dat.end(), ecc.begin(), ecc.end());
        blocks.push_back(std::move(dat));
    }
    std::vector<uint8_t> result;
    for (size_t i = 0; i < blocks[0].size(); ++i)
        for (size_t j = 0; j < blocks.size(); ++j)
            if (i != size_t(shortBlockLen - blockEccLen) || int(j) >= numShortBlocks)
                result.push_back(blocks[j][i]);
    return result;
}

} // namespace

int QrCode::darkCount() const
{
    return int(std::count(modules.begin(), modules.end(), true));
}

int qrFormatBits(QrEcc ecc, int mask)
{
    const int data = kEccFormat[int(ecc)] << 3 | mask;
    int rem = data;
    for (int i = 0; i < 10; ++i)
        rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    return (data << 10 | rem) ^ 0x5412;
}

int qrMaxBytes(QrEcc ecc)
{
    return (dataCodewords(kMaxVersion, ecc) * 8 - 4 - 16) / 8;
}

QrCode qrEncode(const QByteArray &data, QrEcc ecc)
{
    QrCode out;
    out.ecc = ecc;
    if (data.isEmpty()) {
        out.error = QStringLiteral("Type the text or web address to put in the code.");
        return out;
    }
    // Non-ASCII text is UTF-8: say so with an ECI header (designator 26), or
    // readers that keep to the ISO-8859-1 default show it garbled.
    const bool utf8 = std::any_of(data.begin(), data.end(), [](char c) { return uint8_t(c) >= 0x80; });
    const int eciBits = utf8 ? 12 : 0;
    int ver = 0;
    for (int v = 1; v <= kMaxVersion && !ver; ++v) {
        const int ccBits = v <= 9 ? 8 : 16;
        if (eciBits + 4 + ccBits + 8 * data.size() <= dataCodewords(v, ecc) * 8)
            ver = v;
    }
    if (!ver) {
        out.error = QStringLiteral("Too long: at this error correction the code holds up to %1 "
                                   "bytes (you have %2; letters like \u00e9 take more than one). "
                                   "Use a shorter text or level L.")
                        .arg((dataCodewords(kMaxVersion, ecc) * 8 - 4 - 16 - eciBits) / 8).arg(data.size());
        return out;
    }

    // Bit stream: byte mode, count, bytes, terminator, pad.
    std::vector<bool> bits;
    auto append = [&bits](unsigned val, int len) {
        for (int i = len - 1; i >= 0; --i)
            bits.push_back(((val >> i) & 1) != 0);
    };
    if (utf8) {
        append(0x7, 4);
        append(26, 8);
    }
    append(0x4, 4);
    append(unsigned(data.size()), ver <= 9 ? 8 : 16);
    for (char c : data)
        append(uint8_t(c), 8);
    const size_t capacity = size_t(dataCodewords(ver, ecc)) * 8;
    append(0, int(std::min<size_t>(4, capacity - bits.size())));
    append(0, int((8 - bits.size() % 8) % 8));
    for (uint8_t pad = 0xEC; bits.size() < capacity; pad ^= 0xEC ^ 0x11)
        append(pad, 8);
    std::vector<uint8_t> codewords(bits.size() / 8, 0);
    for (size_t i = 0; i < bits.size(); ++i)
        codewords[i >> 3] |= uint8_t((bits[i] ? 1 : 0) << (7 - int(i & 7)));

    Builder b(ver, ecc);
    b.drawFunctionPatterns();
    b.drawCodewords(addEccAndInterleave(codewords, ver, ecc));
    int best = 0;
    long bestPenalty = -1;
    for (int m = 0; m < 8; ++m) {
        b.applyMask(m);
        b.drawFormat(m);
        const long p = b.penalty();
        if (bestPenalty < 0 || p < bestPenalty) {
            best = m;
            bestPenalty = p;
        }
        b.applyMask(m);   // XOR undoes it
    }
    b.applyMask(best);
    b.drawFormat(best);

    out.version = ver;
    out.size = b.size;
    out.mask = best;
    out.modules.reserve(int(b.mod.size()));
    for (bool m : b.mod)
        out.modules << m;
    return out;
}

QrDrawing qrDrawing(const QrCode &code, double sizeMm, int quiet, bool merge, QPointF centre)
{
    QrDrawing d;
    if (code.size <= 0 || !(sizeMm > 0))
        return d;
    quiet = std::max(0, quiet);
    const double m = sizeMm / (code.size + 2 * quiet);
    d.moduleMm = m;
    const double left = centre.x() - sizeMm / 2, top = centre.y() + sizeMm / 2;
    d.border = QPolygonF({QPointF(left, top - sizeMm), QPointF(left + sizeMm, top - sizeMm),
                          QPointF(left + sizeMm, top), QPointF(left, top)});
    auto square = [&](int x, int y) {
        const double x0 = left + (x + quiet) * m, y1 = top - (y + quiet) * m;
        return QPolygonF({QPointF(x0, y1 - m), QPointF(x0 + m, y1 - m),
                          QPointF(x0 + m, y1), QPointF(x0, y1)});
    };
    if (!merge) {
        for (int y = 0; y < code.size; ++y)
            for (int x = 0; x < code.size; ++x)
                if (code.dark(x, y))
                    d.rings << square(x, y);
        return d;
    }
    // Horizontal runs of dark modules first (fewer shapes for the union),
    // then one union of them all.
    QVector<QPainterPath> runs;
    for (int y = 0; y < code.size; ++y)
        for (int x = 0; x < code.size;) {
            if (!code.dark(x, y)) {
                ++x;
                continue;
            }
            int x1 = x;
            while (x1 < code.size && code.dark(x1, y))
                ++x1;
            QPolygonF r = square(x, y);
            r[1].setX(r[1].x() + (x1 - x - 1) * m);
            r[2].setX(r[2].x() + (x1 - x - 1) * m);
            QPainterPath p;
            p.addPolygon(r);
            p.closeSubpath();
            runs << p;
            x = x1;
        }
    d.rings = vec::booleanRings(runs, {}, vec::BoolOp::Union, std::min(0.005, m / 100));
    return d;
}

} // namespace c2d
