#pragma once
// Help -> About: logo, version and build, and a short changelog distilled
// from the CHANGELOG.md compiled into the binary.

#include <QDialog>
#include <QString>
#include <QVector>

namespace c2d {

struct ReleaseSummary {
    QString version;    // "0.4.90"
    int build = 0;      // 30
    QString date;       // "2026-10-05"
    QStringList areas;  // the release's section titles, e.g. "Sketch tools"
    QString summary;    // the "> one line" under the heading, if there is one
};

// One entry per "## vX (build N) — date" heading, newest first, with its
// **Section** titles (Testing left out: users do not need it), and the
// one-line "> summary" written under the heading. About shows the summary
// when there is one.
QVector<ReleaseSummary> summarizeChangelog(const QString &markdown);

class AboutDialog : public QDialog
{
public:
    explicit AboutDialog(QWidget *parent = nullptr);
};

} // namespace c2d
