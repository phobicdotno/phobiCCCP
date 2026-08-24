#include "machinedialog.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QVBoxLayout>

namespace c2d {

MachineDialog::MachineDialog(const QString &gcode, QWidget *parent)
    : QDialog(parent), m_gcode(gcode),
      m_sender([this](const QByteArray &bytes) {
          if (m_port)
              m_port->write(bytes);
      })
{
    setWindowTitle(QStringLiteral("Send to GRBL"));
    resize(560, 420);

    auto *layout = new QVBoxLayout(this);

    auto *portRow = new QHBoxLayout;
    m_ports = new QComboBox(this);
    auto *refreshBtn = new QPushButton(QStringLiteral("Refresh"), this);
    m_connectBtn = new QPushButton(QStringLiteral("Connect"), this);
    portRow->addWidget(new QLabel(QStringLiteral("Port:"), this));
    portRow->addWidget(m_ports, 1);
    portRow->addWidget(refreshBtn);
    portRow->addWidget(m_connectBtn);
    layout->addLayout(portRow);

    auto *runRow = new QHBoxLayout;
    m_startBtn = new QPushButton(QStringLiteral("Start"), this);
    m_abortBtn = new QPushButton(QStringLiteral("Abort"), this);
    m_progress = new QProgressBar(this);
    runRow->addWidget(m_startBtn);
    runRow->addWidget(m_abortBtn);
    runRow->addWidget(m_progress, 1);
    layout->addLayout(runRow);

    m_status = new QLabel(QStringLiteral("Not connected"), this);
    layout->addWidget(m_status);

    m_console = new QPlainTextEdit(this);
    m_console->setReadOnly(true);
    m_console->setMaximumBlockCount(2000);
    layout->addWidget(m_console, 1);

    connect(refreshBtn, &QPushButton::clicked, this, &MachineDialog::refreshPorts);
    connect(m_connectBtn, &QPushButton::clicked, this, &MachineDialog::onConnect);
    connect(m_startBtn, &QPushButton::clicked, this, &MachineDialog::onStart);
    connect(m_abortBtn, &QPushButton::clicked, this, &MachineDialog::onAbort);

    refreshPorts();
    updateUi();
}

void MachineDialog::refreshPorts()
{
    m_ports->clear();
    const auto ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &info : ports)
        m_ports->addItem(QStringLiteral("%1  (%2)").arg(info.portName(),
                                                        info.description()),
                         info.portName());
    if (m_ports->count() == 0)
        m_ports->addItem(QStringLiteral("no serial ports found"), QString());
}

void MachineDialog::onConnect()
{
    if (m_port) {   // disconnect
        m_sender.abort();
        m_port->close();
        m_port->deleteLater();
        m_port = nullptr;
        log(QStringLiteral("Disconnected"));
        updateUi();
        return;
    }
    const QString name = m_ports->currentData().toString();
    if (name.isEmpty())
        return;
    m_port = new QSerialPort(name, this);
    m_port->setBaudRate(115200);
    if (!m_port->open(QIODevice::ReadWrite)) {
        log(QStringLiteral("Open failed: %1").arg(m_port->errorString()));
        m_port->deleteLater();
        m_port = nullptr;
        updateUi();
        return;
    }
    connect(m_port, &QSerialPort::readyRead, this, &MachineDialog::onReadyRead);
    log(QStringLiteral("Connected to %1 @ 115200").arg(name));
    updateUi();
}

void MachineDialog::onStart()
{
    if (!m_port || m_sender.active())
        return;
    const QStringList lines = m_gcode.split(QChar('\n'), Qt::SkipEmptyParts);
    log(QStringLiteral("Streaming %1 lines").arg(lines.size()));
    m_progress->setRange(0, lines.size());
    m_progress->setValue(0);
    m_sender.start(lines);
    updateUi();
}

void MachineDialog::onAbort()
{
    if (!m_port)
        return;
    m_sender.abort();
    m_port->write(QByteArray(1, char(0x18)));   // GRBL soft reset: stop now
    log(QStringLiteral("Aborted (soft reset sent)"));
    updateUi();
}

void MachineDialog::onReadyRead()
{
    if (!m_port)
        return;
    const QByteArray data = m_port->readAll();
    // Echo everything GRBL says except the plain ok acks: banners, ALARM and
    // error lines explain a stalled or failed stream. (A line split across
    // read chunks may log in two pieces; cosmetic only.)
    const QList<QByteArray> rxLines = data.split('\n');
    for (const QByteArray &l : rxLines) {
        const QByteArray t = l.trimmed();
        if (!t.isEmpty() && t != "ok")
            log(QString::fromLatin1(t));
    }
    m_sender.onData(data);
    m_progress->setValue(m_sender.acked());
    if (m_sender.errors() > 0)
        m_status->setText(QStringLiteral("%1 errors, last: %2")
                              .arg(m_sender.errors())
                              .arg(m_sender.lastError()));
    if (m_sender.finished())
        log(QStringLiteral("Done: %1/%2 lines acknowledged")
                .arg(m_sender.acked())
                .arg(m_sender.total()));
    updateUi();
}

void MachineDialog::log(const QString &line)
{
    m_console->appendPlainText(line);
}

void MachineDialog::updateUi()
{
    const bool connected = m_port != nullptr;
    m_connectBtn->setText(connected ? QStringLiteral("Disconnect")
                                    : QStringLiteral("Connect"));
    m_startBtn->setEnabled(connected && !m_sender.active());
    m_abortBtn->setEnabled(connected && m_sender.active());
    if (!connected)
        m_status->setText(QStringLiteral("Not connected"));
    else if (m_sender.active())
        m_status->setText(QStringLiteral("Streaming %1/%2")
                              .arg(m_sender.acked())
                              .arg(m_sender.total()));
    else
        m_status->setText(QStringLiteral("Connected, idle"));
}

} // namespace c2d
