#pragma once
#include <QByteArray>

// Carbide Create stores items.data / compressed sqlar blobs as *plain* zlib
// streams (header 0x78 0x01 / 0x78 0x9C). This is NOT Qt's qCompress format,
// which prepends a 4-byte big-endian uncompressed-size prefix - so qUncompress()
// fails on these blobs. Use these helpers (thin wrappers over zlib) instead.
namespace c2d {

// Largest blob we will inflate out of a document. Well past any real relief
// or preview; a stream that keeps expanding past it is a zip bomb, not data.
constexpr qsizetype kMaxInflateBytes = 256 * 1024 * 1024;

// Inflate a raw zlib blob. `expectedSize` is the `sz` column value (the
// uncompressed byte length); pass 0 if unknown and the buffer grows as needed.
// Returns an empty array if the stream is malformed, or if it would expand
// past `kMaxInflateBytes` - `sz` is a number in the file, not a promise.
QByteArray zlibInflate(const QByteArray &in, int expectedSize = 0);

// Deflate to a raw zlib stream CC can read back (level 6, default window).
QByteArray zlibDeflate(const QByteArray &in);

} // namespace c2d
