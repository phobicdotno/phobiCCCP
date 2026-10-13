#include "toollibrarydialog.h"
#include "tooldiagram.h"

#include <algorithm>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>

namespace c2d {

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
    mid->addWidget(m_preview, 2);
    lay->addLayout(mid, 1);

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

    connect(m_search, &QLineEdit::textChanged, this, [this] { rebuildList(); });
    connect(m_material, &QComboBox::currentIndexChanged, this, [this] { updatePreview(); });
    connect(m_list, &QListWidget::currentRowChanged, this, [this] { updatePreview(); });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this] { accept(); });

    rebuildList();
    for (int i = 0; i < m_list->count(); ++i)
        if (m_list->item(i)->data(Qt::UserRole).toInt() == currentToolNumber) {
            m_list->setCurrentRow(i);
            break;
        }
    if (m_list->currentRow() < 0 && m_list->count() > 0)
        m_list->setCurrentRow(0);
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
    const QStringList order = m_marked.isEmpty() ? m_kindOrder : m_marked;
    QVector<const LibraryTool *> tools;
    for (const LibraryTool &t : m_lib.tools())
        if (order.contains(t.kind))
            tools << &t;
    std::stable_sort(tools.begin(), tools.end(), [&order](auto *a, auto *b) {
        return order.indexOf(a->kind) < order.indexOf(b->kind);
    });
    for (const LibraryTool *tp : tools) {
        const LibraryTool &t = *tp;
        QString hay = QStringLiteral("#%1 %2 %3 %4 %5mm")
                          .arg(t.number).arg(t.name, t.kind, t.model).arg(t.diameter);
        if (!q.isEmpty() && !hay.contains(q, Qt::CaseInsensitive))
            continue;
        QString label = QStringLiteral("%1   Ø%2 mm · %3 fl")
                            .arg(t.name).arg(t.diameter).arg(t.flutes);
        if (t.angle > 0)
            label += QStringLiteral(" · %1°").arg(t.angle);
        if (t.uncertain)
            label += QStringLiteral("  (number unverified)");
        auto *it = new QListWidgetItem(label, m_list);
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
                    .arg(t->name, kind)
                    .arg(t->diameter).arg(t->flutes);
    if (t->angle > 0)
        s += QStringLiteral(" · %1°").arg(t->angle);
    s += QStringLiteral("\n\n%1:\n").arg(m_lib.materialName(selectedMaterialId()));
    if (!f.valid()) {
        s += QStringLiteral("  no feeds for this material");
    } else {
        s += QStringLiteral("  rpm         %1\n  feed        %2 mm/min\n  plunge      %3 mm/min\n"
                            "  stepdown    %4 mm\n  stepover    %5 % (%6 mm)")
                 .arg(f.rpm).arg(f.feed).arg(f.plunge).arg(f.stepdown)
                 .arg(f.stepoverPct).arg(t->diameter * f.stepoverPct / 100.0, 0, 'f', 2);
        if (f.guess)
            s += QStringLiteral("\n  (guess — conservative, not a CC default)");
        if (!f.note.isEmpty())
            s += QStringLiteral("\n  %1").arg(f.note);
    }
    if (!t->notes.isEmpty())
        s += QStringLiteral("\n\n%1").arg(t->notes);
    m_preview->setText(s);
}

} // namespace c2d
