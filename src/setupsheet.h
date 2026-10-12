#pragma once
#include <QString>

// Fusion 360's Setup Sheet: a printable HTML page for the machine — stock,
// material and the job's totals, the tools in the order they are loaded,
// then every enabled toolpath with its tool, depths, feeds and estimated
// time (the post's feed-rate estimate, run per toolpath; rapids assumed
// 5000 mm/min, no acceleration).
namespace c2d {

class Document;

QString setupSheetHtml(const Document &doc, const QString &title);

} // namespace c2d
