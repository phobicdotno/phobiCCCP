#pragma once
#include <QByteArray>

// Carbide Create stores items.data / compressed sqlar blobs as *plain* zlib
// streams (header 0x78 0x01 / 0x78 0x9C). This is NOT Qt's qCompress format,
// which prepends a 4-byte big-endian uncompressed-size prefix - so qUncompress()
// fails on these blobs. Use these helpers (thin wrappers over zlib) instead.
namespace c2d {

// Inflate a raw zlib blob. `expectedSize` is the `sz` column value (the
// uncompressed byte length); pass 0 if unknown and the buffer grows as needed.
QByteArray zlibInflate(const QByteArray &in, int expectedSize = 0);

// Deflate to a raw zlib stream CC can read back (level 6, default window).
QByteArray zlibDeflate(const QByteArray &in);

} // namespace c2d
