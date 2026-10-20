#include "sendtopaddialog.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QFile>
#include <QHostAddress>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSysInfo>
#include <QTimer>
#include <QUdpSocket>
#include <QVBoxLayout>

namespace c2d {

SendToPadDialog::SendToPadDialog(const QString &name, bool receiving, Hooks hooks, QWidget *parent)
    : QDialog(parent), m_name(name), m_receiving(receiving), m_hooks(std::move(hooks))
{
    setWindowTitle(QStringLiteral("Send to pad"));
    auto *lay = new QVBoxLayout(this);
    auto *intro = new QLabel(QStringLiteral("Send “%1” to a pad to edit it there. It opens on the pad "
                                            "within a few seconds.").arg(name));
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
    connect(m_list, &QListWidget::currentRowChanged, this, [this] { idle(); });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this] {
        if (m_send->isEnabled())
            send();
    });
    resize(460, 320);

    m_udp = new QUdpSocket(this);
    m_udp->bind(QHostAddress::AnyIPv4, 0);
    connect(m_udp, &QUdpSocket::readyRead, this, [this] {
        for (const SendToDialog::Computer &c : SendToDialog::answers(m_udp)) {
            if (!c.pad || (!c.id.isEmpty() && c.id == m_hooks.ownId))
                continue;
            bool known = false;
            for (const auto &o : m_found)
                known = known || (!c.id.isEmpty() && o.id == c.id) || (o.address == c.address && o.port == c.port);
            if (!known)
                m_found.append(c);
        }
        refresh();
    });
    SendToDialog::discover(m_udp);
    auto *timer = new QTimer(this);
    timer->setInterval(500);
    connect(timer, &QTimer::timeout, this, &SendToPadDialog::tick);
    timer->start();
    refresh();
}

void SendToPadDialog::refresh()
{
    QList<Row> rows;
    if (m_receiving)
        for (const auto &p : m_hooks.pads())
            rows.append(Row{true, p.id, p.name, p.address, 0});
    for (const auto &c : m_found)
        rows.append(Row{false, c.id, c.host.isEmpty() ? c.address : c.host, c.address, c.port});
    auto key = [](const Row &r) { return QStringLiteral("%1|%2|%3").arg(r.app).arg(r.id, r.address); };
    QStringList now, before;
    for (const auto &r : rows)
        now << key(r);
    for (const auto &r : m_rows)
        before << key(r);
    if (now != before) {
        const int cur = m_list->currentRow();
        const QString current = cur >= 0 && cur < m_rows.size() ? key(m_rows.at(cur)) : QString();
        m_rows = rows;
        QSignalBlocker block(m_list);
        m_list->clear();
        for (const auto &r : rows) {
            QString label = r.name == r.address ? r.name : QStringLiteral("%1 — %2").arg(r.name, r.address);
            if (!r.app)
                label += QStringLiteral(" (phobiCCCP)");
            m_list->addItem(label);
        }
        const int keep = now.indexOf(current);
        m_list->setCurrentRow(keep >= 0 ? keep : (rows.size() == 1 ? 0 : -1));
    }
    if (m_seq == 0 && !m_busy && rows.isEmpty()) {
        m_status->setText(m_receiving
                              ? QStringLiteral("No pad is in touch. Open PhobiCCCP on the pad, on the same "
                                               "network as this computer; it shows here within a few seconds.")
                              : QStringLiteral("No pad found. The pad app cannot reach this computer while "
                                               "File › Receive from Syntexophobic and pads is off."));
        m_emptyShown = true;
    } else if (m_emptyShown && !rows.isEmpty()) {
        m_status->clear();
        m_emptyShown = false;
    }
    idle();
}

void SendToPadDialog::idle()
{
    m_send->setEnabled(m_seq == 0 && !m_busy && m_list->currentRow() >= 0 && m_list->currentRow() < m_rows.size());
}

void SendToPadDialog::send()
{
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_rows.size() || m_seq != 0 || m_busy)
        return;
    const Row r = m_rows.at(row);
    if (!r.app) {
        sendWhole(r);
        return;
    }
    QString note, err;
    const qint64 seq = m_hooks.send(r.id, &note, &err);
    if (seq == 0) {
        m_status->setText(QStringLiteral("Not sent: %1").arg(err));
        return;
    }
    m_seq = seq;
    m_padId = r.id;
    m_padName = r.name;
    m_note = note;
    m_sentAt = QDateTime::currentMSecsSinceEpoch();
    idle();
    m_status->setText(QStringLiteral("Waiting for %1 to take it…").arg(m_padName));
}

void SendToPadDialog::sendWhole(const Row &r)
{
    QString err;
    const QString path = m_hooks.makeCopy(&err);
    QFile f(path);
    if (path.isEmpty() || !f.open(QIODevice::ReadOnly)) {
        m_status->setText(QStringLiteral("Could not save a copy to send: %1").arg(err.isEmpty() ? f.errorString() : err));
        return;
    }
    if (f.size() > SendToDialog::kMaxDesign) {
        m_status->setText(QStringLiteral("The design is %1 MB; at most %2 MB can be sent.")
                              .arg(f.size() >> 20).arg(SendToDialog::kMaxDesign >> 20));
        return;
    }
    const QByteArray data = f.readAll();
    f.close();
    QFile::remove(path);
    const QJsonObject req{{QStringLiteral("op"), QStringLiteral("open")},
                          {QStringLiteral("name"), m_name},
                          {QStringLiteral("c2d"), QString::fromLatin1(data.toBase64())},
                          {QStringLiteral("machine"), QString()},
                          {QStringLiteral("start"), false},
                          {QStringLiteral("from"), QSysInfo::machineHostName()}};
    m_busy = true;
    idle();
    m_status->setText(QStringLiteral("Sending to %1… (it may ask there first)").arg(r.name));
    // Long: the pad may ask its user to save first.
    SendToDialog::call(this, r.address, r.port, req, 5 * 60 * 1000, [this, r](const QJsonObject &a) {
        m_busy = false;
        idle();
        m_status->setText(a.value(QStringLiteral("ok")).toBool()
                              ? QStringLiteral("“%1” is open on %2. Send it back from there with "
                                               "File › Send to phobiCCCP….").arg(m_name, r.name)
                              : QStringLiteral("Not sent: %1").arg(a.value(QStringLiteral("error")).toString()));
    });
}

void SendToPadDialog::tick()
{
    // Pads running this program are asked again now and then: one may be
    // switched on meanwhile.
    if (++m_ticks % 6 == 0)
        SendToDialog::discover(m_udp);
    refresh();
    if (m_seq == 0)
        return;
    const QString note = m_note.isEmpty() ? QString() : QStringLiteral(" ") + m_note;
    if (!m_hooks.waiting(m_padId, m_seq)) {
        m_status->setText(QStringLiteral("“%1” is open on %2; edits there come back to this file.%3")
                              .arg(m_name, m_padName, note));
        m_seq = 0;
        idle();
        return;
    }
    if (QDateTime::currentMSecsSinceEpoch() - m_sentAt > padlink::Pads::kOpenMs) {
        m_status->setText(QStringLiteral("%1 did not take it. Is the pad app up to date (1.3.0 or "
                                         "newer) and still open?").arg(m_padName));
        m_seq = 0;
        idle();
    }
}

} // namespace c2d
