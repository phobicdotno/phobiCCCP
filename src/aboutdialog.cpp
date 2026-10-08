#include "aboutdialog.h"

#include <QDialogButtonBox>
#include <QFile>
#include <QIcon>
#include <QLabel>
#include <QRegularExpression>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace c2d {

QVector<ReleaseSummary> summarizeChangelog(const QString &markdown)
{
    static const QRegularExpression head(
        QStringLiteral(R"(^## v(\S+)\s*(?:\(build (\d+)\))?\s*[—-]*\s*(\S*))"));
    static const QRegularExpression section(QStringLiteral(R"(^\*\*(.+?)\*\*\s*$)"));
    QVector<ReleaseSummary> out;
    QString firstBullet;   // fallback summary for a release without sections
    auto closeRelease = [&] {
        if (!out.isEmpty() && out.last().areas.isEmpty() && !firstBullet.isEmpty()) {
            QString t = firstBullet;
            t.remove(QRegularExpression(QStringLiteral("[*`]")));
            if (t.size() > 70)
                t = t.left(t.lastIndexOf(QLatin1Char(' '), 68)) + QStringLiteral(" …");
            out.last().areas.append(t);
        }
        firstBullet.clear();
    };
    for (const QString &line : markdown.split(QLatin1Char('\n'))) {
        if (line.startsWith(QLatin1String("- ")) && firstBullet.isEmpty() && !out.isEmpty())
            firstBullet = line.mid(2).trimmed();
        const QRegularExpressionMatch h = head.match(line);
        if (h.hasMatch()) {
            closeRelease();
            ReleaseSummary r;
            r.version = h.captured(1);
            r.build = h.captured(2).toInt();
            r.date = h.captured(3);
            out.append(r);
            continue;
        }
        const QRegularExpressionMatch s = section.match(line.trimmed());
        if (s.hasMatch() && !out.isEmpty()) {
            const QString title = s.captured(1).trimmed();
            if (title.compare(QLatin1String("Testing"), Qt::CaseInsensitive) != 0
                && !out.last().areas.contains(title))
                out.last().areas.append(title);
        }
    }
    closeRelease();
    return out;
}

AboutDialog::AboutDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("About phobiCCCP"));
    resize(520, 560);

    QString md;
    QFile f(QStringLiteral(":/data/CHANGELOG.md"));
    if (f.open(QIODevice::ReadOnly))
        md = QString::fromUtf8(f.readAll());
    const QVector<ReleaseSummary> rel = summarizeChangelog(md);

    auto *lay = new QVBoxLayout(this);

    auto *logo = new QLabel(this);
    logo->setAlignment(Qt::AlignCenter);
    QIcon icon(QStringLiteral(":/data/logo.svg"));
    if (icon.isNull())
        icon = QIcon::fromTheme(QStringLiteral("phobicccp"));
    logo->setPixmap(icon.pixmap(96, 96));
    lay->addWidget(logo);

    QString build;
    for (const ReleaseSummary &r : rel)
        if (r.version == QLatin1String(PHOBICCCP_VERSION) && r.build > 0)
            build = QStringLiteral(" (build %1)").arg(r.build);
    auto *title = new QLabel(QStringLiteral(
        "<div align=center><span style='font-size:20pt; font-weight:bold'>phobiCCCP</span><br>"
        "<i>Phobic Carbide Create Clone Project</i><br>"
        "Version %1%2<br><br>"
        "Carbide Create .c2d editor, G-code generator and GRBL sender for Linux<br>"
        "<a href='https://github.com/phobicdotno/phobiCCCP'>github.com/phobicdotno/phobiCCCP</a>"
        "</div>").arg(QLatin1String(PHOBICCCP_VERSION), build), this);
    title->setTextFormat(Qt::RichText);
    title->setOpenExternalLinks(true);
    title->setWordWrap(true);
    lay->addWidget(title);

    QString html = QStringLiteral("<h3>What's new</h3><table cellspacing=0 cellpadding=3>");
    for (const ReleaseSummary &r : rel) {
        html += QStringLiteral("<tr><td valign=top style='white-space:nowrap'><b>v%1</b></td>"
                               "<td valign=top style='white-space:nowrap; color:#8a919c'>%2</td>"
                               "<td valign=top>%3</td></tr>")
                    .arg(r.version.toHtmlEscaped(), r.date.toHtmlEscaped(),
                         r.areas.isEmpty() ? QStringLiteral("—")
                                           : r.areas.join(QStringLiteral(" · ")).toHtmlEscaped());
    }
    html += QStringLiteral("</table>");
    auto *log = new QTextBrowser(this);
    log->setOpenExternalLinks(true);
    log->setHtml(html);
    lay->addWidget(log, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);
}

} // namespace c2d
