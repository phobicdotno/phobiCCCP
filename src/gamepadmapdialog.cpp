#include "gamepadmapdialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace c2d {

namespace {
const QString kNone = QStringLiteral("none");
}

GamepadMapDialog::GamepadMapDialog(const GamepadMap &map, Gamepad *pad, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Gamepad buttons"));
    auto *lay = new QVBoxLayout(this);

    m_hint = new QLabel(this);
    m_hint->setWordWrap(true);
    lay->addWidget(m_hint);

    m_table = new QTableWidget(0, 2, this);
    m_table->setHorizontalHeaderLabels({QStringLiteral("Button"), QStringLiteral("Action")});
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setMinimumSize(300, 280);
    lay->addWidget(m_table, 1);

    auto *note = new QLabel(QStringLiteral(
        "The D-pad's X and Y always jog, whether the pad reports it as axes or as "
        "buttons. Starting a program is never on the pad."), this);
    note->setWordWrap(true);
    note->setEnabled(false);
    lay->addWidget(note);

    auto *row = new QHBoxLayout;
    auto *remove = new QPushButton(QStringLiteral("Remove"), this);
    remove->setToolTip(QStringLiteral("Unmap the selected button"));
    auto *reset = new QPushButton(QStringLiteral("Reset to defaults"), this);
    reset->setToolTip(QStringLiteral("The SNES-style layout phobiCCCP ships with"));
    row->addWidget(remove);
    row->addWidget(reset);
    row->addStretch(1);
    lay->addLayout(row);
    connect(remove, &QPushButton::clicked, this, [this] {
        const int r = m_table->currentRow();
        if (r >= 0)
            m_table->removeRow(r);
    });
    connect(reset, &QPushButton::clicked, this, [this] { setMapping(gamepadmap::defaults()); });

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(bb);

    if (pad && pad->isConnected())
        m_hint->setText(QStringLiteral("Press a button on %1 to find its row, then pick "
                                       "what it does.").arg(pad->deviceName()));
    else
        m_hint->setText(QStringLiteral("No gamepad connected. Plug one in and press a "
                                       "button to find its row, then pick what it does."));
    if (pad) {
        connect(pad, &Gamepad::buttonChanged, this, [this](int number, bool pressed) {
            if (pressed)
                selectButton(number);
        });
        connect(pad, &Gamepad::connected, this, [this](const QString &name) {
            m_hint->setText(QStringLiteral("Press a button on %1 to find its row, then "
                                           "pick what it does.").arg(name));
        });
    }

    setMapping(map);
}

void GamepadMapDialog::setMapping(const GamepadMap &map)
{
    m_table->setRowCount(0);
    QList<int> keys = map.keys();
    std::sort(keys.begin(), keys.end());
    for (int n : keys)
        addRow(n, map.value(n));
}

int GamepadMapDialog::rowFor(int number) const
{
    for (int r = 0; r < m_table->rowCount(); ++r)
        if (m_table->item(r, 0)->data(Qt::UserRole).toInt() == number)
            return r;
    return -1;
}

int GamepadMapDialog::addRow(int number, const QString &action)
{
    // Keep the rows in button order, so the table reads like the pad.
    int r = 0;
    while (r < m_table->rowCount() && m_table->item(r, 0)->data(Qt::UserRole).toInt() < number)
        ++r;
    m_table->insertRow(r);
    auto *item = new QTableWidgetItem(QString::number(number));
    item->setData(Qt::UserRole, number);
    item->setTextAlignment(Qt::AlignCenter);
    m_table->setItem(r, 0, item);

    auto *combo = new QComboBox(m_table);
    combo->addItem(QStringLiteral("(nothing)"), kNone);
    for (const QString &a : gamepadmap::actions())
        combo->addItem(gamepadmap::label(a), a);
    int i = combo->findData(action);
    if (i < 0 && !action.isEmpty() && action != kNone) {
        // Typed into the settings by hand and not an action: keep it visible
        // rather than silently turning it into something else.
        combo->addItem(QStringLiteral("%1 (unknown)").arg(action), action);
        i = combo->count() - 1;
    }
    combo->setCurrentIndex(qMax(0, i));
    m_table->setCellWidget(r, 1, combo);
    return r;
}

void GamepadMapDialog::selectButton(int number)
{
    int r = rowFor(number);
    if (r < 0)
        r = addRow(number, kNone);
    m_table->selectRow(r);
    m_table->scrollToItem(m_table->item(r, 0));
    if (QWidget *w = m_table->cellWidget(r, 1))
        w->setFocus();
}

GamepadMap GamepadMapDialog::mapping() const
{
    GamepadMap map;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        const auto *combo = qobject_cast<QComboBox *>(m_table->cellWidget(r, 1));
        const QString action = combo ? combo->currentData().toString() : kNone;
        if (action != kNone)
            map.insert(m_table->item(r, 0)->data(Qt::UserRole).toInt(), action);
    }
    return map;
}

} // namespace c2d
