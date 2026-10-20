#include "toolstrip.h"
#include "c2ddocument.h"
#include "gcodeexport.h"
#include "post_grbl.h"
#include "tooldiagram.h"
#include "toollibrary.h"

#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QVBoxLayout>

#include <cmath>

namespace c2d {

// The sizes and kind of one embedded tool object (CC keys).
static void fillFromJson(LibraryTool &lt, const QJsonObject &tool)
{
    lt.diameter = tool.value(QStringLiteral("diameter")).toDouble();
    lt.angle = tool.value(QStringLiteral("angle")).toDouble();
    lt.flutes = int(tool.value(QStringLiteral("flutes")).toDouble(2));
    lt.cornerRadius = tool.value(QStringLiteral("corner_radius")).toDouble();
    lt.length = tool.value(QStringLiteral("length")).toDouble();
    lt.shank = lt.diameter;      // CC's tool object has no shank size
    const int type = int(tool.value(QStringLiteral("type")).toDouble());
    lt.kind = type == 1 ? QStringLiteral("ball")
            : (type == 2 || lt.angle > 0) ? QStringLiteral("vbit")
                                          : QStringLiteral("endmill");
    lt.name = tool.value(QStringLiteral("name")).toString();
    lt.model = tool.value(QStringLiteral("model")).toString();
}

LibraryTool toolFromToolpaths(const Document &doc, int number, QStringList *users)
{
    LibraryTool lt;
    lt.number = number;
    for (const Toolpath &t : doc.toolpaths()) {
        const QJsonObject tool = t.json.value(QStringLiteral("tool")).toObject();
        if (int(tool.value(QStringLiteral("number")).toDouble()) != number)
            continue;
        if (!t.json.value(QStringLiteral("enabled")).toBool(true))
            continue;
        if (users)
            *users << t.json.value(QStringLiteral("name")).toString();
        if (lt.diameter > 0)
            continue;
        fillFromJson(lt, tool);
    }
    if (lt.kind.isEmpty())
        lt.kind = QStringLiteral("endmill");
    return lt;
}

LibraryTool toolForChange(const Document &doc, const Op &change, QStringList *users)
{
    // The exporter's tool op carries the bit's shape: x diameter, y V angle,
    // z corner radius, feed kind + 1 (gcodeexport.cpp, toolOp).
    auto same = [](double a, double b) { return std::abs(a - b) < 1e-9; };
    LibraryTool lt;
    lt.number = change.ival;
    bool found = false;
    for (const Toolpath &t : doc.toolpaths()) {
        if (!t.json.value(QStringLiteral("enabled")).toBool(true))
            continue;
        for (const char *key : {"tool", "tool_pocket"}) {
            const QJsonObject tool = t.json.value(QLatin1String(key)).toObject();
            if (tool.isEmpty())
                continue;
            const ToolGeom g = toolGeomFromJson(tool);
            if (g.number != change.ival || int(g.kind) + 1 != int(change.feed)
                || !same(g.diameter, change.x) || !same(g.angle, change.y)
                || !same(g.cornerRadius, change.z))
                continue;
            if (users && !users->contains(t.json.value(QStringLiteral("name")).toString()))
                *users << t.json.value(QStringLiteral("name")).toString();
            if (!found)
                fillFromJson(lt, tool);
            found = true;
        }
    }
    if (!found) {
        lt.diameter = change.x;
        lt.angle = change.y;
        lt.cornerRadius = change.z;
        lt.shank = lt.diameter;
        const int kind = int(change.feed) - 1;
        lt.kind = kind == int(ToolGeom::Ball) ? QStringLiteral("ball")
                : kind == int(ToolGeom::VBit) ? QStringLiteral("vbit")
                                              : QStringLiteral("endmill");
    }
    if (lt.kind.isEmpty())
        lt.kind = QStringLiteral("endmill");
    return lt;
}

QVector<Op> toolChanges(const QVector<Op> &ops)
{
    // The post's rule (post_grbl.cpp, Op::Tool): another number, or the same
    // number with another bit.
    QVector<Op> out;
    int lastTool = -1;
    QString lastKey;
    for (const Op &op : ops) {
        if (op.kind != Op::Tool || (op.ival == lastTool && op.text == lastKey))
            continue;
        lastTool = op.ival;
        lastKey = op.text;
        out << op;
    }
    return out;
}

QPixmap toolPicture(const LibraryTool &t, int w, int h, double devicePixelRatio)
{
    QPixmap pm(QSize(w, h) * devicePixelRatio);
    pm.setDevicePixelRatio(devicePixelRatio);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    // Room above the drawing for the "shank" dimension text it puts there.
    paintToolDiagram(p, QRectF(0, 16, w, h - 16), t, true);
    return pm;
}

ToolStrip::ToolStrip(QWidget *parent)
    : QWidget(parent)
{
    auto *title = new QLabel(QStringLiteral("Tools"), this);
    title->setStyleSheet(QStringLiteral("font-weight: bold; padding: 6px 8px 2px 8px;"));
    m_list = new QListWidget(this);
    m_list->setIconSize(QSize(40, 56));
    m_list->setSpacing(4);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setToolTip(QStringLiteral("Tools in the order the program uses them; click one to jump there"));
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(title);
    lay->addWidget(m_list, 1);
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
        emit toolClicked(it->data(Qt::UserRole).toString());
    });
}

void ToolStrip::setJob(const Document &doc, const QVector<Op> &ops)
{
    m_list->clear();
    // One entry per bit, not per number: two bits may share a number.
    QVector<Op> order;
    QStringList keys;
    for (const Op &op : toolChanges(ops))
        if (!keys.contains(op.text)) {
            keys << op.text;
            order << op;
        }
    for (const Op &op : order) {
        const int n = op.ival;
        QStringList users;
        const LibraryTool lt = toolForChange(doc, op, &users);
        QString label = QStringLiteral("#%1\n\u00d8 %2").arg(n).arg(lt.diameter, 0, 'g', 4);
        if (lt.angle > 0)
            label += QStringLiteral(" %1\u00b0").arg(lt.angle);
        auto *it = new QListWidgetItem(toolKindIcon(lt), label, m_list);
        it->setData(Qt::UserRole, op.text);   // the bit, not the number
        it->setToolTip(QStringLiteral("T%1 %2\n%3").arg(n).arg(lt.name, users.join(QLatin1Char('\n'))));
    }
}

void ToolStrip::setCurrent(const QString &bit)
{
    for (int i = 0; i < m_list->count(); ++i)
        if (m_list->item(i)->data(Qt::UserRole).toString() == bit) {
            m_list->setCurrentRow(i);
            return;
        }
    m_list->setCurrentRow(-1);
}

} // namespace c2d
