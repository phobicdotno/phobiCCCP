#include "sendtopaddialog.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace c2d {

SendToPadDialog::SendToPadDialog(const QString &name, bool receiving, Hooks hooks, QWidget *parent)
    : QDialog(parent), m_name(name), m_hooks(std::move(hooks))
{
    setWindowTitle(QStringLiteral("Send to pad"));
    auto *lay = new QVBoxLayout(this);
    auto *intro = new QLabel(QStringLiteral("Send “%1” to a pad to edit it there. It opens on the pad "
                                            "within a few seconds, and what is changed there comes back "
                                            "to this file.").arg(name));
    intro->setWordWrap(true);
    intro->setTextFormat(Qt::PlainText);
    lay->addWidget(intro);
    m_list = new QListWidget;
    m_list->setObjectName(QStringLiteral("pads"));
    m_list->setMinimumHeight(120);
    lay->addWidget(m_list, 1);
    m_status = new QLabel;
    m_status->setObjectName(QStringLiteral("status"));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    lay->addWidget(m_status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    m_send = buttons->addButton(QStringLiteral("Send"), QDialogButtonBox::AcceptRole);
    m_send->setEnabled(false);
    lay->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_send, &QPushButton::clicked, this, &SendToPadDialog::send);
    connect(m_list, &QListWidget::currentRowChanged, this,
            [this](int row) { m_send->setEnabled(row >= 0 && m_seq == 0); });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this] {
        if (m_send->isEnabled())
            send();
    });
    resize(460, 320);

    if (!receiving) {
        m_status->setText(QStringLiteral("Pads cannot reach this computer: turn on "
                                         "File › Receive from Syntexophobic and pads first."));
        return;
    }
    auto *timer = new QTimer(this);
    timer->setInterval(500);
    connect(timer, &QTimer::timeout, this, &SendToPadDialog::tick);
    timer->start();
    refresh();
}

void SendToPadDialog::refresh()
{
    const QList<padlink::Pads::Pad> pads = m_hooks.pads();
    QStringList ids, shown;
    for (const auto &p : pads)
        ids << p.id;
    for (const auto &p : m_shown)
        shown << p.id;
    if (ids != shown) {
        const QString current = m_list->currentRow() >= 0 && m_list->currentRow() < m_shown.size()
                                    ? m_shown.at(m_list->currentRow()).id : QString();
        m_shown = pads;
        m_list->clear();
        for (const auto &p : pads)
            m_list->addItem(p.name == p.address ? p.name : QStringLiteral("%1 — %2").arg(p.name, p.address));
        const int keep = ids.indexOf(current);
        m_list->setCurrentRow(keep >= 0 ? keep : (pads.size() == 1 ? 0 : -1));
    }
    if (m_seq == 0)
        m_status->setText(pads.isEmpty()
                              ? QStringLiteral("No pad is in touch. Open PhobiCCCP on the pad, on the same "
                                               "network as this computer; it shows here within a few seconds.")
                              : QString());
}

void SendToPadDialog::send()
{
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_shown.size())
        return;
    QString note, err;
    const auto pad = m_shown.at(row);
    const qint64 seq = m_hooks.send(pad.id, &note, &err);
    if (seq == 0) {
        m_status->setText(QStringLiteral("Not sent: %1").arg(err));
        return;
    }
    m_seq = seq;
    m_padId = pad.id;
    m_padName = pad.name;
    m_note = note;
    m_sentAt = QDateTime::currentMSecsSinceEpoch();
    m_send->setEnabled(false);
    m_status->setText(QStringLiteral("Waiting for %1 to take it…").arg(m_padName));
}

void SendToPadDialog::tick()
{
    refresh();
    if (m_seq == 0)
        return;
    const QString note = m_note.isEmpty() ? QString() : QStringLiteral(" ") + m_note;
    if (!m_hooks.waiting(m_padId, m_seq)) {
        m_status->setText(QStringLiteral("“%1” is open on %2.%3").arg(m_name, m_padName, note));
        m_seq = 0;
        m_send->setEnabled(m_list->currentRow() >= 0);
        return;
    }
    if (QDateTime::currentMSecsSinceEpoch() - m_sentAt > padlink::Pads::kOpenMs) {
        m_status->setText(QStringLiteral("%1 did not take it. Is the pad app up to date (1.3.0 or "
                                         "newer) and still open?").arg(m_padName));
        m_seq = 0;
        m_send->setEnabled(m_list->currentRow() >= 0);
    }
}

} // namespace c2d
