// QR code encoder and its vectors: version and size, finder, timing and
// alignment patterns, format and version bits (BCH), the first data bits,
// and the merged outlines cover exactly the dark modules.
#include "../src/qrcode.h"
#include "../src/vectorops.h"
#include <QCoreApplication>
#include <QRectF>
#include <cmath>
#include <cstdio>
using namespace c2d;
static int fails = 0;
static void check(bool ok, const char *what) { std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++fails; }

static bool finderOk(const QrCode &c, int x0, int y0)
{
    for (int dy = -1; dy <= 7; ++dy)
        for (int dx = -1; dx <= 7; ++dx) {
            const int x = x0 + dx, y = y0 + dy;
            if (x < 0 || y < 0 || x >= c.size || y >= c.size)
                continue;
            const int d = std::max(std::abs(dx - 3), std::abs(dy - 3));
            const bool want = d != 2 && d != 4;   // 4 = the light separator
            if (c.dark(x, y) != want)
                return false;
        }
    return true;
}

static bool timingOk(const QrCode &c)
{
    for (int i = 8; i < c.size - 8; ++i)
        if (c.dark(i, 6) != (i % 2 == 0) || c.dark(6, i) != (i % 2 == 0))
            return false;
    return true;
}

static int bchRem(long v, int len, long poly, int polyDeg)
{
    for (int i = len - 1; i >= polyDeg; --i)
        if ((v >> i) & 1)
            v ^= poly << (i - polyDeg);
    return int(v);
}

// Format bits from both copies; -1 if they differ.
static int readFormat(const QrCode &c)
{
    int a = 0, b = 0;
    auto put = [](int &v, int i, bool d) { if (d) v |= 1 << i; };
    for (int i = 0; i <= 5; ++i) put(a, i, c.dark(8, i));
    put(a, 6, c.dark(8, 7)); put(a, 7, c.dark(8, 8)); put(a, 8, c.dark(7, 8));
    for (int i = 9; i < 15; ++i) put(a, i, c.dark(14 - i, 8));
    for (int i = 0; i < 8; ++i) put(b, i, c.dark(c.size - 1 - i, 8));
    for (int i = 8; i < 15; ++i) put(b, i, c.dark(8, c.size - 15 + i));
    return a == b ? a : -1;
}

static bool maskBit(int m, int x, int y)
{
    switch (m) {
    case 0: return (x + y) % 2 == 0;
    case 1: return y % 2 == 0;
    case 2: return x % 3 == 0;
    case 3: return (x + y) % 3 == 0;
    case 4: return (x / 3 + y / 2) % 2 == 0;
    case 5: return x * y % 2 + x * y % 3 == 0;
    case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
    default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

static void structure(const QrCode &c, int ver, QrEcc ecc, const char *name)
{
    char buf[160];
    auto say = [&](bool ok, const char *what) { std::snprintf(buf, sizeof buf, "%s: %s", name, what); check(ok, buf); };
    say(c.version == ver, "version");
    say(c.size == 17 + 4 * ver && c.modules.size() == c.size * c.size, "size");
    say(finderOk(c, 0, 0) && finderOk(c, c.size - 7, 0) && finderOk(c, 0, c.size - 7),
        "finder patterns at three corners");
    say(!finderOk(c, c.size - 7, c.size - 7), "no finder at the bottom right");
    say(timingOk(c), "timing patterns alternate");
    say(c.dark(8, c.size - 8), "dark module");
    const int f = readFormat(c);
    say(f >= 0, "both format copies agree");
    const int raw = f ^ 0x5412;
    say(bchRem(raw, 15, 0x537, 10) == 0, "format BCH remainder is zero");
    const int eccBits[4] = {1, 0, 3, 2};
    say((raw >> 13) == eccBits[int(ecc)], "format names the error correction level");
    say(((raw >> 10) & 7) == c.mask && f == qrFormatBits(ecc, c.mask), "format names the mask");
    if (ver >= 2) {
        const int p = c.size - 7;   // last alignment pattern centre
        bool ok = c.dark(p, p);
        for (int d = -2; d <= 2; ++d)
            ok = ok && c.dark(p + d, p - 2) && c.dark(p + d, p + 2) && c.dark(p - 2, p + d) && c.dark(p + 2, p + d);
        for (int d = -1; d <= 1; ++d)
            ok = ok && !c.dark(p + d, p - 1) && !c.dark(p + d, p + 1) && !c.dark(p - 1, p + d) && !c.dark(p + 1, p + d);
        say(ok, "alignment pattern bottom right");
    }
    if (ver >= 7) {
        long v1 = 0, v2 = 0;
        for (int i = 0; i < 18; ++i) {
            const int a = c.size - 11 + i % 3, b = i / 3;
            if (c.dark(a, b)) v1 |= 1L << i;
            if (c.dark(b, a)) v2 |= 1L << i;
        }
        say(v1 == v2 && (v1 >> 12) == ver && bchRem(v1, 18, 0x1F25, 12) == 0, "version info BCH");
    }
    // Mode indicator 0100 (byte) in the bottom-right 2x2, unmasked.
    const int s = c.size;
    auto bit = [&](int x, int y) { return c.dark(x, y) != maskBit(c.mask, x, y); };
    say(!bit(s - 1, s - 1) && bit(s - 2, s - 1) && !bit(s - 1, s - 2) && !bit(s - 2, s - 2),
        "byte mode indicator");
}

static double netArea(const QVector<QPolygonF> &rings)
{
    double a = 0;
    for (const QPolygonF &r : rings)
        a += vec::ringArea(r);
    return std::fabs(a);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const QrCode hello = qrEncode("HELLO", QrEcc::M);
    structure(hello, 1, QrEcc::M, "HELLO");
    const QrCode url = qrEncode("https://example.com/some/long/path?q=1", QrEcc::M);
    structure(url, 3, QrEcc::M, "URL");
    const QrCode big = qrEncode(QByteArray(150, 'a'), QrEcc::L);
    structure(big, 7, QrEcc::L, "150 bytes at L");
    const QrCode hq = qrEncode("https://example.com/", QrEcc::H);  // 20 bytes: v2-H holds 14, v3-H 24
    structure(hq, 3, QrEcc::H, "URL at H");

    check(qrEncode(QByteArray(qrMaxBytes(QrEcc::L), 'x'), QrEcc::L).version == 10, "longest text fits version 10");
    const QrCode tooLong = qrEncode(QByteArray(qrMaxBytes(QrEcc::L) + 1, 'x'), QrEcc::L);
    check(tooLong.version == 0 && !tooLong.error.isEmpty(), "too long: error, nothing drawn");
    check(qrEncode(QByteArray(), QrEcc::M).version == 0, "empty text: nothing");
    // UTF-8 text carries a 12-bit ECI header: 14 bytes fit v1-M as ASCII, not as UTF-8.
    check(qrEncode(QByteArray(14, 'e'), QrEcc::M).version == 1, "14 ASCII bytes in v1-M");
    check(qrEncode(QString(7, QChar(0xe9)).toUtf8(), QrEcc::M).version == 2, "UTF-8 adds the ECI header");
    check(qrEncode(QByteArray(qrMaxBytes(QrEcc::L), char(0xc3)), QrEcc::L).error.contains(QLatin1String("bytes")),
          "UTF-8 too long: limit told in bytes");

    // Vectors: 50 mm with 2 quiet modules, centred at (100, 80).
    for (const QrCode *c : {&hello, &url, &big}) {
        const QPointF centre(100, 80);
        const QrDrawing merged = qrDrawing(*c, 50, 2, true, centre);
        const double m = 50.0 / (c->size + 4);
        const double want = c->darkCount() * m * m;
        check(std::fabs(merged.moduleMm - m) < 1e-12, "module size");
        check(std::fabs(netArea(merged.rings) - want) < want * 1e-4, "merged area = dark modules x module area");
        check(merged.rings.size() < c->darkCount() / 3, "merged into far fewer shapes");
        const QrDrawing loose = qrDrawing(*c, 50, 2, false, centre);
        check(loose.rings.size() == c->darkCount(), "unmerged: one square per dark module");
        check(std::fabs(netArea(loose.rings) - want) < want * 1e-9, "unmerged area");
        const QRectF b = merged.border.boundingRect();
        check(std::fabs(b.center().x() - 100) < 1e-9 && std::fabs(b.center().y() - 80) < 1e-9
              && std::fabs(b.width() - 50) < 1e-9, "border is 50 mm, centred");
        QRectF inked;
        for (const QPolygonF &r : merged.rings)
            inked = inked.united(r.boundingRect());
        check(std::fabs(inked.width() - c->size * m) < 1e-3 && std::fabs(inked.left() - (75 + 2 * m)) < 1e-3,
              "dark area inside the quiet zone");
        // Top-left finder: Y up, so row 0 is at the top.
        check(std::fabs(inked.top() + inked.height() - (105 - 2 * m)) < 1e-3, "row 0 at the top");
    }
    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
