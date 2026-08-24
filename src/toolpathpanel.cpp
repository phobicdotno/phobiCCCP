#include "toolpathpanel.h"
#include "canvas.h"
#include "c2ddocument.h"

#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QListWidget>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace c2d {

// Flattened, editable scalar entries of a toolpath payload: top-level scalars
// plus one nested level for `speeds` and `tool`. Bookkeeping keys that would
// break references when edited are excluded.
static QStringList flatKeys(const QJsonObject &j)
{
    static const QStringList skip = {QStringLiteral("uuid"), QStringLiteral("type"),
                                     QStringLiteral("version"), QStringLiteral("toolpath_group")};
    QStringList keys;
    for (auto it = j.constBegin(); it != j.constEnd(); ++it) {
        if (skip.contains(it.key()))
            continue;
        if (it.value().isObject()
            && (it.key() == QLatin1String("speeds") || it.key() == QLatin1String("tool"))) {
            const QJsonObject sub = it.value().toObject();
            for (auto s = sub.constBegin(); s != sub.constEnd(); ++s)
                if (!s.value().isObject() && !s.value().isArray())
                    keys << it.key() + QLatin1Char('.') + s.key();
        } else if (!it.value().isObject() && !it.value().isArray()) {
            keys << it.key();
        }
    }
    keys.sort();
    return keys;
}

static QJsonValue flatGet(const QJsonObject &j, const QString &key)
{
    const int dot = key.indexOf(QLatin1Char('.'));
    if (dot < 0)
        return j.value(key);
    return j.value(key.left(dot)).toObject().value(key.mid(dot + 1));
}

static void flatSet(QJsonObject &j, const QString &key, const QJsonValue &v)
{
    const int dot = key.indexOf(QLatin1Char('.'));
    if (dot < 0) {
        j.insert(key, v);
        return;
    }
    QJsonObject sub = j.value(key.left(dot)).toObject();
    sub.insert(key.mid(dot + 1), v);
    j.insert(key.left(dot), sub);
}

static QString valueToText(const QJsonValue &v)
{
    if (v.isBool())
        return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (v.isDouble())
        return QString::number(v.toDouble());
    return v.toString();
}

// Parse the edited text back into the ORIGINAL value's JSON type — CC is
// strict about this (depths are strings, rates are numbers, flags are bools).
static QJsonValue textToValue(const QString &text, const QJsonValue &original)
{
    if (original.isBool())
        return QJsonValue(text.trimmed().compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0
                          || text.trimmed() == QStringLiteral("1"));
    if (original.isDouble()) {
        bool ok = false;
        const double d = text.trimmed().toDouble(&ok);
        return ok ? QJsonValue(d) : original;
    }
    return QJsonValue(text);
}

ToolpathPanel::ToolpathPanel(Canvas *canvas, QWidget *parent)
    : QWidget(parent), m_canvas(canvas)
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->setSpacing(6);

    m_list = new QListWidget(this);
    m_list->setMaximumHeight(110);
    lay->addWidget(m_list);
    connect(m_list, &QListWidget::currentRowChanged, this, &ToolpathPanel::showToolpath);

    m_table = new QTableWidget(0, 2, this);
    m_table->setHorizontalHeaderLabels({QStringLiteral("parameter"), QStringLiteral("value")});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->setAlternatingRowColors(true);
    lay->addWidget(m_table);
    connect(m_table, &QTableWidget::cellChanged, this, &ToolpathPanel::onCellEdited);

    // Replace the toolpath's vector list with the canvas selection.
    auto *assign = new QPushButton(QStringLiteral("Assign selected vectors"), this);
    assign->setToolTip(QStringLiteral(
        "Set this toolpath's vectors to the elements selected on the canvas"));
    lay->addWidget(assign);
    connect(assign, &QPushButton::clicked, this, [this] {
        if (!m_doc || m_uuid.isEmpty())
            return;
        Toolpath *t = m_doc->toolpathByUuid(m_uuid);
        const QStringList ids = m_canvas->selectedElementIds();
        if (!t || ids.isEmpty())
            return;
        QJsonArray arr;
        for (const QString &id : ids) {
            QJsonObject o;
            o.insert(QStringLiteral("uuid"), id);
            arr.append(o);
        }
        QJsonObject j = t->json;
        j.insert(QStringLiteral("elements"), arr);
        m_canvas->editToolpath(m_uuid, j);
    });
}

void ToolpathPanel::setDocument(Document *doc)
{
    m_doc = doc;
    m_uuid.clear();
    refresh();
}

void ToolpathPanel::refresh()
{
    m_loading = true;
    const QString keep = m_uuid;
    m_list->clear();
    int keepRow = -1;
    if (m_doc) {
        int i = 0;
        for (const Toolpath &t : m_doc->toolpaths()) {
            m_list->addItem(QStringLiteral("%1  [%2] · %3 vectors")
                                .arg(t.json.value("name").toString(), t.type)
                                .arg(t.json.value("elements").toArray().size()));
            if (t.uuid == keep)
                keepRow = i;
            ++i;
        }
    }
    m_loading = false;
    if (m_list->count() > 0)
        m_list->setCurrentRow(keepRow >= 0 ? keepRow : 0);
    else
        showToolpath(-1);
}

void ToolpathPanel::showToolpath(int row)
{
    if (m_loading)
        return;
    m_loading = true;
    m_table->setRowCount(0);
    m_uuid.clear();
    if (m_doc && row >= 0 && row < m_doc->toolpaths().size()) {
        const Toolpath &t = m_doc->toolpaths().at(row);
        m_uuid = t.uuid;
        const QStringList keys = flatKeys(t.json);
        m_table->setRowCount(keys.size());
        for (int i = 0; i < keys.size(); ++i) {
            auto *k = new QTableWidgetItem(keys.at(i));
            k->setFlags(k->flags() & ~Qt::ItemIsEditable);
            m_table->setItem(i, 0, k);
            m_table->setItem(i, 1,
                             new QTableWidgetItem(valueToText(flatGet(t.json, keys.at(i)))));
        }
    }
    m_loading = false;
}

void ToolpathPanel::onCellEdited(int row, int col)
{
    if (m_loading || col != 1 || !m_doc || m_uuid.isEmpty())
        return;
    Toolpath *t = m_doc->toolpathByUuid(m_uuid);
    if (!t)
        return;
    const QString key = m_table->item(row, 0)->text();
    const QJsonValue original = flatGet(t->json, key);
    const QJsonValue v = textToValue(m_table->item(row, col)->text(), original);
    if (v == original) {
        // Normalize the cell back (e.g. rejected number) without pushing an edit.
        m_loading = true;
        m_table->item(row, col)->setText(valueToText(original));
        m_loading = false;
        return;
    }
    QJsonObject j = t->json;
    flatSet(j, key, v);
    m_canvas->editToolpath(m_uuid, j);
}

} // namespace c2d
