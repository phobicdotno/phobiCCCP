#include "toollibrarydialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace c2d {

ToolLibraryDialog::ToolLibraryDialog(const ToolLibrary &lib, const QString &materialId,
                                     int currentToolNumber, QWidget *parent)
    : QDialog(parent), m_lib(lib)
{
    setWindowTitle(QStringLiteral("Tool library — speeds and feeds"));
    resize(620, 460);
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

    auto *mid = new QHBoxLayout;
    m_list = new QListWidget(this);
    mid->addWidget(m_list, 3);
    m_preview = new QLabel(this);
    m_preview->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_preview->setWordWrap(true);
    m_preview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_preview->setMinimumWidth(240);
    m_preview->setStyleSheet(QStringLiteral("QLabel { font-family: monospace; padding: 6px; }"));
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

void ToolLibraryDialog::rebuildList()
{
    const QString q = m_search->text().trimmed();
    const int keep = selectedTool() ? selectedTool()->number : -1;
    m_list->clear();
    for (const LibraryTool &t : m_lib.tools()) {
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
    if (!t) {
        m_preview->setText(QStringLiteral("No tool selected"));
        return;
    }
    const ToolFeeds f = selectedFeeds();
    static const char *kinds[] = {"square end mill", "ball end mill", "V-bit"};
    QString s = QStringLiteral("%1\n%2 · Ø %3 mm · %4 flutes")
                    .arg(t->name, QLatin1String(kinds[t->ccType()]))
                    .arg(t->diameter).arg(t->flutes);
    if (t->angle > 0)
        s += QStringLiteral(" · %1°").arg(t->angle);
    s += QStringLiteral("\n\n%1:\n").arg(m_lib.materialName(selectedMaterialId()));
    if (!f.valid()) {
        s += QStringLiteral("  no feeds for this material");
    } else {
        s += QStringLiteral("  rpm         %1\n  feed        %2 mm/min\n  plunge      %3 mm/min\n"
                            "  stepdown    %4 mm\n  stepover    %5 %% (%6 mm)")
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
