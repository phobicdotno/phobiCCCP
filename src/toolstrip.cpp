#include "toolstrip.h"
#include "c2ddocument.h"
#include "post_grbl.h"
#include "tooldiagram.h"
#include "toollibrary.h"

#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QVBoxLayout>

namespace c2d {

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
    if (lt.kind.isEmpty())
        lt.kind = QStringLiteral("endmill");
    return lt;
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
        emit toolClicked(it->data(Qt::UserRole).toInt());
    });
}

void ToolStrip::setJob(const Document &doc, const QVector<Op> &ops)
{
    m_list->clear();
    QList<int> order;
    for (const Op &op : ops)
        if (op.kind == Op::Tool && !order.contains(op.ival))
            order << op.ival;
    for (int n : order) {
        QStringList users;
        const LibraryTool lt = toolFromToolpaths(doc, n, &users);
        QString label = QStringLiteral("#%1\n\u00d8 %2").arg(n).arg(lt.diameter, 0, 'g', 4);
        if (lt.angle > 0)
            label += QStringLiteral(" %1\u00b0").arg(lt.angle);
        auto *it = new QListWidgetItem(toolKindIcon(lt), label, m_list);
        it->setData(Qt::UserRole, n);
        it->setToolTip(QStringLiteral("T%1 %2\n%3").arg(n).arg(lt.name, users.join(QLatin1Char('\n'))));
    }
}

void ToolStrip::setCurrent(int number)
{
    for (int i = 0; i < m_list->count(); ++i)
        if (m_list->item(i)->data(Qt::UserRole).toInt() == number) {
            m_list->setCurrentRow(i);
            return;
        }
    m_list->setCurrentRow(-1);
}

} // namespace c2d
