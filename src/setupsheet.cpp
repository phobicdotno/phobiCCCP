#include "setupsheet.h"
#include "phobiversion.h"
#include "c2ddocument.h"
#include "gcodeexport.h"
#include "heightmodel.h"
#include "post_grbl.h"
#include "toolpathfactory.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHash>
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
    // The 3D model of the document being edited, as Export uses it. The
    // copies below must not ask for their own: the model store has none for
    // a copy, so it made a lazy one per sheet (loaded from the file on disk,
    // so a stale or missing model, and left in its registry for good). An
    // empty model is passed rather than none, which would fall back to that.
    const HeightModel *live = heightModelFor(doc);
    const HeightModel noModel;
    const HeightModel *relief = live ? live : &noModel;

    // The whole job once, for the totals and for each toolpath's time.
    Document all = doc;
    const GcodeResult whole = exportGcode(all, nullptr, relief);
    const JobStats total = computeStats(whole.ops);

    // Per-toolpath times come from that same program, not from exporting
    // each toolpath alone: rest depth makes a toolpath start deeper, or
    // leaves it out, because of the ones cut before it, and on its own it
    // never sees them. Each toolpath's ops open with a comment carrying its
    // name; pooled ring toolpaths come in several runs, which add up.
    QHash<QString, double> secByName;
    {
        QVector<Op> prefix;   // where the machine is when a run starts
        QString cur;
        int start = 0, rpm = 0;
        bool havePos = false;
        Op pos = Op::rapid(0, 0, 0);
        auto close = [&](int end) {
            if (cur.isEmpty() || end <= start)
                return;
            QVector<Op> run = prefix;
            run += whole.ops.mid(start, end - start);
            secByName[cur] += computeStats(run).timeSec - computeStats(prefix).timeSec;
        };
        for (int i = 0; i < whole.ops.size(); ++i) {
            const Op &op = whole.ops.at(i);
            if (op.kind == Op::Comment) {
                close(i);
                cur = op.text;
                start = i;
                // The machine's state carried in, so the run is charged for
                // its first travel and not for a spin-up that never happens.
                prefix.clear();
                if (rpm > 0)
                    prefix.append(Op::spindle(rpm));
                if (havePos)
                    prefix.append(pos);
            } else if (op.kind == Op::Rapid || op.kind == Op::Feed || op.kind == Op::Arc) {
                pos = Op::rapid(op.x, op.y, op.z);
                havePos = true;
            } else if (op.kind == Op::Spindle) {
                rpm = op.ival;
            } else if (op.kind == Op::Tool) {
                rpm = 0;
            }
        }
        close(whole.ops.size());
    }
    // A name two toolpaths share cannot be split by the comments; those fall
    // back to exporting the toolpath alone.
    QHash<QString, int> nameUses;
    for (const Toolpath &t : doc.toolpaths())
        if (t.json.value("enabled").toBool(true))
            ++nameUses[t.json.value("name").toString()];

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

    // Tools, in the order the program loads them: one row per bit, keyed
    // as the export keys its tool changes (number and shape), so two bits
    // that share a number are two rows, as they are two tool changes.
    struct ToolRow { QString key; int number; QJsonObject tool; QStringList used; };
    QVector<ToolRow> tools;
    auto rowFor = [&tools](const QString &key, int number, const QJsonObject &tool) -> ToolRow & {
        auto it = std::find_if(tools.begin(), tools.end(), [&key](const ToolRow &r) { return r.key == key; });
        if (it != tools.end())
            return *it;
        tools.append({key, number, tool, {}});
        return tools.last();
    };
    // The export's key for a bit (toolOp in gcodeexport.cpp): number, kind,
    // diameter, angle, corner radius.
    auto keyOf = [](const QJsonObject &tool) {
        const ToolGeom g = toolGeomFromJson(tool);
        return QStringLiteral("%1|%2|%3|%4|%5").arg(g.number).arg(int(g.kind))
            .arg(g.diameter, 0, 'g', 10).arg(g.angle, 0, 'g', 10).arg(g.cornerRadius, 0, 'g', 10);
    };
    // The tool object of an enabled toolpath named `name` that is the bit
    // with export key `key`.
    auto toolOfChange = [&doc, &keyOf](const QString &name, const QString &key) {
        for (const Toolpath &t : doc.toolpaths()) {
            if (!t.json.value("enabled").toBool(true) || t.json.value("name").toString() != name)
                continue;
            for (const char *k : {"tool", "tool_pocket"}) {
                const QJsonObject tool = t.json.value(QLatin1String(k)).toObject();
                if (!tool.isEmpty() && keyOf(tool) == key)
                    return tool;
            }
        }
        return QJsonObject();
    };
    QSet<QString> listed;   // toolpath names the program's tool rows cover
    {
        QString cur;
        int curNumber = 0;
        for (const Op &op : whole.ops) {
            if (op.kind == Op::Tool) {
                cur = op.text;
                curNumber = op.ival;
            } else if (op.kind == Op::Comment && !cur.isEmpty()) {
                const QJsonObject tool = toolOfChange(op.text, cur);
                if (tool.isEmpty())
                    continue;
                ToolRow &r = rowFor(cur, curNumber, tool);
                if (!r.used.contains(op.text))
                    r.used << op.text;
                listed.insert(op.text);
            }
        }
    }
    // Enabled toolpaths the program leaves out still name their bit.
    for (const Toolpath &t : doc.toolpaths()) {
        const QString name = t.json.value("name").toString();
        if (!t.json.value("enabled").toBool(true) || listed.contains(name))
            continue;
        const QJsonObject tool = t.json.value("tool").toObject();
        ToolRow &r = rowFor(keyOf(tool), int(numOr(tool.value("number"), 0)), tool);
        if (!r.used.contains(name))
            r.used << name;
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
        const QString name = j.value("name").toString();
        QString time;
        if (nameUses.value(name) <= 1) {
            if (whole.done.contains(name)) {
                time = minutes(secByName.value(name));
            } else {
                QStringList why;
                for (const QString &sk : whole.skipped)
                    if (sk.startsWith(name + QStringLiteral(" (")))
                        why << sk;
                time = QStringLiteral("<span class=\"skip\">%1</span>")
                           .arg(esc(why.isEmpty() ? name + QStringLiteral(" (not in the program)")
                                                  : why.join(QStringLiteral("; "))));
            }
        } else {
            Document one = doc;
            for (const Toolpath &o : doc.toolpaths()) {
                Toolpath c = o;
                c.json.insert(QStringLiteral("enabled"), o.uuid == t.uuid);
                one.replaceToolpath(c);
            }
            const GcodeResult r = exportGcode(one, nullptr, relief);
            time = r.done.isEmpty() ? QStringLiteral("<span class=\"skip\">%1</span>")
                                          .arg(esc(r.skipped.join(QStringLiteral("; "))))
                                    : minutes(computeStats(r.ops).timeSec);
        }
        const QJsonObject speeds = j.value("speeds").toObject();
        const QJsonObject tool = j.value("tool").toObject();
        const bool cutout = t.type == QLatin1String("cutout");
        const QString depth = cutout
            // break-through included: the sheet must match the program's min Z.
            ? mm(std::fabs(numOr(j.value("cut_depth"), 0)) + std::fabs(numOr(j.value("break_through"), 0)))
            : QStringLiteral("%1 → %2").arg(depthText(j.value("start_depth")),
                                             depthText(j.value("end_depth")));
        const QJsonValue sd = j.value(cutout ? "depth_per_pass" : "stepdown");
        const QJsonValue so = j.value("stepover");
        h += QStringLiteral("<tr><td class=\"n\">%1</td><td>%2</td><td>%3</td><td>T%4</td>"
                            "<td class=\"n\">%5</td><td class=\"n\">%6</td><td class=\"n\">%7</td>"
                            "<td class=\"n\">%8 / %9</td><td class=\"n\">%10</td><td class=\"n\">%11</td></tr>")
                 // One .arg for all: chained, a "%3" in a toolpath name (or a
                 // skip note) was filled in by the next call.
                 .arg(QString::number(row),
                      esc(j.value("name").toString()), esc(toolpathLabel(t.type)),
                      QString::number(int(numOr(tool.value("number"), 0))),
                      depth, sd.isUndefined() ? QStringLiteral("–") : mm(numOr(sd, 0)),
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
