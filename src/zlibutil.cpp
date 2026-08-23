#include "zlibutil.h"
#include <zlib.h>

namespace c2d {

QByteArray zlibInflate(const QByteArray &in, int expectedSize)
{
    if (in.isEmpty())
        return {};

    z_stream zs{};
    if (inflateInit(&zs) != Z_OK)   // auto-detects the zlib header
        return {};

    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.constData()));
    zs.avail_in = static_cast<uInt>(in.size());

    QByteArray out;
    out.reserve(expectedSize > 0 ? expectedSize : in.size() * 4);

    char buf[16384];
    int ret;
    do {
        zs.next_out = reinterpret_cast<Bytef *>(buf);
        zs.avail_out = sizeof(buf);
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
            inflateEnd(&zs);
            return {};
        }
        out.append(buf, sizeof(buf) - zs.avail_out);
    } while (ret != Z_STREAM_END && zs.avail_out == 0);

    inflateEnd(&zs);
    return out;
}

QByteArray zlibDeflate(const QByteArray &in)
{
    z_stream zs{};
    if (deflateInit(&zs, 6) != Z_OK)
        return {};

    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.constData()));
    zs.avail_in = static_cast<uInt>(in.size());

    QByteArray out;
    char buf[16384];
    int ret;
    do {
        zs.next_out = reinterpret_cast<Bytef *>(buf);
        zs.avail_out = sizeof(buf);
        ret = deflate(&zs, Z_FINISH);
        out.append(buf, sizeof(buf) - zs.avail_out);
    } while (ret != Z_STREAM_END);

    deflateEnd(&zs);
    return out;
}

} // namespace c2d
