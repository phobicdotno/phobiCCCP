#include "machinepanel.h"

#include "gamepad.h"

#include "exportprogress.h"
#include "c2ddocument.h"
#include "gcodeexport.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QFocusEvent>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

namespace c2d {

static QDoubleSpinBox *mmSpin(QWidget *parent, double lo, double hi, double val,
                              int decimals = 2, const QString &suffix = QStringLiteral(" mm"))
{
    auto *s = new QDoubleSpinBox(parent);
    s->setRange(lo, hi);
    s->setDecimals(decimals);
    s->setValue(val);
    s->setSuffix(suffix);
    s->setKeyboardTracking(false);
    return s;
}

MachinePanel::MachinePanel(QWidget *parent)
    : QWidget(parent), m_grbl(new GrblStreamer(this)), m_holdTimer(new QTimer(this)),
      m_holdRepeat(new QTimer(this))
{
    setFocusPolicy(Qt::StrongFocus);
    m_holdTimer->setSingleShot(true);
    m_holdTimer->setInterval(350);
    m_holdRepeat->setInterval(120);

    // The dock is narrow and this panel is tall: scroll the controls, keep the
    // console pinned at the bottom.
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *body = new QWidget(scroll);
    scroll->setWidget(body);
    outer->addWidget(scroll, 3);
    auto *lay = new QVBoxLayout(body);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(6);

    // ---- Connection --------------------------------------------------------
    auto *connRow = new QHBoxLayout;
    m_ports = new QComboBox(body);
    m_ports->setEditable(true);            // type /dev/ttyACM0 or a pty by hand
    m_ports->setMinimumWidth(110);
    m_ports->setToolTip(QStringLiteral("Serial port of the GRBL controller. "
                                       "Pick one, or type a device path."));
    auto *refresh = new QPushButton(QStringLiteral("⟳"), body);
    refresh->setFixedWidth(28);
    refresh->setToolTip(QStringLiteral("Rescan serial ports"));
    m_connectBtn = new QPushButton(QStringLiteral("Connect"), body);
    connRow->addWidget(m_ports, 1);
    connRow->addWidget(refresh);
    connRow->addWidget(m_connectBtn);
    lay->addLayout(connRow);
    connect(refresh, &QPushButton::clicked, this, [this] { refreshPorts(); });
    connect(m_connectBtn, &QPushButton::clicked, this, [this] { toggleConnect(); });

    // ---- Status (state, work + machine position) --------------------------
    auto *stGrid = new QGridLayout;
    stGrid->setHorizontalSpacing(10);
    m_state = new QLabel(QStringLiteral("not connected"), body);
    QFont bold = m_state->font();
    bold.setBold(true);
    m_state->setFont(bold);
    m_wpos = new QLabel(QStringLiteral("—"), body);
    QFont mono(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::Monospace);
    mono.setPointSizeF(mono.pointSizeF() + 2);
    m_wpos->setFont(mono);
    m_mpos = new QLabel(QStringLiteral("—"), body);
    m_extra = new QLabel(QString(), body);
    m_mpos->setStyleSheet(QStringLiteral("color: palette(mid);"));
    m_extra->setStyleSheet(QStringLiteral("color: palette(mid);"));
    stGrid->addWidget(m_state, 0, 0, 1, 2);
    stGrid->addWidget(new QLabel(QStringLiteral("work"), body), 1, 0);
    stGrid->addWidget(m_wpos, 1, 1);
    stGrid->addWidget(new QLabel(QStringLiteral("machine"), body), 2, 0);
    stGrid->addWidget(m_mpos, 2, 1);
    stGrid->addWidget(m_extra, 3, 0, 1, 2);
    stGrid->setColumnStretch(1, 1);
    lay->addLayout(stGrid);

    // ---- Jog ---------------------------------------------------------------
    auto *jogBox = new QGroupBox(QStringLiteral("Jog  (click = step, hold = continuous, "
                                                "arrows / PgUp / PgDn)"), body);
    auto *jogRow = new QHBoxLayout(jogBox);
    auto *grid = new QGridLayout;
    grid->setSpacing(3);
    grid->addWidget(jogButton(QStringLiteral("Y+"), 'Y', +1), 0, 1);
    grid->addWidget(jogButton(QStringLiteral("X−"), 'X', -1), 1, 0);
    grid->addWidget(jogButton(QStringLiteral("X+"), 'X', +1), 1, 2);
    grid->addWidget(jogButton(QStringLiteral("Y−"), 'Y', -1), 2, 1);
    grid->addWidget(jogButton(QStringLiteral("Z+"), 'Z', +1), 0, 3);
    grid->addWidget(jogButton(QStringLiteral("Z−"), 'Z', -1), 2, 3);
    auto *stopJog = new QPushButton(QStringLiteral("■"), jogBox);
    stopJog->setFixedSize(40, 32);
    stopJog->setToolTip(QStringLiteral("Cancel jog"));
    connect(stopJog, &QPushButton::clicked, m_grbl, &GrblStreamer::jogCancel);
    grid->addWidget(stopJog, 1, 1);
    jogRow->addLayout(grid);

    auto *side = new QFormLayout;
    side->setContentsMargins(6, 0, 0, 0);
    m_step = new QComboBox(jogBox);
    m_step->addItems({QStringLiteral("0.1"), QStringLiteral("1"),
                      QStringLiteral("10"), QStringLiteral("100")});
    m_step->setCurrentIndex(2);
    side->addRow(QStringLiteral("step mm"), m_step);
    m_speed = new QComboBox(jogBox);
    m_speed->addItem(QStringLiteral("slow  200"), 200);
    m_speed->addItem(QStringLiteral("medium  1000"), 1000);
    m_speed->addItem(QStringLiteral("fast  3000"), 3000);
    m_speed->addItem(QStringLiteral("rapid  6000"), 6000);
    m_speed->setCurrentIndex(1);
    side->addRow(QStringLiteral("mm/min"), m_speed);
    // Says which pad is driving the jog, and stays out of the way when there
    // is none. Button numbers are logged to the console on every press so a
    // clone with a different layout can be remapped without guessing.
    m_padLabel = new QLabel(QStringLiteral("gamepad: none"), jogBox);
    m_padLabel->setEnabled(false);
    side->addRow(QString(), m_padLabel);
    jogRow->addLayout(side);
    lay->addWidget(jogBox);

    // ---- Work zero ---------------------------------------------------------
    auto *zeroBox = new QGroupBox(QStringLiteral("Work zero"), body);
    auto *zg = new QGridLayout(zeroBox);
    zg->setSpacing(3);
    auto zeroBtn = [&](const QString &t, const QString &axes, int r, int c, const QString &tip) {
        auto *b = new QPushButton(t, zeroBox);
        b->setToolTip(tip);
        zg->addWidget(b, r, c);
        connect(b, &QPushButton::clicked, this, [this, axes] { zero(axes); });
    };
    zeroBtn(QStringLiteral("Zero X"), QStringLiteral("X"), 0, 0,
            QStringLiteral("Current X becomes work X0  (G10 L20)"));
    zeroBtn(QStringLiteral("Zero Y"), QStringLiteral("Y"), 0, 1,
            QStringLiteral("Current Y becomes work Y0"));
    zeroBtn(QStringLiteral("Zero Z"), QStringLiteral("Z"), 0, 2,
            QStringLiteral("Current Z becomes work Z0. With the BitSetter enabled the "
                           "tool is measured afterwards as the reference tool."));
    zeroBtn(QStringLiteral("Zero XY"), QStringLiteral("XY"), 1, 0,
            QStringLiteral("Current X and Y become work X0 Y0"));
    zeroBtn(QStringLiteral("Zero all"), QStringLiteral("XYZ"), 1, 1,
            QStringLiteral("Current position becomes work X0 Y0 Z0"));
    auto *gotoBtn = new QPushButton(QStringLiteral("→ X0 Y0"), zeroBox);
    gotoBtn->setToolTip(QStringLiteral("Raise to safe Z, then rapid to work X0 Y0"));
    zg->addWidget(gotoBtn, 1, 2);
    connect(gotoBtn, &QPushButton::clicked, this, [this] { goToXY0(); });
    auto *homeBtn = new QPushButton(QStringLiteral("Home"), zeroBox);
    homeBtn->setToolTip(QStringLiteral("Run homing cycle ($H)"));
    zg->addWidget(homeBtn, 2, 0);
    connect(homeBtn, &QPushButton::clicked, this,
            [this] { m_grbl->sendCommand(QStringLiteral("$H")); });
    auto *unlockBtn = new QPushButton(QStringLiteral("Unlock"), zeroBox);
    unlockBtn->setToolTip(QStringLiteral("Clear alarm lock ($X)"));
    zg->addWidget(unlockBtn, 2, 1);
    connect(unlockBtn, &QPushButton::clicked, this,
            [this] { m_grbl->sendCommand(QStringLiteral("$X")); });
    lay->addWidget(zeroBox);

    // ---- BitSetter + tool change -------------------------------------------
    m_bsGroup = new QGroupBox(QStringLiteral("BitSetter (tool length probe)"), body);
    m_bsGroup->setCheckable(true);
    m_bsGroup->setChecked(false);
    m_bsGroup->setToolTip(QStringLiteral(
        "Positions are MACHINE coordinates (G53) — they stay valid when work zero moves."));
    auto *bf = new QFormLayout(m_bsGroup);
    bf->setContentsMargins(6, 4, 6, 4);
    bf->setVerticalSpacing(3);
    auto *bsPos = new QHBoxLayout;
    m_bsX = mmSpin(m_bsGroup, -5000, 5000, -20, 3, QString());
    m_bsY = mmSpin(m_bsGroup, -5000, 5000, -20, 3, QString());
    auto *bsUse = new QPushButton(QStringLiteral("use current"), m_bsGroup);
    bsUse->setToolTip(QStringLiteral("Jog the bit over the BitSetter button, then click"));
    bsPos->addWidget(new QLabel(QStringLiteral("X"), m_bsGroup));
    bsPos->addWidget(m_bsX, 1);
    bsPos->addWidget(new QLabel(QStringLiteral("Y"), m_bsGroup));
    bsPos->addWidget(m_bsY, 1);
    bsPos->addWidget(bsUse);
    bf->addRow(QStringLiteral("button"), bsPos);
    connect(bsUse, &QPushButton::clicked, this, [this] {
        const MachineStatus &st = m_grbl->status();
        if (!st.valid) return;
        m_bsX->setValue(st.mx);
        m_bsY->setValue(st.my);
        saveSettings();
    });
    m_bsSafeZ = mmSpin(m_bsGroup, -1000, 0, -5, 2);
    m_bsSafeZ->setToolTip(QStringLiteral("Machine Z used for all travel to / from the button"));
    bf->addRow(QStringLiteral("safe Z (machine)"), m_bsSafeZ);
    auto *feeds = new QHBoxLayout;
    m_bsFast = mmSpin(m_bsGroup, 10, 3000, 600, 0, QStringLiteral(" fast"));
    m_bsSlow = mmSpin(m_bsGroup, 5, 500, 50, 0, QStringLiteral(" slow"));
    feeds->addWidget(m_bsFast);
    feeds->addWidget(m_bsSlow);
    bf->addRow(QStringLiteral("probe mm/min"), feeds);

    auto *tcPos = new QHBoxLayout;
    m_tcX = mmSpin(m_bsGroup, -5000, 5000, -120, 3, QString());
    m_tcY = mmSpin(m_bsGroup, -5000, 5000, -20, 3, QString());
    auto *tcUse = new QPushButton(QStringLiteral("use current"), m_bsGroup);
    tcPos->addWidget(new QLabel(QStringLiteral("X"), m_bsGroup));
    tcPos->addWidget(m_tcX, 1);
    tcPos->addWidget(new QLabel(QStringLiteral("Y"), m_bsGroup));
    tcPos->addWidget(m_tcY, 1);
    tcPos->addWidget(tcUse);
    bf->addRow(QStringLiteral("tool-change spot"), tcPos);
    connect(tcUse, &QPushButton::clicked, this, [this] {
        const MachineStatus &st = m_grbl->status();
        if (!st.valid) return;
        m_tcX->setValue(st.mx);
        m_tcY->setValue(st.my);
        saveSettings();
    });

    auto *bsBtns = new QHBoxLayout;
    m_measureBtn = new QPushButton(QStringLiteral("Measure tool"), m_bsGroup);
    m_measureBtn->setToolTip(QStringLiteral(
        "Go to the button, probe twice, come back. The first measurement after "
        "zeroing Z is the reference; later ones set the G43.1 length offset."));
    auto *clearRef = new QPushButton(QStringLiteral("Clear reference"), m_bsGroup);
    bsBtns->addWidget(m_measureBtn);
    bsBtns->addWidget(clearRef);
    bf->addRow(bsBtns);
    connect(m_measureBtn, &QPushButton::clicked, this, [this] { measureTool(Phase::MeasureManual); });
    connect(clearRef, &QPushButton::clicked, this, [this] {
        m_haveRef = false;
        m_grbl->applyToolLengthOffset(0);
        saveSettings();
        updateOffsetLabels();
    });
    m_refLabel = new QLabel(m_bsGroup);
    m_tloLabel = new QLabel(m_bsGroup);
    bf->addRow(QStringLiteral("reference"), m_refLabel);
    bf->addRow(QStringLiteral("offset"), m_tloLabel);
    lay->addWidget(m_bsGroup);
    for (QDoubleSpinBox *s : {m_bsX, m_bsY, m_bsSafeZ, m_bsFast, m_bsSlow, m_tcX, m_tcY})
        connect(s, &QDoubleSpinBox::valueChanged, this, [this] { saveSettings(); });
    connect(m_bsGroup, &QGroupBox::toggled, this, [this] { saveSettings(); });

    // ---- Program -----------------------------------------------------------
    auto *airRow = new QHBoxLayout;
    m_airCut = new QCheckBox(QStringLiteral("Air cut"), body);
    m_airCut->setToolTip(QStringLiteral(
        "Rehearse: spindle commands removed, every Z raised by the lift amount"));
    m_airLift = mmSpin(body, 0, 100, 10, 1, QStringLiteral(" mm lift"));
    m_airLift->setEnabled(false);
    connect(m_airCut, &QCheckBox::toggled, m_airLift, &QWidget::setEnabled);
    airRow->addWidget(m_airCut);
    airRow->addWidget(m_airLift);
    airRow->addStretch(1);
    lay->addLayout(airRow);

    auto *runRow = new QHBoxLayout;
    m_runBtn = new QPushButton(QStringLiteral("▶ Run"), body);
    m_pauseBtn = new QPushButton(QStringLiteral("⏸ Hold"), body);
    m_stopBtn = new QPushButton(QStringLiteral("⏹ Stop"), body);
    runRow->addWidget(m_runBtn);
    runRow->addWidget(m_pauseBtn);
    runRow->addWidget(m_stopBtn);
    lay->addLayout(runRow);
    connect(m_runBtn, &QPushButton::clicked, this, [this] {
        if (!m_grbl->isParkedForTool()) {
            runProgram();
            return;
        }
        // Continuing without a successful measurement resumes with the
        // *previous* tool's G43.1 still applied: a tool 10 mm longer then
        // cuts 10 mm deeper on its first plunge. The message after a failed
        // probe points at this very button, so say it plainly here.
        if (!m_toolMeasured
            && QMessageBox::warning(
                   this, QStringLiteral("Tool not measured"),
                   QStringLiteral("T%1 has not been measured on the BitSetter.\n\n"
                                  "Continuing keeps the previous tool's length offset, "
                                  "so the program will cut as deep as the two tools "
                                  "differ in length.")
                       .arg(m_pendingTool),
                   QMessageBox::Cancel | QMessageBox::Ignore, QMessageBox::Cancel)
                   != QMessageBox::Ignore)
            return;
        m_grbl->continueAfterToolChange();
    });
    connect(m_pauseBtn, &QPushButton::clicked, this, [this] {
        m_held = !m_held;
        if (m_held) { m_grbl->pauseStream(); m_pauseBtn->setText(QStringLiteral("⏵ Resume")); }
        else        { m_grbl->resumeStream(); m_pauseBtn->setText(QStringLiteral("⏸ Hold")); }
    });
    connect(m_stopBtn, &QPushButton::clicked, this, [this] {
        resetFlow();
        m_grbl->stopStream();
    });

    // GRBL realtime feed override — takes effect mid-program, no queue flush.
    auto *ovRow = new QHBoxLayout;
    ovRow->addWidget(new QLabel(QStringLiteral("feed ovr"), body));
    auto ovBtn = [&](const QString &t, char code) {
        auto *b = new QPushButton(t, body);
        b->setFixedWidth(52);
        ovRow->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, code] { m_grbl->sendRealtime(code); });
    };
    ovBtn(QStringLiteral("−10%"), char(0x92));
    ovBtn(QStringLiteral("100%"), char(0x90));
    ovBtn(QStringLiteral("+10%"), char(0x91));
    ovRow->addStretch(1);
    lay->addLayout(ovRow);

    m_progress = new QProgressBar(body);
    m_progress->setTextVisible(true);
    lay->addWidget(m_progress);
    lay->addStretch(1);

    m_console = new QPlainTextEdit(this);
    m_console->setReadOnly(true);
    m_console->setMaximumBlockCount(800);
    outer->addWidget(m_console, 2);

    // ---- Streamer feedback -----------------------------------------------
    connect(m_grbl, &GrblStreamer::connected, this, [this](const QString &p) {
        m_connectBtn->setText(QStringLiteral("Disconnect"));
        m_state->setText(QStringLiteral("connected: %1").arg(p));
        m_grbl->sendCommand(QString());   // wake newline
        saveSettings();
    });
    connect(m_grbl, &GrblStreamer::disconnected, this, [this] {
        resetFlow();
        stopHoldJog();
        m_connectBtn->setText(QStringLiteral("Connect"));
        m_state->setText(QStringLiteral("not connected"));
        m_wpos->setText(QStringLiteral("—"));
        m_mpos->setText(QStringLiteral("—"));
        m_extra->clear();
        emit livePosition(0, 0, 0, false);
    });
    connect(m_grbl, &GrblStreamer::statusChanged, this, [this](const MachineStatus &st) { onStatus(st); });
    connect(m_grbl, &GrblStreamer::progressChanged, this, [this](int a, int t) {
        m_progress->setMaximum(qMax(1, t));
        m_progress->setValue(a);
    });
    connect(m_grbl, &GrblStreamer::consoleLine, this, [this](const QString &l) { log(l); });
    connect(m_grbl, &GrblStreamer::errorOccurred, this, [this](const QString &e) {
        log(QStringLiteral("!! %1").arg(e));
    });
    connect(m_grbl, &GrblStreamer::probeResult, this,
            [this](double x, double y, double z, bool ok) {
        log(QStringLiteral("probe %1 at machine X%2 Y%3 Z%4")
                .arg(ok ? QStringLiteral("contact") : QStringLiteral("FAILED"))
                .arg(x, 0, 'f', 3).arg(y, 0, 'f', 3).arg(z, 0, 'f', 3));
    });
    connect(m_grbl, &GrblStreamer::macroFinished, this, [this](bool ok) { onMacroFinished(ok); });
    connect(m_grbl, &GrblStreamer::toolChangeRequested, this, [this](int t) { onToolChange(t); });
    connect(m_grbl, &GrblStreamer::streamFinished, this, [this](bool ok) {
        log(ok ? QStringLiteral("== program finished ==")
               : QStringLiteral("== program stopped =="));
        resetFlow();
    });

    // Press-and-hold: after the hold delay a step-click turns into a
    // continuous jog. Rather than one long $J= that relies on the release
    // event for its cancel, feed GRBL a short increment every tick (each
    // worth ~2 ticks of travel, so the planner never runs dry); if the
    // release is ever lost, motion stops on its own within a fraction of a
    // second. Release still sends jog-cancel to stop instantly.
    connect(m_holdTimer, &QTimer::timeout, this, [this] {
        if (m_holdAxis) {
            m_holdJogging = true;
            m_holdRepeat->start();
            jogIncrement();
        }
    });
    connect(m_holdRepeat, &QTimer::timeout, this, [this] { jogIncrement(); });

    loadSettings();
    loadGamepadMapping();
    updateOffsetLabels();
    refreshPorts();

    // A USB pad is a much better way to drive a machine than the arrow keys:
    // it works from wherever you are standing, without the window focused.
    m_pad = new Gamepad(this);
    connect(m_pad, &Gamepad::connected, this, [this](const QString &name) {
        m_padLabel->setText(QStringLiteral("gamepad: %1").arg(name));
        m_padLabel->setEnabled(true);
        log(QStringLiteral("gamepad connected: %1").arg(name));
    });
    connect(m_pad, &Gamepad::disconnected, this, [this] {
        m_padLabel->setText(QStringLiteral("gamepad: none"));
        m_padLabel->setEnabled(false);
        endHoldJog();                     // a release we will never see
        m_padAxisDir[0] = m_padAxisDir[1] = 0;
        log(QStringLiteral("gamepad disconnected"));
    });
    connect(m_pad, &Gamepad::buttonChanged, this, &MachinePanel::gamepadButton);
    connect(m_pad, &Gamepad::axisChanged, this, &MachinePanel::gamepadAxis);
    m_pad->start();
}

MachinePanel::~MachinePanel()
{
    disconnect(m_grbl, nullptr, this, nullptr);
}

// ---- small helpers -----------------------------------------------------------

void MachinePanel::log(const QString &line)
{
    m_console->appendPlainText(line);
}

QPushButton *MachinePanel::jogButton(const QString &text, char axis, int dir)
{
    auto *b = new QPushButton(text, this);
    b->setFixedSize(40, 32);
    b->setFocusPolicy(Qt::NoFocus);          // keep keyboard jogging on the panel
    connect(b, &QPushButton::pressed, this, [this, axis, dir] {
        setFocus();                       // arrow keys jog from here on
        stopHoldJog();
        m_holdAxis = axis;
        m_holdDir = dir;
        m_holdJogging = false;
        m_holdTimer->start();
    });
    connect(b, &QPushButton::released, this, [this] {
        m_holdTimer->stop();
        m_holdRepeat->stop();
        if (m_holdJogging)
            m_grbl->jogCancel();
        else if (m_holdAxis)
            jogStep(m_holdAxis, m_holdDir);
        m_holdAxis = 0;
        m_holdJogging = false;
    });
    return b;
}

double MachinePanel::jogFeed(char axis) const
{
    const double f = m_speed->currentData().toDouble();
    return axis == 'Z' ? qMin(f, 1500.0) : f;
}

void MachinePanel::jogIncrement()
{
    if (!m_holdAxis || !m_grbl->canSendCommand())
        return;
    // Each tick deliberately asks for twice the travel of the interval, so the
    // motion never gaps. With nothing bounding it, holding a jog button for a
    // few seconds queues blocks faster than the machine retires them: GRBL's
    // planner fills, its acks stop coming, and we keep writing 25-byte lines
    // into a 128-byte RX buffer. Losing one character of `$J=G91 X-24.000` -
    // the minus, say - is a full-speed jog the wrong way. Two unanswered jogs
    // is enough to keep the motion continuous.
    if (m_grbl->pendingCommands() >= 2)
        return;
    const double f = jogFeed(m_holdAxis);
    const double mm = f / 60.0 * (2.0 * m_holdRepeat->interval() / 1000.0);
    m_grbl->sendCommand(QStringLiteral("$J=G91 %1%2 F%3")
                            .arg(QChar::fromLatin1(m_holdAxis)).arg(m_holdDir * mm, 0, 'f', 3).arg(f));
}

void MachinePanel::jogStep(char axis, int dir)
{
    const double s = m_step->currentText().toDouble();
    m_grbl->sendCommand(QStringLiteral("$J=G91 %1%2 F%3")
                            .arg(QChar::fromLatin1(axis)).arg(dir * s).arg(jogFeed(axis)));
}

void MachinePanel::keyPressEvent(QKeyEvent *event)
{
    char axis = 0;
    int dir = 0;
    switch (event->key()) {
    case Qt::Key_Left:     axis = 'X'; dir = -1; break;
    case Qt::Key_Right:    axis = 'X'; dir = +1; break;
    case Qt::Key_Up:       axis = 'Y'; dir = +1; break;
    case Qt::Key_Down:     axis = 'Y'; dir = -1; break;
    case Qt::Key_PageUp:   axis = 'Z'; dir = +1; break;
    case Qt::Key_PageDown: axis = 'Z'; dir = -1; break;
    default: QWidget::keyPressEvent(event); return;
    }
    if (event->isAutoRepeat())
        return;
    beginHoldJog(axis, dir);
}

// Keys, buttons and the gamepad all jog the same way: a tap shorter than the
// hold delay becomes one step, holding becomes continuous motion.
void MachinePanel::beginHoldJog(char axis, int dir)
{
    stopHoldJog();
    m_holdAxis = axis;
    m_holdDir = dir;
    m_holdJogging = false;
    m_holdTimer->start();
}

void MachinePanel::endHoldJog()
{
    m_holdTimer->stop();
    m_holdRepeat->stop();
    if (m_holdJogging)
        m_grbl->jogCancel();
    else if (m_holdAxis)
        jogStep(m_holdAxis, m_holdDir);
    m_holdAxis = 0;
    m_holdJogging = false;
}

void MachinePanel::keyReleaseEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Left: case Qt::Key_Right: case Qt::Key_Up: case Qt::Key_Down:
    case Qt::Key_PageUp: case Qt::Key_PageDown:
        break;
    default: QWidget::keyReleaseEvent(event); return;
    }
    if (event->isAutoRepeat())
        return;
    endHoldJog();
}

QString MachinePanel::currentPortName() const
{
    // Items carry the bare port name as data and a description as text; a
    // path typed by hand has no item.
    const int i = m_ports->currentIndex();
    if (i >= 0 && m_ports->itemText(i) == m_ports->currentText())
        return m_ports->itemData(i).toString();
    return m_ports->currentText().trimmed();
}

void MachinePanel::stopHoldJog()
{
    m_holdTimer->stop();
    m_holdRepeat->stop();
    if (m_holdJogging)
        m_grbl->jogCancel();
    m_holdAxis = 0;
    m_holdJogging = false;
}

void MachinePanel::focusOutEvent(QFocusEvent *event)
{
    stopHoldJog();                        // a release we will never see
    QWidget::focusOutEvent(event);
}

void MachinePanel::hideEvent(QHideEvent *event)
{
    stopHoldJog();
    QWidget::hideEvent(event);
}

void MachinePanel::resetFlow()
{
    m_phase = Phase::Idle;
    m_pendingTool = -1;
    m_toolMeasured = false;
    m_onIdle = nullptr;
    m_held = false;
    m_pauseBtn->setText(QStringLiteral("⏸ Hold"));
    m_runBtn->setText(QStringLiteral("▶ Run"));
}

// ---- gamepad -----------------------------------------------------------------
//
// Default mapping for a SNES-style pad. Clones number their buttons
// differently, so every press is logged with its number and any of these can be
// overridden in QSettings under "gamepad/<number>" with one of the action
// names below. Program start is deliberately NOT on the pad: a bumped button
// must never begin a cut. Hold and Stop are, because those are the ones worth
// being able to hit without looking.
void MachinePanel::loadGamepadMapping()
{
    m_padMap = {
        {4, QStringLiteral("z-")},        // L
        {5, QStringLiteral("z+")},        // R
        {8, QStringLiteral("step")},      // Select: cycle the jog step
        {9, QStringLiteral("hold")},      // Start:  hold / resume
        {1, QStringLiteral("stop")},      // B
        {0, QStringLiteral("zeroxy")},    // A
        {3, QStringLiteral("zeroz")},     // X
        {2, QStringLiteral("unlock")},    // Y
    };
    QSettings s;
    s.beginGroup(QStringLiteral("gamepad"));
    for (const QString &key : s.childKeys()) {
        bool ok = false;
        const int n = key.toInt(&ok);
        const QString action = s.value(key).toString().trimmed().toLower();
        if (!ok)
            continue;
        if (action.isEmpty() || action == QLatin1String("none"))
            m_padMap.remove(n);
        else
            m_padMap.insert(n, action);
    }
    s.endGroup();
}

void MachinePanel::gamepadAxis(int number, int value)
{
    if (number > 1 || !m_grbl->isConnected())
        return;
    // A D-pad reports its rest position as 0 and a direction as full scale;
    // an analogue stick needs the same treatment with a dead zone, so both are
    // reduced to -1, 0 or +1 here.
    const int dir = value > 16000 ? 1 : (value < -16000 ? -1 : 0);
    if (dir == m_padAxisDir[number])
        return;
    m_padAxisDir[number] = dir;
    if (dir == 0) {
        endHoldJog();
        return;
    }
    // Axis 1 is the pad's vertical: up reads negative, and up should be +Y.
    beginHoldJog(number == 0 ? 'X' : 'Y', number == 0 ? dir : -dir);
}

void MachinePanel::gamepadButton(int number, bool pressed)
{
    const QString action = m_padMap.value(number);
    if (action.isEmpty()) {
        if (pressed)
            log(QStringLiteral("gamepad: button %1 (unmapped — set gamepad/%1 "
                               "in the settings to use it)").arg(number));
        return;
    }
    if (!m_grbl->isConnected()) {
        if (pressed)
            log(QStringLiteral("gamepad: %1 ignored — not connected").arg(action));
        return;
    }

    // Direction actions press and release like a jog key; everything else acts
    // on press. X and Y are here as well as on the axes because plenty of SNES
    // clones report their D-pad as four buttons rather than two axes.
    if (action.size() == 2 && QStringLiteral("xyz").contains(action.at(0))
        && (action.at(1) == QChar('+') || action.at(1) == QChar('-'))) {
        if (pressed)
            beginHoldJog(action.at(0).toUpper().toLatin1(),
                         action.at(1) == QChar('+') ? 1 : -1);
        else
            endHoldJog();
        return;
    }
    if (!pressed)
        return;

    if (action == QLatin1String("step")) {
        const int n = m_step->count();
        if (n > 0)
            m_step->setCurrentIndex((m_step->currentIndex() + 1) % n);
        log(QStringLiteral("gamepad: jog step %1 mm").arg(m_step->currentText()));
    } else if (action == QLatin1String("hold")) {
        m_pauseBtn->click();              // same path as the on-screen button
    } else if (action == QLatin1String("stop")) {
        m_stopBtn->click();
    } else if (action == QLatin1String("zeroxy")) {
        zero(QStringLiteral("XY"));
    } else if (action == QLatin1String("zeroz")) {
        zero(QStringLiteral("Z"));
    } else if (action == QLatin1String("unlock")) {
        m_grbl->sendCommand(QStringLiteral("$X"));
    } else {
        log(QStringLiteral("gamepad: button %1 is mapped to \"%2\", which is not "
                           "an action").arg(number).arg(action));
    }
}

void MachinePanel::refreshPorts()
{
    const QString keep = currentPortName();
    m_ports->clear();
    const QStringList names = GrblStreamer::availablePorts();
    const QStringList details = GrblStreamer::availablePortDetails();
    for (int i = 0; i < names.size(); ++i)
        m_ports->addItem(details.value(i, names.at(i)), names.at(i));
    const int i = m_ports->findData(keep);
    if (i >= 0)
        m_ports->setCurrentIndex(i);
    else if (!keep.isEmpty())
        m_ports->setEditText(keep);
}

void MachinePanel::toggleConnect()
{
    if (m_grbl->isConnected()) {
        m_grbl->disconnectPort();
        return;
    }
    const QString port = currentPortName();
    if (port.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Machine"),
                                 QStringLiteral("No serial port found. Plug in the "
                                                "controller and rescan, or type a device path."));
        return;
    }
    m_grbl->connectPort(port);
}

BitSetterConfig MachinePanel::bitSetter() const
{
    BitSetterConfig c;
    c.enabled = m_bsGroup->isChecked();
    c.x = m_bsX->value();
    c.y = m_bsY->value();
    c.safeZ = m_bsSafeZ->value();
    c.fastFeed = m_bsFast->value();
    c.slowFeed = m_bsSlow->value();
    return c;
}

void MachinePanel::loadSettings()
{
    m_loading = true;                     // widget setters must not save back
    QSettings s;
    s.beginGroup(QStringLiteral("machine"));
    m_bsGroup->setChecked(s.value(QStringLiteral("bitsetter/enabled"), false).toBool());
    m_bsX->setValue(s.value(QStringLiteral("bitsetter/x"), -20.0).toDouble());
    m_bsY->setValue(s.value(QStringLiteral("bitsetter/y"), -20.0).toDouble());
    m_bsSafeZ->setValue(s.value(QStringLiteral("bitsetter/safeZ"), -5.0).toDouble());
    m_bsFast->setValue(s.value(QStringLiteral("bitsetter/fast"), 600.0).toDouble());
    m_bsSlow->setValue(s.value(QStringLiteral("bitsetter/slow"), 50.0).toDouble());
    m_tcX->setValue(s.value(QStringLiteral("toolchange/x"), -120.0).toDouble());
    m_tcY->setValue(s.value(QStringLiteral("toolchange/y"), -20.0).toDouble());
    m_haveRef = s.value(QStringLiteral("ref/valid"), false).toBool();
    m_refZ = s.value(QStringLiteral("ref/z"), 0.0).toDouble();
    const QString port = s.value(QStringLiteral("port")).toString();
    if (!port.isEmpty())
        m_ports->setEditText(port);
    m_loading = false;
}

void MachinePanel::saveSettings()
{
    if (m_loading)
        return;
    QSettings s;
    s.beginGroup(QStringLiteral("machine"));
    s.setValue(QStringLiteral("bitsetter/enabled"), m_bsGroup->isChecked());
    s.setValue(QStringLiteral("bitsetter/x"), m_bsX->value());
    s.setValue(QStringLiteral("bitsetter/y"), m_bsY->value());
    s.setValue(QStringLiteral("bitsetter/safeZ"), m_bsSafeZ->value());
    s.setValue(QStringLiteral("bitsetter/fast"), m_bsFast->value());
    s.setValue(QStringLiteral("bitsetter/slow"), m_bsSlow->value());
    s.setValue(QStringLiteral("toolchange/x"), m_tcX->value());
    s.setValue(QStringLiteral("toolchange/y"), m_tcY->value());
    s.setValue(QStringLiteral("ref/valid"), m_haveRef);
    s.setValue(QStringLiteral("ref/z"), m_refZ);
    if (m_grbl->isConnected())
        s.setValue(QStringLiteral("port"), currentPortName());
}

void MachinePanel::updateOffsetLabels()
{
    m_refLabel->setText(m_haveRef ? QStringLiteral("tool tripped at machine Z %1")
                                        .arg(m_refZ, 0, 'f', 3)
                                  : QStringLiteral("none — zero Z or measure to set"));
    const double tlo = m_grbl->toolLengthOffset();
    m_tloLabel->setText(QStringLiteral("G43.1 Z%1%2").arg(tlo, 0, 'f', 3)
                            .arg(tlo == 0.0 ? QStringLiteral("  (reference tool)") : QString()));
}

// ---- status ------------------------------------------------------------------

void MachinePanel::onStatus(const MachineStatus &st)
{
    m_state->setText(st.state);
    m_wpos->setText(QStringLiteral("X %1  Y %2  Z %3")
                        .arg(st.wx, 8, 'f', 3).arg(st.wy, 8, 'f', 3).arg(st.wz, 8, 'f', 3));
    m_mpos->setText(QStringLiteral("X %1  Y %2  Z %3")
                        .arg(st.mx, 8, 'f', 3).arg(st.my, 8, 'f', 3).arg(st.mz, 8, 'f', 3));
    QString extra = QStringLiteral("F %1   S %2").arg(st.feed, 0, 'f', 0).arg(st.spindle, 0, 'f', 0);
    if (m_grbl->toolLengthOffset() != 0.0)
        extra += QStringLiteral("   TLO %1").arg(m_grbl->toolLengthOffset(), 0, 'f', 3);
    if (st.probePin)
        extra += QStringLiteral("   PROBE");
    m_extra->setText(extra);
    emit livePosition(st.wx, st.wy, st.wz, true);

    // Deferred actions wait for two consecutive Idle reports: the `ok` of a
    // G0 arrives before the move starts, so a single Idle can be stale.
    if (m_onIdle) {
        if (st.state == QLatin1String("Idle") && m_lastState == QLatin1String("Idle")) {
            auto fn = std::move(m_onIdle);
            m_onIdle = nullptr;
            fn();
        }
    }
    m_lastState = st.state;
}

void MachinePanel::whenIdle(std::function<void()> fn)
{
    m_lastState.clear();
    m_onIdle = std::move(fn);
}

// ---- zero / go ---------------------------------------------------------------

void MachinePanel::zero(const QString &axes)
{
    if (!m_grbl->canSendCommand()) {
        log(QStringLiteral("!! busy — cannot zero now"));
        return;
    }
    QStringList cmds;
    const bool zeroZ = axes.contains(QChar('Z'));
    if (zeroZ) {
        // A fresh Z zero belongs to the tool in the spindle, so any old length
        // offset must go first; the measurement below re-establishes it as 0.
        cmds << QStringLiteral("G49");
        m_grbl->applyToolLengthOffset(0);
    }
    QString g10 = QStringLiteral("G10 L20 P1");
    for (const QChar ax : axes)
        g10 += QStringLiteral(" %1").arg(ax) + QStringLiteral("0");   // " X0"
    cmds << g10;

    const BitSetterConfig cfg = bitSetter();
    if (zeroZ && cfg.enabled) {
        const MachineStatus st = m_grbl->status();
        m_phase = Phase::MeasureForRef;
        m_grbl->startMacro(cmds + GrblStreamer::measureToolLines(cfg, true, st.mx, st.my));
        log(QStringLiteral("zeroing Z, then measuring the reference tool on the BitSetter"));
    } else {
        for (const QString &c : cmds)
            m_grbl->sendCommand(c);
    }
    updateOffsetLabels();
}

void MachinePanel::goToXY0()
{
    if (!m_grbl->canSendCommand())
        return;
    m_phase = Phase::GoToZero;
    m_grbl->startMacro({QStringLiteral("G90"),
                        QStringLiteral("G53 G0 Z%1").arg(m_bsSafeZ->value(), 0, 'f', 3),
                        QStringLiteral("G0 X0 Y0")});
}

// ---- BitSetter / tool change -------------------------------------------------

void MachinePanel::measureTool(Phase why)
{
    const BitSetterConfig cfg = bitSetter();
    if (!m_grbl->isConnected()) {
        log(QStringLiteral("!! not connected"));
        return;
    }
    if (!cfg.enabled) {
        log(QStringLiteral("!! BitSetter is disabled — enable it and set the button position"));
        return;
    }
    if (!m_grbl->canSendCommand()) {
        log(QStringLiteral("!! busy — cannot probe now"));
        return;
    }
    m_phase = why;
    const MachineStatus st = m_grbl->status();
    // Manual measurements return to where the tool was; after a tool change
    // the program's first rapid takes over from safe Z anyway.
    m_grbl->measureTool(cfg, why == Phase::MeasureManual, st.mx, st.my);
}

void MachinePanel::onToolChange(int tool)
{
    m_pendingTool = tool;
    m_toolMeasured = false;
    m_runBtn->setText(QStringLiteral("▶ Continue"));
    m_phase = Phase::ParkForTool;
    // Spindle off, up, and over to where the operator can reach the collet.
    m_grbl->startMacro({QStringLiteral("M5"), QStringLiteral("G90"),
                        QStringLiteral("G53 G0 Z%1").arg(m_bsSafeZ->value(), 0, 'f', 3),
                        QStringLiteral("G53 G0 X%1 Y%2")
                            .arg(m_tcX->value(), 0, 'f', 3).arg(m_tcY->value(), 0, 'f', 3)});
}

void MachinePanel::onMacroFinished(bool ok)
{
    const Phase phase = m_phase;
    m_phase = Phase::Idle;
    switch (phase) {
    case Phase::Idle:
    case Phase::GoToZero:
        return;

    case Phase::ParkForTool:
        if (!ok) {
            log(QStringLiteral("!! could not park for the tool change — fix the alarm, "
                               "then press Continue"));
            return;
        }
        whenIdle([this] {
            const bool bs = m_bsGroup->isChecked();
            const QString msg = QStringLiteral("Insert tool T%1 and tighten the collet.\n\n%2")
                .arg(m_pendingTool)
                .arg(bs ? QStringLiteral("OK measures it on the BitSetter, then the program continues.")
                        : QStringLiteral("BitSetter is off: re-zero Z by hand if the tool length "
                                         "changed, then press OK to continue."));
            const auto r = QMessageBox::question(this, QStringLiteral("Tool change"), msg,
                                                 QMessageBox::Ok | QMessageBox::Cancel,
                                                 QMessageBox::Ok);
            if (r != QMessageBox::Ok) {
                m_grbl->stopStream();
                return;
            }
            if (bs)
                measureTool(Phase::MeasureAfterChange);
            else
                m_grbl->continueAfterToolChange();
        });
        return;

    case Phase::MeasureForRef:
        if (ok && m_grbl->lastProbeOk()) {
            m_haveRef = true;
            m_refZ = m_grbl->lastProbeZ();
            m_grbl->applyToolLengthOffset(0);
            log(QStringLiteral("reference tool: trips at machine Z %1").arg(m_refZ, 0, 'f', 3));
        } else {
            log(QStringLiteral("!! reference measurement failed — Z zero is set, but tool "
                               "changes cannot be compensated until a measurement succeeds"));
        }
        saveSettings();
        updateOffsetLabels();
        return;

    case Phase::MeasureManual:
    case Phase::MeasureAfterChange:
        if (!(ok && m_grbl->lastProbeOk())) {
            log(QStringLiteral("!! tool measurement failed (probe not triggered?). "
                               "Unlock if alarmed, check the BitSetter position, retry."));
            if (phase == Phase::MeasureAfterChange)
                log(QStringLiteral("program is parked — Measure tool again, then Continue"));
            return;
        }
        if (!m_haveRef) {
            m_haveRef = true;
            m_toolMeasured = true;
            m_refZ = m_grbl->lastProbeZ();
            m_grbl->applyToolLengthOffset(0);
            log(QStringLiteral("no reference yet — this tool becomes the reference "
                               "(trips at machine Z %1)").arg(m_refZ, 0, 'f', 3));
        } else {
            m_toolMeasured = true;
            const double tlo = m_grbl->lastProbeZ() - m_refZ;
            m_grbl->applyToolLengthOffset(tlo);
            log(QStringLiteral("tool length offset %1%2 mm vs reference → G43.1 applied")
                    .arg(tlo >= 0 ? QStringLiteral("+") : QString()).arg(tlo, 0, 'f', 3));
        }
        saveSettings();
        updateOffsetLabels();
        if (phase == Phase::MeasureAfterChange)
            whenIdle([this] { m_grbl->continueAfterToolChange(); });
        return;
    }
}

// ---- program -------------------------------------------------------------------

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
    // Re-exported at the moment Run is pressed, so it is the current document
    // that gets streamed - and a 3D finish is minutes of work, which used to
    // freeze the window with the machine sitting idle.
    const GcodeResult r = exportWithProgress(this, *m_doc, QStringLiteral("Preparing program"));
    if (r.cancelled) {
        log(QStringLiteral("program preparation cancelled"));
        return;
    }
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
    const int npause = int(lines.filter(QStringLiteral("M0 ;")).size());
    QString q = QStringLiteral("%1 toolpath(s), %2 lines\n%3")
                    .arg(r.done.size()).arg(lines.size())
                    .arg(statsSummary(computeStats(r.ops)));
    if (npause > 0)
        q += QStringLiteral("\n\n%1 tool change(s): the machine parks at the tool-change "
                            "spot and prompts%2.")
                 .arg(npause)
                 .arg(m_bsGroup->isChecked() ? QStringLiteral(", then measures on the BitSetter")
                                             : QString());
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

    if (air)
        log(QStringLiteral("air cut: spindle off, Z lifted %1 mm").arg(m_airLift->value(), 0, 'f', 1));
    m_held = false;
    m_pauseBtn->setText(QStringLiteral("⏸ Hold"));
    m_grbl->startStream(lines);
}

} // namespace c2d
