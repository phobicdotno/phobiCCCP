#include "toollibrarydialog.h"
#include "stockstamp.h"
#include "stocksync.h"
#include "tooldiagram.h"

#include <algorithm>

#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

namespace c2d {

// A diameter for display: at most three decimals, no trailing zeros
// (1/16" = 1.5875 shows as 1.587, 6.0 as 6).
static QString mmText(double mm)
{
    QString s = QString::number(mm, 'f', 3);
    while (s.endsWith(QLatin1Char('0')))
        s.chop(1);
    if (s.endsWith(QLatin1Char('.')))
        s.chop(1);
    return s;
}

ToolLibraryDialog::ToolLibraryDialog(const ToolLibrary &lib, const QString &materialId,
                                     int currentToolNumber, QWidget *parent)
    : QDialog(parent), m_lib(lib)
{
    setWindowTitle(QStringLiteral("Tool library — speeds and feeds"));
    resize(940, 520);
    auto *lay = new QVBoxLayout(this);

    auto *top = new QHBoxLayout;
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(QStringLiteral("Search (number, name, 1/4, ball, V…)"));
    m_search->setClearButtonEnabled(true);
    top->addWidget(m_search, 2);
    m_material = new QComboBox(this);
    for (const LibraryMaterial &m : lib.materials())
        m_material->addItem(m.name, m.id);
    const int mi = m_material->findData(materialId);
    m_material->setCurrentIndex(mi >= 0 ? mi : 0);
    top->addWidget(new QLabel(QStringLiteral("Material"), this));
    top->addWidget(m_material, 1);
    lay->addLayout(top);

    // Bit kinds, one icon each, in the order the library first lists them.
    static const QHash<QString, QString> kindNames = {
        {QStringLiteral("endmill"), QStringLiteral("End mill")},
        {QStringLiteral("ball"), QStringLiteral("Ball")},
        {QStringLiteral("vbit"), QStringLiteral("V-bit")},
        {QStringLiteral("engraver"), QStringLiteral("Engraver")},
        {QStringLiteral("roundover"), QStringLiteral("Corner round")}};
    auto *kinds = new QHBoxLayout;
    for (const LibraryTool &t : lib.tools()) {
        if (m_kindOrder.contains(t.kind))
            continue;
        m_kindOrder << t.kind;
        auto *b = new QToolButton(this);
        b->setCheckable(true);
        b->setIcon(toolKindIcon(t));
        b->setIconSize(QSize(24, 36));
        b->setText(kindNames.value(t.kind, t.kind));
        b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        b->setProperty("kind", t.kind);
        b->setToolTip(QStringLiteral("Show %1 bits; press several to list them in that order")
                          .arg(b->text().toLower()));
        connect(b, &QToolButton::toggled, this,
                [this, kind = t.kind](bool on) { kindToggled(kind, on); });
        m_kindButtons << b;
        kinds->addWidget(b);
    }
    kinds->addStretch(1);

    // Collections, at the right end of the same row.
    m_collections = ToolCollections::loadSettings();
    kinds->addWidget(new QLabel(QStringLiteral("Collection"), this));
    m_collection = new QComboBox(this);
    m_collection->setMinimumWidth(150);
    m_collection->setToolTip(QStringLiteral("Show only the bits in one collection"));
    kinds->addWidget(m_collection);
    m_addTo = new QToolButton(this);
    m_addTo->setText(QStringLiteral("Add to"));
    m_addTo->setToolTip(QStringLiteral("Put the selected bit in a collection"));
    m_addTo->setPopupMode(QToolButton::InstantPopup);
    m_addTo->setMenu(new QMenu(m_addTo));
    connect(m_addTo->menu(), &QMenu::aboutToShow, this, [this] { fillAddMenu(); });
    kinds->addWidget(m_addTo);
    m_removeFrom = new QToolButton(this);
    m_removeFrom->setText(QStringLiteral("Remove"));
    m_removeFrom->setToolTip(QStringLiteral("Take the selected bit out of this collection"));
    connect(m_removeFrom, &QToolButton::clicked, this, [this] {
        if (const LibraryTool *t = selectedTool()) {
            m_collections.removeTool(shownCollection(), t->number);
            collectionsChanged(shownCollection());
        }
    });
    kinds->addWidget(m_removeFrom);
    auto *manage = new QToolButton(this);
    manage->setText(QStringLiteral("Collections"));
    manage->setPopupMode(QToolButton::InstantPopup);
    auto *mm = new QMenu(manage);
    mm->addAction(QStringLiteral("New collection…"), this, [this] { newCollection(-1); });
    QAction *ren = mm->addAction(QStringLiteral("Rename…"), this, [this] { renameCollection(); });
    QAction *del = mm->addAction(QStringLiteral("Delete"), this, [this] { deleteCollection(); });
    mm->addSeparator();
    mm->addAction(QStringLiteral("Save to file…"), this, [this] { saveCollections(); });
    mm->addAction(QStringLiteral("Load from file…"), this, [this] { loadCollections(); });
    connect(mm, &QMenu::aboutToShow, this, [this, ren, del] {
        ren->setEnabled(!shownCollection().isEmpty());
        del->setEnabled(!shownCollection().isEmpty());
    });
    manage->setMenu(mm);
    kinds->addWidget(manage);
    lay->addLayout(kinds);
    const QStringList saved =
        QSettings().value(QStringLiteral("ui/toolKinds")).toStringList();
    for (const QString &k : saved)
        if (m_kindOrder.contains(k) && !m_marked.contains(k))
            m_marked << k;
    for (QToolButton *b : m_kindButtons) {
        const QSignalBlocker block(b);
        b->setChecked(m_marked.contains(b->property("kind").toString()));
    }

    auto *mid = new QHBoxLayout;
    m_list = new QListWidget(this);
    m_list->setIconSize(QSize(24, 36));
    mid->addWidget(m_list, 3);
    m_preview = new QLabel(this);
    m_preview->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_preview->setWordWrap(true);
    m_preview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_preview->setMinimumWidth(240);
    m_preview->setStyleSheet(QStringLiteral("QLabel { font-family: monospace; padding: 6px; }"));
    m_diagram = new ToolDiagram(this);
    mid->addWidget(m_diagram, 0);
    auto *right = new QVBoxLayout;
    right->addWidget(m_preview, 1);
    // Stock row: how many are in the drawer, and a bin for the one that just
    // snapped. The bin takes one out of stock and counts it as thrown away.
    auto *stock = new QHBoxLayout;
    stock->addWidget(new QLabel(QStringLiteral("In stock"), this));
    m_stock = new QSpinBox(this);
    m_stock->setRange(0, 999);
    m_stock->setToolTip(QStringLiteral("How many of this bit you have"));
    stock->addWidget(m_stock);
    m_trash = new QToolButton(this);
    m_trash->setIcon(QIcon::fromTheme(QStringLiteral("user-trash"),
                                      QIcon::fromTheme(QStringLiteral("edit-delete"))));
    m_trash->setText(QStringLiteral("Broke one"));
    m_trash->setToolButtonStyle(m_trash->icon().isNull() ? Qt::ToolButtonTextOnly
                                                          : Qt::ToolButtonTextBesideIcon);
    m_trash->setToolTip(QStringLiteral("One bit broke and went in the bin: one less in stock"));
    stock->addWidget(m_trash);
    m_broken = new QLabel(this);
    stock->addWidget(m_broken);
    stock->addStretch(1);
    right->addLayout(stock);
    mid->addLayout(right, 2);
    lay->addLayout(mid, 1);
    if (auto *ss = QCoreApplication::instance()->findChild<StockSync *>())
        connect(ss, &StockSync::changed, this, &ToolLibraryDialog::reloadFromSettings);
    connect(m_stock, &QSpinBox::valueChanged, this, [this](int v) {
        if (const LibraryTool *t = selectedTool()) {
            QSettings().setValue(QStringLiteral("toolStock/%1/have").arg(t->number), v);
            stampStock(t->number);
            // The bin and the list's "· N in stock" follow the typed count,
            // not the one the bit was selected with.
            m_trash->setEnabled(v > 0);
            if (QListWidgetItem *it = m_list->currentItem())
                it->setText(itemLabel(*t));
        }
    });
    connect(m_trash, &QToolButton::clicked, this, [this] {
        const LibraryTool *t = selectedTool();
        if (!t)
            return;
        QSettings st;
        st.setValue(QStringLiteral("toolStock/%1/broken").arg(t->number), stockBroken(*t) + 1);
        st.setValue(QStringLiteral("toolStock/%1/have").arg(t->number), qMax(0, stockHave(*t) - 1));
        st.sync();
        stampStock(t->number);
        updateStock();
    });

    auto *src = new QLabel(QStringLiteral("Library: %1 · %2 tools · values flagged "
                                          "(guess) are conservative, not CC defaults")
                               .arg(lib.source()).arg(lib.tools().size()), this);
    src->setWordWrap(true);
    lay->addWidget(src);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Use tool"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);

    rebuildCollections(QSettings().value(QStringLiteral("ui/toolCollection")).toString());
    connect(m_collection, &QComboBox::currentIndexChanged, this, [this] {
        QSettings().setValue(QStringLiteral("ui/toolCollection"), shownCollection());
        rebuildList();
    });
    connect(m_search, &QLineEdit::textChanged, this, [this] { rebuildList(); });
    connect(m_material, &QComboBox::currentIndexChanged, this, [this] { updatePreview(); });
    connect(m_list, &QListWidget::currentRowChanged, this, [this] { updatePreview(); updateStock(); });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this] { accept(); });

    rebuildList();
    for (int i = 0; i < m_list->count(); ++i)
        if (m_list->item(i)->data(Qt::UserRole).toInt() == currentToolNumber) {
            m_list->setCurrentRow(i);
            break;
        }
    if (m_list->currentRow() < 0 && m_list->count() > 0)
        m_list->setCurrentRow(0);
    updateStock();
}

int ToolLibraryDialog::stockHave(const LibraryTool &t)
{
    return QSettings().value(QStringLiteral("toolStock/%1/have").arg(t.number), t.bought).toInt();
}

int ToolLibraryDialog::stockBroken(const LibraryTool &t)
{
    return QSettings().value(QStringLiteral("toolStock/%1/broken").arg(t.number), 0).toInt();
}

QString ToolLibraryDialog::itemLabel(const LibraryTool &t)
{
    QString label = QStringLiteral("%1   Ø%2 mm · %3 fl")
                        .arg(t.name, mmText(t.diameter), QString::number(t.flutes));
    if (t.angle > 0)
        label += QStringLiteral(" · %1°").arg(t.angle);
    if (t.uncertain)
        label += QStringLiteral(" · unverified");
    if (const int have = stockHave(t); have > 0 || t.bought > 0)
        label += QStringLiteral(" · %1 in stock").arg(have);
    return label;
}

void ToolLibraryDialog::updateStock()
{
    const LibraryTool *t = selectedTool();
    m_stock->setEnabled(t);
    m_trash->setEnabled(t && stockHave(*t) > 0);
    if (!t) {
        m_broken->clear();
        return;
    }
    const QSignalBlocker block(m_stock);
    m_stock->setValue(stockHave(*t));
    if (QListWidgetItem *it = m_list->currentItem())
        it->setText(itemLabel(*t));       // "· N in stock" follows the count
    const int b = stockBroken(*t);
    m_broken->setText(b == 0 ? QString()
                      : b == 1 ? QStringLiteral("1 thrown away")
                               : QStringLiteral("%1 thrown away").arg(b));
}

void ToolLibraryDialog::kindToggled(const QString &kind, bool on)
{
    m_marked.removeAll(kind);
    if (on)
        m_marked << kind;
    QSettings().setValue(QStringLiteral("ui/toolKinds"), m_marked);
    rebuildList();
}

void ToolLibraryDialog::rebuildList()
{
    const QString q = m_search->text().trimmed();
    const int keep = selectedTool() ? selectedTool()->number : -1;
    m_list->clear();
    // Marked kinds first-pressed first; nothing marked lists every kind in
    // the row's order. Within a kind the library's own order stays.
    // A collection lists its bits in the order they were added.
    const QStringList order = m_marked.isEmpty() ? m_kindOrder : m_marked;
    QVector<const LibraryTool *> tools;
    if (const ToolCollection *c = m_collections.byName(shownCollection())) {
        for (int n : c->tools)
            if (const LibraryTool *t = m_lib.byNumber(n); t && order.contains(t->kind))
                tools << t;
    } else {
        for (const LibraryTool &t : m_lib.tools())
            if (order.contains(t.kind))
                tools << &t;
    }
    // A collection with no kind pressed keeps its own order.
    if (!m_marked.isEmpty() || shownCollection().isEmpty())
        std::stable_sort(tools.begin(), tools.end(), [&order](auto *a, auto *b) {
            return order.indexOf(a->kind) < order.indexOf(b->kind);
        });
    for (const LibraryTool *tp : tools) {
        const LibraryTool &t = *tp;
        QString hay = QStringLiteral("#%1 %2 %3 %4 %5mm")
                          .arg(QString::number(t.number), t.name, t.kind, t.model, QString::number(t.diameter));
        if (!q.isEmpty() && !hay.contains(q, Qt::CaseInsensitive))
            continue;
        auto *it = new QListWidgetItem(itemLabel(t), m_list);
        it->setData(Qt::UserRole, t.number);
        it->setIcon(toolKindIcon(t));
        it->setToolTip(t.notes);
        if (t.number == keep)
            m_list->setCurrentItem(it);
    }
    if (m_list->currentRow() < 0 && m_list->count() > 0)
        m_list->setCurrentRow(0);
    updatePreview();
}

// ---- collections ----------------------------------------------------------

QString ToolLibraryDialog::shownCollection() const
{
    return m_collection->currentData().toString();
}

void ToolLibraryDialog::rebuildCollections(const QString &show)
{
    {
        const QSignalBlocker block(m_collection);
        m_collection->clear();
        m_collection->addItem(QStringLiteral("All bits"), QString());
        for (const ToolCollection &c : m_collections.all())
            m_collection->addItem(QStringLiteral("%1 (%2)").arg(c.name, QString::number(c.tools.size())),
                                  c.name);
        const int i = m_collection->findData(show);
        m_collection->setCurrentIndex(i >= 0 ? i : 0);
    }
    QSettings().setValue(QStringLiteral("ui/toolCollection"), shownCollection());
    m_removeFrom->setEnabled(!shownCollection().isEmpty());
}

void ToolLibraryDialog::reloadFromSettings()
{
    m_collections = ToolCollections::loadSettings();
    collectionsChanged(shownCollection());
    updateStock();
}

void ToolLibraryDialog::collectionsChanged(const QString &show)
{
    m_collections.saveSettings();
    rebuildCollections(show);
    rebuildList();
}

void ToolLibraryDialog::fillAddMenu()
{
    QMenu *m = m_addTo->menu();
    m->clear();
    const LibraryTool *t = selectedTool();
    for (const ToolCollection &c : m_collections.all()) {
        QAction *a = m->addAction(c.name);
        a->setEnabled(t && !c.tools.contains(t->number));
        const QString name = c.name;
        connect(a, &QAction::triggered, this, [this, name, n = t ? t->number : -1] {
            m_collections.addTool(name, n);
            collectionsChanged(shownCollection());
        });
    }
    m->addSeparator();
    QAction *nw = m->addAction(QStringLiteral("New collection…"));
    nw->setEnabled(t);
    connect(nw, &QAction::triggered, this,
            [this, n = t ? t->number : -1] { newCollection(n); });
}

void ToolLibraryDialog::newCollection(int addTool)
{
    const QString name = QInputDialog::getText(this, QStringLiteral("New collection"),
                                               QStringLiteral("Name"));
    if (name.trimmed().isEmpty())
        return;
    if (!m_collections.add(name)) {
        QMessageBox::information(this, QStringLiteral("New collection"),
                                 QStringLiteral("There is already a collection called %1.")
                                     .arg(name.trimmed()));
        return;
    }
    if (addTool >= 0)
        m_collections.addTool(name.trimmed(), addTool);
    collectionsChanged(addTool >= 0 ? shownCollection() : name.trimmed());
}

void ToolLibraryDialog::renameCollection()
{
    const QString from = shownCollection();
    if (from.isEmpty())
        return;
    const QString to = QInputDialog::getText(this, QStringLiteral("Rename collection"),
                                             QStringLiteral("Name"), QLineEdit::Normal, from);
    if (to.trimmed().isEmpty() || to.trimmed() == from)
        return;
    if (!m_collections.rename(from, to)) {
        QMessageBox::information(this, QStringLiteral("Rename collection"),
                                 QStringLiteral("There is already a collection called %1.")
                                     .arg(to.trimmed()));
        return;
    }
    collectionsChanged(to.trimmed());
}

void ToolLibraryDialog::deleteCollection()
{
    const QString name = shownCollection();
    if (name.isEmpty())
        return;
    if (QMessageBox::question(this, QStringLiteral("Delete collection"),
                              QStringLiteral("Delete the collection %1? The bits stay in "
                                             "the library.").arg(name))
        != QMessageBox::Yes)
        return;
    m_collections.remove(name);
    collectionsChanged(QString());
}

void ToolLibraryDialog::saveCollections()
{
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save collections"),
                                                QStringLiteral("tool-collections.json"),
                                                QStringLiteral("Tool collections (*.json)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(QLatin1String(".json"), Qt::CaseInsensitive))
        path += QLatin1String(".json");
    QString err;
    if (!m_collections.saveFile(path, &err))
        QMessageBox::warning(this, QStringLiteral("Save collections"), err);
}

void ToolLibraryDialog::loadCollections()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Load collections"),
                                                      QString(),
                                                      QStringLiteral("Tool collections (*.json)"));
    if (path.isEmpty())
        return;
    ToolCollections loaded;
    QString err;
    if (!ToolCollections::loadFile(path, &loaded, &err)) {
        QMessageBox::warning(this, QStringLiteral("Load collections"), err);
        return;
    }
    m_collections.merge(loaded);
    collectionsChanged(loaded.all().isEmpty() ? shownCollection() : loaded.all().first().name);
}

const LibraryTool *ToolLibraryDialog::selectedTool() const
{
    const QListWidgetItem *it = m_list->currentItem();
    return it ? m_lib.byNumber(it->data(Qt::UserRole).toInt()) : nullptr;
}

QString ToolLibraryDialog::selectedMaterialId() const
{
    return m_material->currentData().toString();
}

ToolFeeds ToolLibraryDialog::selectedFeeds() const
{
    const LibraryTool *t = selectedTool();
    if (!t)
        return ToolFeeds();
    if (const ToolFeeds *f = t->feedsFor(selectedMaterialId()))
        return *f;
    return ToolFeeds();
}

void ToolLibraryDialog::updatePreview()
{
    const LibraryTool *t = selectedTool();
    m_diagram->setTool(t);
    if (!t) {
        m_preview->setText(QStringLiteral("No tool selected"));
        return;
    }
    const ToolFeeds f = selectedFeeds();
    static const char *kinds[] = {"square end mill", "ball end mill", "V-bit"};
    const QString kind = t->kind == QLatin1String("roundover")
        ? QStringLiteral("corner-rounding bit, Ø = cutting tip")
        : QLatin1String(kinds[t->ccType()]);
    QString s = QStringLiteral("%1\n%2 · Ø %3 mm · %4 flutes")
                    .arg(t->name, kind, mmText(t->diameter), QString::number(t->flutes));
    if (t->angle > 0)
        s += QStringLiteral(" · %1°").arg(t->angle);
    if (t->price > 0) {
        s += QStringLiteral("\nPrice: %1 %2").arg(t->currency).arg(t->price, 0, 'f', 2);
        if (!t->priceNote.isEmpty())
            s += QStringLiteral(" (%1)").arg(t->priceNote);
    }
    s += QStringLiteral("\n\n%1:\n").arg(m_lib.materialName(selectedMaterialId()));
    if (!f.valid()) {
        s += QStringLiteral("  no feeds for this material");
    } else {
        s += QStringLiteral("  rpm         %1\n  feed        %2 mm/min\n  plunge      %3 mm/min\n"
                            "  stepdown    %4 mm\n  stepover    %5 % (%6 mm)")
                 .arg(f.rpm).arg(f.feed).arg(f.plunge).arg(f.stepdown)
                 .arg(f.stepoverPct).arg(t->diameter * f.stepoverPct / 100.0, 0, 'f', 2);
        if (f.guess)
            s += QStringLiteral("\n  (guess, not a CC default)");
        if (!f.note.isEmpty())
            s += QStringLiteral("\n  %1").arg(f.note);
    }
    if (!t->notes.isEmpty())
        s += QStringLiteral("\n\n%1").arg(t->notes);
    m_preview->setText(s);
}

} // namespace c2d
