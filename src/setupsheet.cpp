#include "setupsheet.h"
#include "c2ddocument.h"
#include "gcodeexport.h"
#include "post_grbl.h"
#include "toolpathfactory.h"

#include <QDateTime>
#include <QFileInfo>
#include <QJsonObject>
#include <QSet>
#include <cmath>

namespace c2d {

static QString esc(const QString &s) { return s.toHtmlEscaped(); }

static QString minutes(double sec)
{
    const int t = int(sec + 0.5);
    return QStringLiteral("%1:%2").arg(t / 60).arg(t % 60, 2, 10, QChar('0'));
}

static QString mm(double v, int dec = 2) { return QString::number(v, 'f', dec); }

// A depth as written in the file ("2.540" or "-2.540"), shown positive down.
static QString depthText(const QJsonValue &v)
{
    if (v.isUndefined() || v.isNull())
        return QStringLiteral("–");
    return mm(std::fabs(numOr(v, 0)));
}

static QString toolKind(const QJsonObject &tool)
{
    const int type = int(numOr(tool.value("type"), 0));
    const double angle = numOr(tool.value("angle"), 0);
    if (type == 2 || angle > 0)
        return QStringLiteral("V-bit %1°").arg(angle, 0, 'f', 0);
    if (type == 1)
        return QStringLiteral("Ball end mill");
    return QStringLiteral("Flat end mill");
}

QString setupSheetHtml(const Document &doc, const QString &title)
{
    // The whole job once for the totals, then each enabled toolpath on its
    // own for its time (pooled ring toolpaths interleave in the real
    // program, so the per-toolpath figures add up to a little more).
    Document all = doc;
    const GcodeResult whole = exportGcode(all);
    const JobStats total = computeStats(whole.ops);

    QString h;
    h += QStringLiteral("<!doctype html><html><head><meta charset=\"utf-8\">"
                        "<title>Setup sheet — %1</title><style>"
                        "body{font:13px/1.4 sans-serif;color:#111;margin:24px;}"
                        "h1{font-size:20px;margin:0 0 4px}h2{font-size:15px;margin:22px 0 6px}"
                        ".sub{color:#555;margin-bottom:12px}"
                        "table{border-collapse:collapse;width:100%}"
                        "th,td{border:1px solid #bbb;padding:4px 6px;text-align:left;vertical-align:top}"
                        "th{background:#eee}td.n{text-align:right;white-space:nowrap}"
                        ".skip{color:#a33}@media print{body{margin:8mm}}"
                        "</style></head><body>").arg(esc(title));
    h += QStringLiteral("<h1>%1</h1><div class=\"sub\">Setup sheet · %2 · phobiCCCP %3</div>")
             .arg(esc(title), QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")),
                  QStringLiteral(PHOBICCCP_VERSION));

    const auto &p = doc.params();
    h += QStringLiteral("<h2>Stock and job</h2><table>");
    h += QStringLiteral("<tr><th>Stock</th><td>%1 × %2 × %3 mm</td></tr>")
             .arg(mm(doc.boardWidth()), mm(doc.boardHeight()),
                  mm(p.value(QStringLiteral("thickness"), QStringLiteral("0")).toDouble()));
    if (!p.value(QStringLiteral("material")).isEmpty())
        h += QStringLiteral("<tr><th>Material</th><td>%1</td></tr>").arg(esc(p.value(QStringLiteral("material"))));
    h += QStringLiteral("<tr><th>Safe Z</th><td>%1 mm</td></tr>").arg(mm(documentSafeZ(doc)));
    if (total.hasBounds) {
        h += QStringLiteral("<tr><th>Extents</th><td>X %1 … %2, Y %3 … %4, Z %5 … %6 mm</td></tr>")
                 .arg(mm(total.minX, 1), mm(total.maxX, 1), mm(total.minY, 1), mm(total.maxY, 1),
                      mm(total.minZ, 1), mm(total.maxZ, 1));
        h += QStringLiteral("<tr><th>Estimated time</th><td>%1 min (cut %2 mm, rapid %3 mm)</td></tr>")
                 .arg(minutes(total.timeSec), mm(total.cutLen, 0), mm(total.rapidLen, 0));
    }
    h += QStringLiteral("</table>");

    // Tools, in the order the program loads them.
    struct ToolRow { int number; QJsonObject tool; QStringList used; };
    QVector<ToolRow> tools;
    for (const Toolpath &t : doc.toolpaths()) {
        if (!t.json.value("enabled").toBool(true))
            continue;
        const QJsonObject tool = t.json.value("tool").toObject();
        const int n = int(numOr(tool.value("number"), 0));
        auto it = std::find_if(tools.begin(), tools.end(), [n](const ToolRow &r) { return r.number == n; });
        if (it == tools.end()) {
            tools.append({n, tool, {}});
            it = tools.end() - 1;
        }
        it->used << t.json.value("name").toString();
    }
    h += QStringLiteral("<h2>Tools</h2><table><tr><th>#</th><th>Tool</th><th>Type</th>"
                        "<th>Diameter</th><th>Used by</th></tr>");
    for (const ToolRow &r : tools) {
        const QString name = r.tool.value("name").toString();
        h += QStringLiteral("<tr><td class=\"n\">T%1</td><td>%2</td><td>%3</td>"
                            "<td class=\"n\">%4 mm</td><td>%5</td></tr>")
                 .arg(r.number)
                 .arg(esc(name.isEmpty() ? r.tool.value("model").toString() : name),
                      esc(toolKind(r.tool)), mm(numOr(r.tool.value("diameter"), 0), 3),
                      esc(r.used.join(QStringLiteral(", "))));
    }
    h += QStringLiteral("</table>");

    h += QStringLiteral("<h2>Toolpaths</h2><table><tr><th>#</th><th>Toolpath</th><th>Type</th>"
                        "<th>Tool</th><th>Depth</th><th>Stepdown</th><th>Stepover</th>"
                        "<th>Feed / plunge</th><th>RPM</th><th>Time</th></tr>");
    int row = 0;
    for (const Toolpath &t : doc.toolpaths()) {
        const QJsonObject &j = t.json;
        if (!j.value("enabled").toBool(true))
            continue;
        ++row;
        Document one = doc;
        for (const Toolpath &o : doc.toolpaths()) {
            Toolpath c = o;
            c.json.insert(QStringLiteral("enabled"), o.uuid == t.uuid);
            one.replaceToolpath(c);
        }
        const GcodeResult r = exportGcode(one);
        const JobStats st = computeStats(r.ops);
        const QJsonObject speeds = j.value("speeds").toObject();
        const QJsonObject tool = j.value("tool").toObject();
        const bool cutout = t.type == QLatin1String("cutout");
        const QString depth = cutout
            ? mm(numOr(j.value("cut_depth"), 0))
            : QStringLiteral("%1 → %2").arg(depthText(j.value("start_depth")),
                                             depthText(j.value("end_depth")));
        const QJsonValue sd = j.value(cutout ? "depth_per_pass" : "stepdown");
        const QJsonValue so = j.value("stepover");
        QString time = r.done.isEmpty() ? QStringLiteral("<span class=\"skip\">%1</span>")
                                              .arg(esc(r.skipped.join(QStringLiteral("; "))))
                                        : minutes(st.timeSec);
        h += QStringLiteral("<tr><td class=\"n\">%1</td><td>%2</td><td>%3</td><td>T%4</td>"
                            "<td class=\"n\">%5</td><td class=\"n\">%6</td><td class=\"n\">%7</td>"
                            "<td class=\"n\">%8 / %9</td><td class=\"n\">%10</td><td class=\"n\">%11</td></tr>")
                 .arg(row)
                 .arg(esc(j.value("name").toString()), esc(toolpathLabel(t.type)))
                 .arg(int(numOr(tool.value("number"), 0)))
                 .arg(depth, sd.isUndefined() ? QStringLiteral("–") : mm(numOr(sd, 0)),
                      so.isUndefined() ? QStringLiteral("–") : mm(numOr(so, 0)),
                      mm(numOr(speeds.value("feedrate"), 0), 0), mm(numOr(speeds.value("plungerate"), 0), 0),
                      mm(numOr(speeds.value("rpm"), 0), 0), time);
    }
    h += QStringLiteral("</table>");
    if (!whole.skipped.isEmpty()) {
        h += QStringLiteral("<h2>Not in the program</h2><ul>");
        for (const QString &s : whole.skipped)
            h += QStringLiteral("<li class=\"skip\">%1</li>").arg(esc(s));
        h += QStringLiteral("</ul>");
    }
    h += QStringLiteral("</body></html>\n");
    return h;
}

} // namespace c2d
