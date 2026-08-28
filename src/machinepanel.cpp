#include "machinepanel.h"
#include "c2ddocument.h"
#include "gcodeexport.h"
#include "grblstreamer.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

namespace c2d {

MachinePanel::MachinePanel(QWidget *parent)
    : QWidget(parent), m_grbl(new GrblStreamer(this))
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(6);

    // Connection row.
    auto *connRow = new QHBoxLayout;
    m_ports = new QComboBox(this);
    m_ports->setMinimumWidth(110);
    auto *refresh = new QPushButton(QStringLiteral("⟳"), this);
    refresh->setFixedWidth(28);
    refresh->setToolTip(QStringLiteral("Rescan serial ports"));
    m_connectBtn = new QPushButton(QStringLiteral("Connect"), this);
    connRow->addWidget(m_ports, 1);
    connRow->addWidget(refresh);
    connRow->addWidget(m_connectBtn);
    lay->addLayout(connRow);
    connect(refresh, &QPushButton::clicked, this, [this] { refreshPorts(); });
    connect(m_connectBtn, &QPushButton::clicked, this, [this] { toggleConnect(); });

    m_status = new QLabel(QStringLiteral("not connected"), this);
    lay->addWidget(m_status);

    // Jog pad + step size.
    auto *jogRow = new QHBoxLayout;
    auto *grid = new QGridLayout;
    grid->setSpacing(3);
    auto jogBtn = [&](const QString &t, int r, int c, double dx, double dy, double dz) {
        auto *b = new QPushButton(t, this);
        b->setFixedSize(40, 32);
        grid->addWidget(b, r, c);
        connect(b, &QPushButton::clicked, this, [this, dx, dy, dz] { jog(dx, dy, dz); });
    };
    jogBtn(QStringLiteral("Y+"), 0, 1, 0, 1, 0);
    jogBtn(QStringLiteral("X−"), 1, 0, -1, 0, 0);
    jogBtn(QStringLiteral("X+"), 1, 2, 1, 0, 0);
    jogBtn(QStringLiteral("Y−"), 2, 1, 0, -1, 0);
    jogBtn(QStringLiteral("Z+"), 0, 3, 0, 0, 1);
    jogBtn(QStringLiteral("Z−"), 2, 3, 0, 0, -1);
    jogRow->addLayout(grid);

    auto *side = new QVBoxLayout;
    m_step = new QComboBox(this);
    m_step->addItems({QStringLiteral("0.1"), QStringLiteral("1"),
                      QStringLiteral("10"), QStringLiteral("100")});
    m_step->setCurrentIndex(2);
    side->addWidget(new QLabel(QStringLiteral("step (mm)"), this));
    side->addWidget(m_step);
    auto cmdBtn = [&](const QString &t, const QString &cmd, const QString &tip) {
        auto *b = new QPushButton(t, this);
        b->setToolTip(tip);
        side->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, cmd] { m_grbl->sendCommand(cmd); });
    };
    cmdBtn(QStringLiteral("Zero XYZ"), QStringLiteral("G10 L20 P1 X0 Y0 Z0"),
           QStringLiteral("Set current position as work zero"));
    cmdBtn(QStringLiteral("Home"), QStringLiteral("$H"), QStringLiteral("Run homing cycle"));
    cmdBtn(QStringLiteral("Unlock"), QStringLiteral("$X"), QStringLiteral("Clear alarm lock"));
    jogRow->addLayout(side);
    lay->addLayout(jogRow);

    // Air-cut rehearsal: stream the program with all spindle commands stripped
    // and every Z lifted, so the machine traces the job above the stock.
    auto *airRow = new QHBoxLayout;
    m_airCut = new QCheckBox(QStringLiteral("Air cut"), this);
    m_airCut->setToolTip(QStringLiteral(
        "Rehearse: spindle commands removed, every Z raised by the lift amount"));
    m_airLift = new QDoubleSpinBox(this);
    m_airLift->setRange(0.0, 100.0);
    m_airLift->setDecimals(1);
    m_airLift->setValue(10.0);
    m_airLift->setSuffix(QStringLiteral(" mm lift"));
    m_airLift->setEnabled(false);
    connect(m_airCut, &QCheckBox::toggled, m_airLift, &QWidget::setEnabled);
    airRow->addWidget(m_airCut);
    airRow->addWidget(m_airLift);
    airRow->addStretch(1);
    lay->addLayout(airRow);

    // Program controls.
    auto *runRow = new QHBoxLayout;
    m_runBtn = new QPushButton(QStringLiteral("▶ Run"), this);
    m_pauseBtn = new QPushButton(QStringLiteral("⏸ Hold"), this);
    m_stopBtn = new QPushButton(QStringLiteral("⏹ Stop"), this);
    runRow->addWidget(m_runBtn);
    runRow->addWidget(m_pauseBtn);
    runRow->addWidget(m_stopBtn);
    lay->addLayout(runRow);
    connect(m_runBtn, &QPushButton::clicked, this, [this] { runProgram(); });
    connect(m_pauseBtn, &QPushButton::clicked, this, [this] {
        static bool held = false;
        held = !held;
        if (held) { m_grbl->pauseStream(); m_pauseBtn->setText(QStringLiteral("⏵ Resume")); }
        else      { m_grbl->resumeStream(); m_pauseBtn->setText(QStringLiteral("⏸ Hold")); }
    });
    connect(m_stopBtn, &QPushButton::clicked, m_grbl, &GrblStreamer::stopStream);

    // GRBL realtime feed override — takes effect mid-program, no queue flush.
    auto *ovRow = new QHBoxLayout;
    ovRow->addWidget(new QLabel(QStringLiteral("feed ovr"), this));
    auto ovBtn = [&](const QString &t, char code) {
        auto *b = new QPushButton(t, this);
        b->setFixedWidth(52);
        ovRow->addWidget(b);
        connect(b, &QPushButton::clicked, this,
                [this, code] { m_grbl->sendRealtime(code); });
    };
    ovBtn(QStringLiteral("−10%"), char(0x92));
    ovBtn(QStringLiteral("100%"), char(0x90));
    ovBtn(QStringLiteral("+10%"), char(0x91));
    ovRow->addStretch(1);
    lay->addLayout(ovRow);

    m_progress = new QProgressBar(this);
    m_progress->setTextVisible(true);
    lay->addWidget(m_progress);

    m_console = new QPlainTextEdit(this);
    m_console->setReadOnly(true);
    m_console->setMaximumBlockCount(500);
    lay->addWidget(m_console, 1);

    // Streamer feedback.
    connect(m_grbl, &GrblStreamer::connected, this, [this](const QString &p) {
        m_connectBtn->setText(QStringLiteral("Disconnect"));
        m_status->setText(QStringLiteral("connected: %1").arg(p));
        m_grbl->sendCommand(QString());   // wake newline
    });
    connect(m_grbl, &GrblStreamer::disconnected, this, [this] {
        m_connectBtn->setText(QStringLiteral("Connect"));
        m_status->setText(QStringLiteral("not connected"));
    });
    connect(m_grbl, &GrblStreamer::statusReport, this,
            [this](const QString &st, double x, double y, double z) {
        m_status->setText(QStringLiteral("%1   X%2 Y%3 Z%4")
                              .arg(st).arg(x, 0, 'f', 2).arg(y, 0, 'f', 2).arg(z, 0, 'f', 2));
    });
    connect(m_grbl, &GrblStreamer::progressChanged, this, [this](int a, int t) {
        m_progress->setMaximum(qMax(1, t));
        m_progress->setValue(a);
    });
    connect(m_grbl, &GrblStreamer::consoleLine, this, [this](const QString &l) {
        m_console->appendPlainText(l);
    });
    connect(m_grbl, &GrblStreamer::errorOccurred, this, [this](const QString &e) {
        m_console->appendPlainText(QStringLiteral("!! %1").arg(e));
    });
    connect(m_grbl, &GrblStreamer::streamFinished, this, [this](bool ok) {
        m_console->appendPlainText(ok ? QStringLiteral("== program finished ==")
                                      : QStringLiteral("== program stopped =="));
    });

    refreshPorts();
}

void MachinePanel::refreshPorts()
{
    const QString keep = m_ports->currentText();
    m_ports->clear();
    m_ports->addItems(GrblStreamer::availablePorts());
    const int i = m_ports->findText(keep);
    if (i >= 0)
        m_ports->setCurrentIndex(i);
}

void MachinePanel::toggleConnect()
{
    if (m_grbl->isConnected()) {
        m_grbl->disconnectPort();
        return;
    }
    if (m_ports->currentText().isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Machine"),
                                 QStringLiteral("No serial port found. Plug in the "
                                                "controller and rescan."));
        return;
    }
    m_grbl->connectPort(m_ports->currentText());
}

void MachinePanel::jog(double dx, double dy, double dz)
{
    const double s = m_step->currentText().toDouble();
    const double f = dz != 0 ? 500 : 1500;
    QStringList words;
    if (dx != 0) words << QStringLiteral("X%1").arg(dx * s);
    if (dy != 0) words << QStringLiteral("Y%1").arg(dy * s);
    if (dz != 0) words << QStringLiteral("Z%1").arg(dz * s);
    m_grbl->sendCommand(QStringLiteral("$J=G91 %1 F%2").arg(words.join(QChar(' '))).arg(f));
}

void MachinePanel::runProgram()
{
    if (!m_grbl->isConnected()) {
        QMessageBox::information(this, QStringLiteral("Machine"),
                                 QStringLiteral("Connect to the controller first."));
        return;
    }
    if (!m_doc) {
        QMessageBox::information(this, QStringLiteral("Machine"),
                                 QStringLiteral("Open a .c2d file first."));
        return;
    }
    const GcodeResult r = exportGcode(*m_doc);
    if (r.done.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Machine"),
                                 QStringLiteral("No exportable toolpaths."));
        return;
    }
    QStringList lines = r.gcode.split(QChar('\n'), Qt::SkipEmptyParts);
    const bool air = m_airCut->isChecked();
    if (air)
        lines = airCutTransform(lines, m_airLift->value());

    // Confirmation gate: extents + time estimate, and an unmissable note about
    // whether this run will spin the spindle.
    QString q = QStringLiteral("%1 toolpath(s), %2 lines\n%3")
                    .arg(r.done.size()).arg(lines.size())
                    .arg(statsSummary(computeStats(r.ops)));
    if (air)
        q += QStringLiteral("\n\nAIR CUT: spindle stripped, all Z +%1 mm")
                 .arg(m_airLift->value(), 0, 'f', 1);
    else
        q += QStringLiteral("\n\nLIVE RUN: the spindle WILL start.");
    q += QStringLiteral("\n\nStream to the machine?");
    if (QMessageBox::question(this, QStringLiteral("Run program"), q,
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes)
        return;

    // `M0` pauses GRBL until cycle-start — surface tool changes in the console.
    const int npause = int(lines.filter(QStringLiteral("M0 ;")).size());
    if (npause > 0)
        m_console->appendPlainText(
            QStringLiteral("note: %1 tool-change pause(s); press Resume after each")
                .arg(npause));
    if (air)
        m_console->appendPlainText(
            QStringLiteral("air cut: spindle off, Z lifted %1 mm")
                .arg(m_airLift->value(), 0, 'f', 1));
    m_grbl->startStream(lines);
}

} // namespace c2d
