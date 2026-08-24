#include "toolpathpanel.h"

#include <QJsonValue>
#include <QStringList>

namespace c2d {

// Roles on column 0 of parameter rows.
enum {
    RoleTpIndex = Qt::UserRole,      // int: index into Document::toolpaths()
    RoleKeyPath = Qt::UserRole + 1,  // QStringList: nested key path in json
    RoleType    = Qt::UserRole + 2,  // int(QJsonValue::Type) of the original value
};

// Structural top-level keys that must not be hand-edited.
static bool skippedKey(const QString &key)
{
    static const QStringList skip{
        QStringLiteral("uuid"), QStringLiteral("type"), QStringLiteral("version"),
        QStringLiteral("toolpath_group"), QStringLiteral("link_uuid"),
    };
    return skip.contains(key);
}

static QString valueText(const QJsonValue &v)
{
    switch (v.type()) {
    case QJsonValue::Bool:   return v.toBool() ? QStringLiteral("true")
                                               : QStringLiteral("false");
    case QJsonValue::Double: return QString::number(v.toDouble(), 'g', 15);
    default:                 return v.toString();
    }
}

// Write `v` at a nested key path, rebuilding the object chain on the way up
// (QJsonObject::value returns copies).
static void setNested(QJsonObject &root, const QStringList &path, const QJsonValue &v)
{
    if (path.size() == 1) {
        root.insert(path.first(), v);
        return;
    }
    QJsonObject child = root.value(path.first()).toObject();
    setNested(child, path.mid(1), v);
    root.insert(path.first(), child);
}

ToolpathPanel::ToolpathPanel(QWidget *parent)
    : QTreeWidget(parent)
{
    setColumnCount(2);
    setHeaderLabels({QStringLiteral("Toolpath / parameter"), QStringLiteral("Value")});
    setAlternatingRowColors(true);
    setUniformRowHeights(true);
    connect(this, &QTreeWidget::itemChanged, this, &ToolpathPanel::onItemChanged);
}

void ToolpathPanel::setDocument(Document *doc)
{
    m_doc = doc;
    reload();
}

void ToolpathPanel::reload()
{
    m_loading = true;
    clear();
    if (m_doc) {
        const QVector<Toolpath> &tps = m_doc->toolpaths();
        for (int i = 0; i < tps.size(); ++i) {
            const Toolpath &tp = tps.at(i);
            auto *top = new QTreeWidgetItem(this);
            top->setText(0, QStringLiteral("%1  [%2]")
                                .arg(tp.json.value("name").toString(), tp.type));
            top->setFirstColumnSpanned(true);
            addObjectRows(top, tp.json, i, {});
        }
        resizeColumnToContents(0);
    }
    m_loading = false;
}

void ToolpathPanel::addObjectRows(QTreeWidgetItem *parent, const QJsonObject &obj,
                                  int tpIndex, const QStringList &prefix)
{
    QStringList keys = obj.keys();
    keys.sort();
    for (const QString &key : keys) {
        if (prefix.isEmpty() && skippedKey(key))
            continue;
        const QJsonValue v = obj.value(key);
        QStringList path = prefix;
        path.append(key);

        if (v.isObject()) {
            auto *group = new QTreeWidgetItem(parent);
            group->setText(0, key);
            group->setFlags(group->flags() & ~Qt::ItemIsEditable);
            addObjectRows(group, v.toObject(), tpIndex, path);
        } else if (v.isBool() || v.isDouble() || v.isString()) {
            auto *row = new QTreeWidgetItem(parent);
            row->setText(0, key);
            row->setText(1, valueText(v));
            row->setData(0, RoleTpIndex, tpIndex);
            row->setData(0, RoleKeyPath, path);
            row->setData(0, RoleType, int(v.type()));
            row->setFlags(row->flags() | Qt::ItemIsEditable);
        }
        // Arrays (elements, toolpath_layers) are references, not parameters.
    }
}

void ToolpathPanel::onItemChanged(QTreeWidgetItem *item, int column)
{
    if (m_loading || !m_doc)
        return;
    const QVariant idxV = item->data(0, RoleTpIndex);
    if (idxV.isNull())
        return;
    if (column == 0) {
        // The key column is not editable content; restore the key name.
        m_loading = true;
        item->setText(0, item->data(0, RoleKeyPath).toStringList().last());
        m_loading = false;
        return;
    }

    const int tpIndex = idxV.toInt();
    const QStringList path = item->data(0, RoleKeyPath).toStringList();
    const auto type = QJsonValue::Type(item->data(0, RoleType).toInt());
    const QString text = item->text(1).trimmed();

    Toolpath &tp = m_doc->toolpaths()[tpIndex];
    QJsonValue newValue;
    bool valid = true;

    switch (type) {
    case QJsonValue::Bool:
        if (text.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0 ||
            text == QLatin1String("1"))
            newValue = true;
        else if (text.compare(QLatin1String("false"), Qt::CaseInsensitive) == 0 ||
                 text == QLatin1String("0"))
            newValue = false;
        else
            valid = false;
        break;
    case QJsonValue::Double: {
        bool ok = false;
        const double d = text.toDouble(&ok);
        if (ok)
            newValue = d;
        else
            valid = false;
        break;
    }
    default:
        newValue = text;   // strings verbatim — depths keep their convention
        break;
    }

    m_loading = true;   // suppress recursion from the setText below
    if (!valid) {
        // Revert the cell to the current document value.
        QJsonValue cur = tp.json.value(path.first());
        for (int i = 1; i < path.size(); ++i)
            cur = cur.toObject().value(path.at(i));
        item->setText(1, valueText(cur));
        m_loading = false;
        return;
    }
    setNested(tp.json, path, newValue);
    item->setText(1, valueText(newValue));   // normalize display
    if (path == QStringList{QStringLiteral("name")} && item->parent())
        item->parent()->setText(0, QStringLiteral("%1  [%2]").arg(text, tp.type));
    m_loading = false;
    emit edited();
}

} // namespace c2d
