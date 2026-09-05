#include "toolpathpanel.h"
#include "canvas.h"
#include "c2ddocument.h"
#include "toollibrary.h"
#include "toollibrarydialog.h"

#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QUuid>
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

    // Tool library: pick a cutter + material, write tool/speeds/stepdown.
    auto *toolBtn = new QPushButton(QStringLiteral("Tool library…"), this);
    toolBtn->setToolTip(QStringLiteral(
        "Choose a Carbide 3D cutter and a material: writes the tool definition,\n"
        "feed / plunge / rpm and depth per pass + stepover into this toolpath."));
    lay->addWidget(toolBtn);
    connect(toolBtn, &QPushButton::clicked, this, [this] { pickTool(); });

    // V-carve inlay: generate the mirrored male from the selected female.
    auto *inlay = new QPushButton(QStringLiteral("Create inlay male"), this);
    inlay->setToolTip(QStringLiteral(
        "From the selected advanced v-carve (the female): place a mirrored copy\n"
        "of its design beside it inside a border, and add a v-carve of the\n"
        "inverse region with the depths shifted by the glue gap. Carve it in a\n"
        "second piece, flip it over into the female, glue, then plane flush."));
    lay->addWidget(inlay);
    connect(inlay, &QPushButton::clicked, this, [this] { createInlayMale(); });
}

void ToolpathPanel::pickTool()
{
    if (!m_doc || m_uuid.isEmpty())
        return;
    Toolpath *t = m_doc->toolpathByUuid(m_uuid);
    if (!t)
        return;
    ToolLibrary lib;
    QString err;
    if (!lib.loadDefault(&err)) {
        QMessageBox::warning(this, QStringLiteral("Tool library"), err);
        return;
    }
    QString mat = lib.materialIdForCC(m_doc->params().value("material"));
    if (mat.isEmpty())
        mat = QStringLiteral("softwood");
    const int cur = int(t->json.value("tool").toObject().value("number").toDouble());
    ToolLibraryDialog dlg(lib, mat, cur, this);
    if (dlg.exec() != QDialog::Accepted || !dlg.selectedTool())
        return;
    const ToolFeeds f = dlg.selectedFeeds();
    if (!f.valid()) {
        QMessageBox::information(this, QStringLiteral("Tool library"),
            QStringLiteral("%1 has no feeds and speeds for %2.")
                .arg(dlg.selectedTool()->name, lib.materialName(dlg.selectedMaterialId())));
        return;
    }
    QJsonObject j = t->json;
    ToolLibrary::applyToToolpath(j, *dlg.selectedTool(), f);
    m_canvas->editToolpath(m_uuid, j);
}

void ToolpathPanel::createInlayMale()
{
    if (!m_doc || m_uuid.isEmpty())
        return;
    Toolpath *t = m_doc->toolpathByUuid(m_uuid);
    if (!t || t->type != QLatin1String("advanced_vcarve_toolpath")) {
        QMessageBox::information(this, QStringLiteral("Inlay male"),
            QStringLiteral("Select an advanced v-carve toolpath first — the male "
                           "is generated from the female's design."));
        return;
    }
    QVector<const Element *> els;
    for (const QJsonValue &v : t->json.value("elements").toArray())
        if (const Element *e = m_doc->elementById(v.toObject().value("uuid").toString()))
            els.append(e);
    if (els.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Inlay male"),
                                 QStringLiteral("The toolpath references no vectors."));
        return;
    }
    bool ok = false;
    const double gap = QInputDialog::getDouble(
        this, QStringLiteral("Inlay male"),
        QStringLiteral("Glue gap (mm): both male depths are shifted down by\n"
                       "this much, so the assembled male sits proud by the gap\n"
                       "(glue space below, planed flush after glue-up)"),
        0.5, 0.0, 5.0, 2, &ok);
    if (!ok)
        return;
    const double margin = QInputDialog::getDouble(
        this, QStringLiteral("Inlay male"),
        QStringLiteral("Border margin around the mirrored design (mm)"),
        10.0, 2.0, 100.0, 1, &ok);
    if (!ok)
        return;

    QRectF bb;
    for (const Element *e : els)
        bb = bb.united(e->painterPath.boundingRect());
    const double cx = bb.center().x();
    const double dx = bb.width() + 2 * margin + 20.0;   // male sits to the right
    const QJsonObject layer = m_doc->defaultLayer();

    QVector<Element> newEls;
    QJsonArray refs;
    const Element border = Element::makeRectangle(
        QPointF(cx + dx, bb.center().y()),
        bb.width() + 2 * margin, bb.height() + 2 * margin, layer);
    newEls.append(border);
    refs.append(QJsonObject{{QStringLiteral("uuid"), border.id}});
    // Mirror every subpath about the design's vertical centerline, then shift
    // beside the original. Border + mirrored design under even-odd fill = the
    // inverse region: the male carves the field, leaving the design standing.
    for (const Element *e : els) {
        const QList<QPolygonF> polys = e->painterPath.toSubpathPolygons();
        for (const QPolygonF &p : polys) {
            const bool closed = p.size() > 3 && p.first() == p.last();
            const int n = closed ? int(p.size()) - 1 : int(p.size());
            if (n < 2)
                continue;
            QVector<QPointF> pts;
            pts.reserve(n);
            for (int i = n - 1; i >= 0; --i)   // reversed: mirroring flips winding
                pts.append(QPointF(2 * cx - p.at(i).x() + dx, p.at(i).y()));
            const Element pe = Element::makePath(pts, closed, layer);
            newEls.append(pe);
            refs.append(QJsonObject{{QStringLiteral("uuid"), pe.id}});
        }
    }

    Toolpath male = *t;
    QJsonObject j = male.json;
    j.insert(QStringLiteral("uuid"), QUuid::createUuid().toString());
    j.insert(QStringLiteral("name"),
             j.value("name").toString() + QStringLiteral(" (inlay male)"));
    j.insert(QStringLiteral("elements"), refs);
    auto num = [](const QJsonValue &v) {
        return v.isString() ? v.toString().toDouble() : v.toDouble();
    };
    // Preserve the file's depth sign convention and value type (string/number).
    const double sign = num(j.value("end_depth")) < 0 ? -1.0 : 1.0;
    auto setDepth = [&](const char *key, double absVal) {
        const QJsonValue orig = j.value(QLatin1String(key));
        if (orig.isString())
            j.insert(QLatin1String(key), QString::number(sign * absVal, 'f', 3));
        else
            j.insert(QLatin1String(key), sign * absVal);
    };
    setDepth("start_depth", qAbs(num(j.value("start_depth"))) + gap);
    setDepth("end_depth", qAbs(num(j.value("end_depth"))) + gap);
    male.json = j;
    male.uuid = j.value("uuid").toString();
    m_canvas->insertGenerated(newEls, male);
    QMessageBox::information(this, QStringLiteral("Inlay male"),
        QStringLiteral("Generated \"%1\": mirrored design in a %2 mm border, "
                       "depths shifted %3 mm.\n\nCarve it in the second piece, "
                       "flip that piece face-down into the female, glue, then "
                       "plane the ~%3 mm proud surface flush.")
            .arg(j.value("name").toString())
            .arg(margin, 0, 'f', 1)
            .arg(gap, 0, 'f', 2));
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
