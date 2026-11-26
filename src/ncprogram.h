#pragma once
#include "post_grbl.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace c2d {

// A G-code file read for the Machine page: its lines tidied for the same
// path a design's program takes (comments and line numbers off, a T/M6
// tool change turned into the post's `M0 ;T<n>` pause, a "(Tool n)" line
// after it to name the next row of the job list), and its moves as ops for
// the travel check and the time estimate. A file that uses what GRBL or
// this sender cannot run safely is refused with the line and the reason.
struct NcProgram {
    QStringList lines;
    QVector<Op> ops;
    QString error;     // empty when the file can run
    int errorLine = 0; // 1-based line of the file the error is about
    bool ok() const { return error.isEmpty(); }
};

NcProgram readNcProgram(const QString &text);

} // namespace c2d
