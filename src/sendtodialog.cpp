#include "sendtodialog.h"
#include "projectreceiver.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QSysInfo>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>
#include <QVBoxLayout>

namespace c2d {

SendToDialog::SendToDialog(const QString &name, const QString &ownId,
                           std::function<QString(QString *)> makeCopy, QWidget *parent)
    : QDialog(parent), m_name(name), m_ownId(ownId), m_makeCopy(std::move(makeCopy))
{
    setWindowTitle(QStringLiteral("Send to phobiCCCP"));
    auto *lay = new QVBoxLayout(this);
    auto *intro = new QLabel(QStringLiteral("Send “%1” to phobiCCCP on the computer by the machine. "
                                            "It opens there; it can also start cutting.").arg(name));
    intro->setWordWrap(true);
    intro->setTextFormat(Qt::PlainText);
    lay->addWidget(intro);

    m_list = new QListWidget;
    m_list->setMinimumHeight(140);
    lay->addWidget(m_list, 1);
    auto *addRow = new QHBoxLayout;
    m_address = new QLineEdit;
    m_address->setPlaceholderText(QStringLiteral("Not listed? Its address, e.g. 192.168.1.20 or shop-pc"));
    auto *add = new QPushButton(QStringLiteral("Add"));
    addRow->addWidget(m_address, 1);
    addRow->addWidget(add);
    lay->addLayout(addRow);

    auto *form = new QFormLayout;
    m_machine = new QComboBox;
    form->addRow(QStringLiteral("Machine"), m_machine);
    m_start = new QCheckBox(QStringLiteral("Start cutting when it opens"));
    form->addRow(QString(), m_start);
    lay->addLayout(form);
    m_status = new QLabel;
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    lay->addWidget(m_status);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    m_send = buttons->addButton(QStringLiteral("Send"), QDialogButtonBox::AcceptRole);
    m_send->setEnabled(false);
    lay->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_send, &QPushButton::clicked, this, &SendToDialog::send);
    connect(add, &QPushButton::clicked, this, &SendToDialog::addByAddress);
    connect(m_address, &QLineEdit::returnPressed, this, &SendToDialog::addByAddress);
    connect(m_list, &QListWidget::currentRowChanged, this, &SendToDialog::selectComputer);
    resize(520, 420);

    m_udp = new QUdpSocket(this);
    m_udp->bind(QHostAddress::AnyIPv4, 0);
    connect(m_udp, &QUdpSocket::readyRead, this, &SendToDialog::onDatagrams);
    // Asked again now and then: a computer may be switched on meanwhile.
    auto *again = new QTimer(this);
    again->setInterval(3000);
    connect(again, &QTimer::timeout, this, &SendToDialog::discover);
    again->start();
    discover();
    // The last one used, in case discovery does not reach it (Tailscale).
    const QString last = QSettings().value(QStringLiteral("sendTo/last")).toString();
    if (!last.isEmpty()) {
        m_address->setText(last);
        addByAddress();
        m_address->clear();
    }
    m_status->setText(QStringLiteral("Looking for phobiCCCP on the network…"));
}

void SendToDialog::discover()
{
    const QByteArray ask = QByteArrayLiteral("PHOBICCCP-DISCOVER 1");
    m_udp->writeDatagram(ask, QHostAddress::Broadcast, ProjectReceiver::kPort);
    for (const QNetworkInterface &i : QNetworkInterface::allInterfaces()) {
        if (!(i.flags() & QNetworkInterface::IsUp) || (i.flags() & QNetworkInterface::IsLoopBack))
            continue;
        for (const QNetworkAddressEntry &e : i.addressEntries())
            if (!e.broadcast().isNull())
                m_udp->writeDatagram(ask, e.broadcast(), ProjectReceiver::kPort);
    }
}

void SendToDialog::onDatagrams()
{
    while (m_udp->hasPendingDatagrams()) {
        const QNetworkDatagram d = m_udp->receiveDatagram(8192);
        const QJsonObject o = QJsonDocument::fromJson(d.data()).object();
        if (o.value(QStringLiteral("app")).toString() != QLatin1String("phobiCCCP"))
            continue;
        Computer c;
        c.id = o.value(QStringLiteral("id")).toString();
        c.host = o.value(QStringLiteral("host")).toString().left(60);
        c.machine = o.value(QStringLiteral("machine")).toString().left(60);
        c.port = quint16(o.value(QStringLiteral("port")).toInt(ProjectReceiver::kPort));
        QHostAddress a = d.senderAddress();
        if (a.protocol() == QAbstractSocket::IPv6Protocol) {
            bool ok = false;
            const QHostAddress v4(a.toIPv4Address(&ok));
            if (ok)
                a = v4;
        }
        c.address = a.toString();
        if (c.id.isEmpty() || c.id != m_ownId)
            addComputer(c);
    }
}

void SendToDialog::addComputer(const Computer &c)
{
    for (int i = 0; i < m_computers.size(); ++i) {
        Computer &o = m_computers[i];
        // The same install answering again (or on another of its addresses).
        if ((!c.id.isEmpty() && o.id == c.id) || (o.address == c.address && o.port == c.port)) {
            if (o.id.isEmpty() && !c.id.isEmpty()) {
                o = c;
                m_list->item(i)->setText(c.machine.isEmpty() ? c.host
                                                             : QStringLiteral("%1 — %2").arg(c.host, c.machine));
            }
            return;
        }
    }
    m_computers.append(c);
    QString label = c.host.isEmpty() ? c.address : c.host;
    if (!c.machine.isEmpty())
        label += QStringLiteral(" — %1").arg(c.machine);
    m_list->addItem(label);
    if (m_list->currentRow() < 0)
        m_list->setCurrentRow(0);
    if (m_status->text().startsWith(QLatin1String("Looking")))
        m_status->clear();
}

void SendToDialog::addByAddress()
{
    QString a = m_address->text().trimmed();
    if (a.isEmpty())
        return;
    Computer c;
    c.port = ProjectReceiver::kPort;
    if (const int colon = a.lastIndexOf(QLatin1Char(':')); colon > 0 && a.count(QLatin1Char(':')) == 1) {
        bool ok = false;
        const int p = a.mid(colon + 1).toInt(&ok);
        if (ok && p > 0 && p < 65536) {
            c.port = quint16(p);
            a = a.left(colon);
        }
    }
    c.address = a;
    c.host = a;
    addComputer(c);
    m_list->setCurrentRow(m_computers.size() - 1);
    for (int i = 0; i < m_computers.size(); ++i)
        if (m_computers[i].address == c.address && m_computers[i].port == c.port)
            m_list->setCurrentRow(i);
    m_address->clear();
}

void SendToDialog::selectComputer()
{
    const int row = m_list->currentRow();
    m_machine->clear();
    m_send->setEnabled(false);
    if (row < 0 || row >= m_computers.size())
        return;
    const Computer c = m_computers[row];
    m_status->setText(QStringLiteral("Asking %1…").arg(c.host));
    const int serial = ++m_infoSerial;
    call(c.address, c.port, QJsonObject{{QStringLiteral("op"), QStringLiteral("info")}}, 8000,
         [this, serial, c](const QJsonObject &r) {
             if (serial != m_infoSerial)
                 return;
             if (!r.value(QStringLiteral("ok")).toBool()) {
                 m_status->setText(QStringLiteral("%1 does not answer: %2. Is phobiCCCP running there, "
                                                  "with File > Receive from Syntexophobic and pads on?")
                                       .arg(c.host, r.value(QStringLiteral("error")).toString()));
                 return;
             }
             for (const QJsonValue &v : r.value(QStringLiteral("machines")).toArray()) {
                 const QJsonObject m = v.toObject();
                 m_machine->addItem(m.value(QStringLiteral("name")).toString().left(60),
                                    m.value(QStringLiteral("id")).toString());
                 if (m.value(QStringLiteral("active")).toBool())
                     m_machine->setCurrentIndex(m_machine->count() - 1);
             }
             const bool mayStart = r.value(QStringLiteral("allowStart")).toBool();
             m_start->setEnabled(mayStart);
             if (!mayStart)
                 m_start->setChecked(false);
             m_start->setToolTip(mayStart ? QString()
                                          : QStringLiteral("Starting from here is off on %1 "
                                                           "(Machine > Pads may start cutting)").arg(c.host));
             const QString doc = r.value(QStringLiteral("document")).toString();
             m_status->setText(doc.isEmpty() ? QString()
                               : r.value(QStringLiteral("unsaved")).toBool()
                                   ? QStringLiteral("%1 has %2 open with unsaved changes; it will ask there first.")
                                         .arg(c.host, doc)
                                   : QStringLiteral("%1 has %2 open.").arg(c.host, doc));
             m_send->setEnabled(!m_busy);
         });
}

void SendToDialog::send()
{
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_computers.size() || m_busy)
        return;
    const Computer c = m_computers[row];
    QString err;
    const QString path = m_makeCopy(&err);
    QFile f(path);
    if (path.isEmpty() || !f.open(QIODevice::ReadOnly)) {
        m_status->setText(QStringLiteral("Could not save a copy to send: %1")
                              .arg(err.isEmpty() ? f.errorString() : err));
        return;
    }
    if (f.size() > kMaxDesign) {
        m_status->setText(QStringLiteral("The design is %1 MB; at most %2 MB can be sent.")
                              .arg(f.size() >> 20).arg(kMaxDesign >> 20));
        return;
    }
    const QByteArray data = f.readAll();
    f.close();
    QFile::remove(path);
    QSettings().setValue(QStringLiteral("sendTo/last"),
                         c.port == ProjectReceiver::kPort ? c.address
                                                          : QStringLiteral("%1:%2").arg(c.address).arg(c.port));
    const QJsonObject req{{QStringLiteral("op"), QStringLiteral("open")},
                          {QStringLiteral("name"), m_name},
                          {QStringLiteral("c2d"), QString::fromLatin1(data.toBase64())},
                          {QStringLiteral("machine"), m_machine->currentData().toString()},
                          {QStringLiteral("start"), m_start->isEnabled() && m_start->isChecked()},
                          {QStringLiteral("from"), QSysInfo::machineHostName()}};
    setBusy(true);
    m_status->setText(QStringLiteral("Sending to %1… (it may ask there first)").arg(c.host));
    // Long: the other side may ask its user to save first.
    call(c.address, c.port, req, 5 * 60 * 1000, [this, c](const QJsonObject &r) {
        setBusy(false);
        if (r.value(QStringLiteral("ok")).toBool()) {
            QString msg = r.value(QStringLiteral("message")).toString();
            if (msg.isEmpty())
                msg = QStringLiteral("opened on %1").arg(c.host);
            m_status->setText(QStringLiteral("Done: %1.").arg(msg));
        } else {
            m_status->setText(QStringLiteral("Not sent: %1").arg(r.value(QStringLiteral("error")).toString()));
        }
    });
}

void SendToDialog::setBusy(bool busy)
{
    m_busy = busy;
    m_send->setEnabled(!busy && m_list->currentRow() >= 0 && m_machine->count() > 0);
    m_list->setEnabled(!busy);
}

void SendToDialog::call(const QString &address, quint16 port, const QJsonObject &req, int timeoutMs,
                        std::function<void(const QJsonObject &)> done)
{
    auto *s = new QTcpSocket(this);
    auto *timer = new QTimer(s);
    auto buf = std::make_shared<QByteArray>();
    auto finished = std::make_shared<bool>(false);
    QPointer<SendToDialog> self(this);
    auto finish = [s, finished, done, self](const QJsonObject &r) {
        if (*finished)
            return;
        *finished = true;
        if (self)
            done(r);
        s->abort();
        s->deleteLater();
    };
    auto fail = [finish](const QString &why) {
        finish(QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), why}});
    };
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, s, [fail] { fail(QStringLiteral("no answer")); });
    connect(s, &QTcpSocket::connected, s, [s, req] {
        s->write(QByteArrayLiteral("PHOBICCCP-API 1\n"));
        s->write(QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n');
    });
    connect(s, &QTcpSocket::readyRead, s, [s, buf, finish, fail] {
        buf->append(s->readAll());
        const qsizetype nl = buf->indexOf('\n');
        if (nl < 0) {
            if (buf->size() > 4 * 1024 * 1024)
                fail(QStringLiteral("answer too long"));
            return;
        }
        const QJsonDocument d = QJsonDocument::fromJson(buf->left(nl));
        if (!d.isObject())
            fail(QStringLiteral("an answer it did not understand: %1")
                     .arg(QString::fromUtf8(buf->left(qMin<qsizetype>(nl, 80)))));
        else
            finish(d.object());
    });
    connect(s, &QTcpSocket::errorOccurred, s, [s, fail](QAbstractSocket::SocketError) {
        fail(s->errorString());
    });
    timer->start(timeoutMs);
    s->connectToHost(address, port);
}

} // namespace c2d
