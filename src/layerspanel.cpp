#include "layerspanel.h"
#include "canvas.h"
#include "layers.h"
#include "shopdialog.h"
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace c2d {

namespace {
constexpr int kRowH = 52;   // touch screen: a finger-sized row
const char *kHideTip =
    "Show or hide this layer's shapes on the screen. Hiding is for the view only: "
    "toolpaths that use hidden shapes still cut them.";
const char *kLockTip =
    "A locked layer's shapes are drawn but can't be selected, moved or edited.";
} // namespace

LayersPanel::LayersPanel(Canvas *canvas, QWidget *parent)
    : QWidget(parent), m_canvas(canvas)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(6, 6, 6, 6);
    v->setSpacing(6);
    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setStyleSheet(QStringLiteral(
        "QListWidget{background:#14171d;border:none;}"
        "QListWidget::item{border-bottom:1px solid #2a2f3a;}"
        "QListWidget::item:selected{background:#2b3240;}"));
    v->addWidget(m_list, 1);

    auto mk = [this](const QString &text, const QString &tip) {
        auto *b = new QPushButton(text, this);
        b->setMinimumHeight(44);
        b->setToolTip(tip);
        return b;
    };
    m_add = mk(tr("Add layer"), tr("Add a new, empty layer"));
    m_rename = mk(tr("Rename"), tr("Give the picked layer a new name"));
    m_delete = mk(tr("Delete"), tr("Delete the picked layer; only an empty one can go"));
    m_move = mk(tr("Move selection here"),
                tr("Put the shapes selected on the drawing onto the picked layer"));
    auto *row1 = new QHBoxLayout;
    row1->addWidget(m_add);
    row1->addWidget(m_rename);
    row1->addWidget(m_delete);
    v->addLayout(row1);
    v->addWidget(m_move);

    connect(m_add, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(
            this, tr("Add layer"), tr("Layer name:"), QLineEdit::Normal,
            tr("Layer %1").arg(m_list->count() + 1), &ok).trimmed();
        if (ok && !name.isEmpty())
            addLayer(name);
    });
    connect(m_rename, &QPushButton::clicked, this, [this] {
        const QString key = currentKey();
        if (key.isEmpty())
            return;
        bool ok = false;
        const QString name = QInputDialog::getText(
            this, tr("Rename layer"), tr("Layer name:"), QLineEdit::Normal,
            layerObject(key).value(QStringLiteral("name")).toString(), &ok).trimmed();
        if (ok && !name.isEmpty())
            renameLayer(key, name);
    });
    connect(m_delete, &QPushButton::clicked, this, [this] {
        const QString key = currentKey();
        if (!key.isEmpty() && !deleteLayer(key))
            shopTell(this, tr("Layers"), tr("This layer still has shapes"),
                     tr("Move its shapes to another layer, or delete them, first."));
    });
    connect(m_move, &QPushButton::clicked, this, [this] {
        moveSelectionTo(currentKey());
    });
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
        selectLayer(it->data(Qt::UserRole).toString());
    });
    connect(m_list, &QListWidget::currentRowChanged, this, [this] { updateButtons(); });
    connect(m_canvas, &Canvas::documentChanged, this, &LayersPanel::reload);
    connect(m_canvas, &Canvas::selectionChangedIds, this, [this] { updateButtons(); });
    reload();
}

QString LayersPanel::currentKey() const
{
    const QListWidgetItem *it = m_list->currentItem();
    return it ? it->data(Qt::UserRole).toString() : QString();
}

void LayersPanel::setCurrentKey(const QString &key)
{
    for (int i = 0; i < m_list->count(); ++i)
        if (m_list->item(i)->data(Qt::UserRole).toString() == key) {
            m_list->setCurrentRow(i);
            return;
        }
}

QJsonObject LayersPanel::layerObject(const QString &key) const
{
    if (const Document *d = m_canvas->document())
        for (const layers::Layer &l : layers::list(*d, m_extra))
            if (l.key == key)
                return l.obj;
    return {};
}

void LayersPanel::documentReplaced()
{
    m_extra.clear();
    reload();
}

void LayersPanel::reload()
{
    if (m_busy)
        return;
    const QString keep = currentKey();
    m_list->clear();
    const Document *d = m_canvas->document();
    if (d) {
        const QVector<layers::Layer> all = layers::list(*d, m_extra);
        // An added layer keeps its definition (as its shapes have it now) so
        // undoing the move that filled it brings it back empty, not gone.
        for (QJsonObject &o : m_extra)
            for (const layers::Layer &l : all)
                if (l.key == layers::keyOf(o) && l.count > 0)
                    o = l.obj;
        for (const layers::Layer &l : all) {
            auto *it = new QListWidgetItem(m_list);
            it->setData(Qt::UserRole, l.key);
            it->setSizeHint(QSize(0, kRowH));
            buildRow(m_list->count() - 1);
        }
    }
    setCurrentKey(keep);
    if (!m_list->currentItem() && m_list->count() > 0)
        m_list->setCurrentRow(0);
    updateButtons();
}

void LayersPanel::buildRow(int row)
{
    QListWidgetItem *it = m_list->item(row);
    const QString key = it->data(Qt::UserRole).toString();
    const Document *d = m_canvas->document();
    layers::Layer l;
    for (const layers::Layer &x : layers::list(*d, m_extra))
        if (x.key == key)
            l = x;
    auto *w = new QWidget(m_list);
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(4, 2, 4, 2);
    h->setSpacing(6);
    auto *eye = new QPushButton(l.visible() ? tr("Shown") : tr("Hidden"), w);
    eye->setCheckable(true);
    eye->setChecked(l.visible());
    eye->setFixedSize(76, 44);
    eye->setToolTip(tr(kHideTip));
    eye->setObjectName(QStringLiteral("layerEye"));
    auto *lock = new QPushButton(l.locked() ? tr("Locked") : tr("Lock"), w);
    lock->setCheckable(true);
    lock->setChecked(l.locked());
    lock->setFixedSize(76, 44);
    lock->setToolTip(tr(kLockTip));
    lock->setObjectName(QStringLiteral("layerLock"));
    auto *name = new QLabel(tr("%1  (%2)").arg(l.name()).arg(l.count), w);
    if (!l.visible())
        name->setStyleSheet(QStringLiteral("color:#7a808c;"));
    h->addWidget(eye);
    h->addWidget(lock);
    h->addWidget(name, 1);
    connect(eye, &QPushButton::clicked, this, [this, key](bool on) { setVisible(key, on); });
    connect(lock, &QPushButton::clicked, this, [this, key](bool on) { setLocked(key, on); });
    m_list->setItemWidget(it, w);
}

void LayersPanel::updateButtons()
{
    const QString key = currentKey();
    int count = 0;
    bool locked = false;
    if (const Document *d = m_canvas->document())
        for (const layers::Layer &l : layers::list(*d, m_extra))
            if (l.key == key) {
                count = l.count;
                locked = l.locked();
            }
    m_rename->setEnabled(!key.isEmpty());
    m_delete->setEnabled(!key.isEmpty() && count == 0);
    m_move->setEnabled(!key.isEmpty() && !locked && !m_canvas->selectedElementIds().isEmpty());
}

void LayersPanel::addLayer(const QString &name)
{
    const QJsonObject l = layers::makeLayer(name);
    m_extra.append(l);
    reload();
    setCurrentKey(layers::keyOf(l));
}

void LayersPanel::renameLayer(const QString &key, const QString &name)
{
    for (QJsonObject &o : m_extra)
        if (layers::keyOf(o) == key)
            o.insert(QStringLiteral("name"), name);
    QString newKey = key;
    if (Document *d = m_canvas->document()) {
        const QVector<Element> after = layers::edited(*d, key, [&](QJsonObject &l) {
            l.insert(QStringLiteral("name"), name);
            newKey = layers::keyOf(l);   // a blank-uuid layer is known by its name
        });
        m_canvas->editLayers(after, tr("rename layer"));
    }
    reload();
    setCurrentKey(newKey);
}

void LayersPanel::setVisible(const QString &key, bool on)
{
    for (QJsonObject &o : m_extra)
        if (layers::keyOf(o) == key)
            o.insert(QStringLiteral("visible"), on);
    if (Document *d = m_canvas->document())
        m_canvas->editLayers(layers::edited(*d, key, [on](QJsonObject &l) {
                                 l.insert(QStringLiteral("visible"), on);
                             }), on ? tr("show layer") : tr("hide layer"));
    reload();
}

void LayersPanel::setLocked(const QString &key, bool on)
{
    for (QJsonObject &o : m_extra)
        if (layers::keyOf(o) == key)
            o.insert(QStringLiteral("locked"), on);
    if (Document *d = m_canvas->document())
        m_canvas->editLayers(layers::edited(*d, key, [on](QJsonObject &l) {
                                 l.insert(QStringLiteral("locked"), on);
                             }), on ? tr("lock layer") : tr("unlock layer"));
    reload();
}

bool LayersPanel::deleteLayer(const QString &key)
{
    if (const Document *d = m_canvas->document())
        if (!layers::idsOn(*d, key).isEmpty())
            return false;
    for (int i = 0; i < m_extra.size(); ++i)
        if (layers::keyOf(m_extra.at(i)) == key) {
            m_extra.removeAt(i);
            break;
        }
    reload();
    return true;
}

void LayersPanel::moveSelectionTo(const QString &key)
{
    Document *d = m_canvas->document();
    if (!d || key.isEmpty())
        return;
    const QJsonObject target = layerObject(key);
    if (target.isEmpty())
        return;
    const QStringList ids = m_canvas->selectedElementIds();
    m_canvas->editLayers(layers::movedTo(*d, ids, target), tr("move to layer"));
    // On a hidden layer the shapes can't stay selected.
    if (target.value(QStringLiteral("visible")).toBool(true))
        m_canvas->selectElements(ids);
    reload();
    setCurrentKey(key);
}

void LayersPanel::selectLayer(const QString &key)
{
    if (const Document *d = m_canvas->document())
        m_canvas->selectElements(layers::idsOn(*d, key));   // hidden / locked: none
}

} // namespace c2d
