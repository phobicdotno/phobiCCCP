#include "machinepanel.h"
#include "shopdialog.h"
#include "toollibrary.h"
#include "toolstrip.h"

#include "gamepad.h"
#include "gamepadmapdialog.h"
#include "settingsdialog.h"

#include "exportprogress.h"
#include "c2ddocument.h"
#include "gcodeexport.h"
#include "surfacemap.h"
#include "post_grbl.h"
#include "tiling.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDialog>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QDateTime>
#include <QStandardPaths>
#include <QFrame>
#include <QShowEvent>
#include <QLineEdit>
#include <QStackedWidget>
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
#include <QPixmap>
#include <QIcon>
#include <QPainterPath>
#include <QPainter>
#include <QScrollArea>
#include <QSettings>
#include <QTimer>
#include <memory>
#include <utility>
#include <QToolButton>
#include <QVBoxLayout>

namespace c2d {

// A SNES pad (PAL colours): grey dog-bone body with a darker inner ring on
// the button side, black d-pad left, the four coloured buttons in a diamond
// right (X blue top, Y green left, A red right, B yellow bottom), select and
// start slanted in the middle. For the "map gamepad" button.
static QIcon snesIcon()
{
    QPixmap pm(QSize(40, 24) * 4);
    pm.setDevicePixelRatio(4);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath left, right, mid;
    left.addEllipse(QPointF(10, 12), 9.5, 9.5);
    right.addEllipse(QPointF(30, 12), 9.5, 9.5);
    mid.addRect(QRectF(10, 4.5, 20, 15));
    const QPainterPath body = left.united(right).united(mid);
    p.setPen(QPen(QColor(0x8b, 0x90, 0x9a), 1.0));
    p.setBrush(QColor(0xc9, 0xcc, 0xd3));
    p.drawPath(body);
    // darker ring behind the buttons
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x9a, 0x9e, 0xa8));
    p.drawEllipse(QPointF(30, 12), 7.2, 7.2);
    // d-pad
    p.setBrush(QColor(0x2a, 0x2d, 0x33));
    p.drawRoundedRect(QRectF(8.2, 6.8, 3.6, 10.4), 0.8, 0.8);
    p.drawRoundedRect(QRectF(4.8, 10.2, 10.4, 3.6), 0.8, 0.8);
    // buttons
    struct B { QPointF at; QColor col; };
    const B bs[4] = {{QPointF(30, 7.4), QColor(0x3b, 0x6f, 0xd8)},    // X
                     {QPointF(25.4, 12), QColor(0x2f, 0xa8, 0x5a)},   // Y
                     {QPointF(34.6, 12), QColor(0xd8, 0x3a, 0x3a)},   // A
                     {QPointF(30, 16.6), QColor(0xf0, 0xc0, 0x30)}};  // B
    for (const B &b : bs) {
        p.setBrush(b.col);
        p.drawEllipse(b.at, 2.3, 2.3);
    }
    // select / start, slanted
    p.setBrush(QColor(0x5a, 0x5f, 0x69));
    for (double x : {16.9, 20.3}) {
        p.save();
        p.translate(x, 13.4);
        p.rotate(-30);
        p.drawRoundedRect(QRectF(-1.4, -0.7, 2.8, 1.4), 0.7, 0.7);
        p.restore();
    }
    return QIcon(pm);
}

// A NES pad: flat grey box with a black face plate, black d-pad left, grey
// select/start in the middle and the two red round buttons (B, A) right.
static QIcon nesIcon()
{
    QPixmap pm(QSize(40, 24) * 4);
    pm.setDevicePixelRatio(4);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(0x8b, 0x90, 0x9a), 1.0));
    p.setBrush(QColor(0xc9, 0xcc, 0xd3));
    p.drawRoundedRect(QRectF(1, 4, 38, 16), 1.5, 1.5);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x22, 0x24, 0x29));
    p.drawRoundedRect(QRectF(3, 6, 34, 12), 1.0, 1.0);
    // d-pad
    p.setBrush(QColor(0x0f, 0x10, 0x12));
    p.setPen(QPen(QColor(0x5a, 0x5f, 0x69), 0.5));
    p.drawRect(QRectF(8.4, 7.6, 2.6, 8.8));
    p.drawRect(QRectF(5.3, 10.7, 8.8, 2.6));
    // select / start
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x9a, 0x9e, 0xa8));
    p.drawRoundedRect(QRectF(16.2, 12.6, 3.0, 1.4), 0.7, 0.7);
    p.drawRoundedRect(QRectF(20.8, 12.6, 3.0, 1.4), 0.7, 0.7);
    // B and A in their grey squares
    for (double x : {27.6, 33.0}) {
        p.setBrush(QColor(0xc9, 0xcc, 0xd3));
        p.drawRoundedRect(QRectF(x - 2.5, 10.0, 5.0, 5.0), 0.8, 0.8);
        p.setBrush(QColor(0xd8, 0x2a, 0x2a));
        p.drawEllipse(QPointF(x, 12.5), 1.9, 1.9);
    }
    return QIcon(pm);
}

// The pad style the mapper last set up ("nes" or "snes").
static QIcon padIcon()
{
    return QSettings().value(QStringLiteral("ui/padStyle")).toString() == QLatin1String("nes")
        ? nesIcon() : snesIcon();
}

// A stacked widget normally reserves the size of its largest page, so a small
// page would still stand as tall as the tallest one and the Machine page would
// scroll for nothing. This one is exactly as big as the page on show.
class CurrentPageStack : public QStackedWidget
{
public:
    using QStackedWidget::QStackedWidget;
    QSize sizeHint() const override
    {
        return currentWidget() ? currentWidget()->sizeHint() : QStackedWidget::sizeHint();
    }
    QSize minimumSizeHint() const override
    {
        return currentWidget() ? currentWidget()->minimumSizeHint()
                               : QStackedWidget::minimumSizeHint();
    }
};

// The big work-position readings: as large as the column allows, never
// wider than it. The font shrinks to fit the widest reading the machine can
// show, so the numbers never push the page into scrolling.
class FitLabel : public QLabel
{
public:
    explicit FitLabel(QWidget *parent) : QLabel(parent)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setMinimumWidth(90);
    }
    void setMaxFont(const QFont &f) { m_max = f; fit(); }
protected:
    void resizeEvent(QResizeEvent *e) override
    {
        QLabel::resizeEvent(e);
        fit();
    }
private:
    void fit()
    {
        QFont f = m_max;
        const int need = QFontMetrics(f).horizontalAdvance(QStringLiteral("-8888.888"));
        if (need > width() && need > 0)
            f.setPointSizeF(qMax(8.0, f.pointSizeF() * width() / need));
        if (f != font())
            setFont(f);
    }
    QFont m_max;
};

class BodyLayout : public QVBoxLayout
{
public:
    using QVBoxLayout::QVBoxLayout;
    bool hasHeightForWidth() const override { return false; }
};

static QDoubleSpinBox *mmSpin(QWidget *parent, double lo, double hi, double val,
                              int decimals = 2, const QString &suffix = QStringLiteral(" mm"))
{
    auto *s = new QDoubleSpinBox(parent);
    s->setRange(lo, hi);
    s->setDecimals(decimals);
    s->setValue(val);
    s->setSuffix(suffix);
    s->setKeyboardTracking(false);
    s->setMinimumWidth(70);         // the range would otherwise set a wide minimum
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

    // Full-page "shop floor" layout, made for the touchscreen: a status line
    // across the top, three cards (position, jog, job) and a row of machine
    // actions underneath. Everything that is not needed while cutting lives
    // behind a button (BitSetter…) or a fold (More, Log). The page scrolls
    // when the window is too small for it.
    setObjectName(QStringLiteral("machinePage"));
    setAttribute(Qt::WA_StyledBackground);
    setStyleSheet(QStringLiteral(
        "#machinePage, #machineBody { background: #16181d; }"
        "QFrame#card { background: #1e2127; border-radius: 12px; }"
        "QLabel#caption { color: #8b93a1; font-size: 11px; letter-spacing: 1px; }"
        "QLabel#axis { color: #8b93a1; font-size: 20px; }"
        "QLabel#field { color: #8b93a1; font-size: 14px; }"
        "QDoubleSpinBox, QComboBox { font-size: 14px; min-height: 34px; }"
        "QLabel#statePill { border-radius: 10px; padding: 6px 14px; background: #1e2127; "
        "  border: 1px solid #2a2e36; }"
        "QPushButton#jog { font-size: 22px; border-radius: 14px; background: #2a2e36; }"
        "QPushButton#jog:pressed { background: #3a4150; }"
        "QPushButton#jogStop { font-size: 13px; border-radius: 36px; background: #16181d; "
        "  border: 2px solid #3a404b; }"
        "QPushButton#start { font-size: 18px; font-weight: bold; border-radius: 12px; "
        "  background: #f2a536; color: #1a1a1a; }"
        "QPushButton#start:disabled { background: #4a3413; color: #8b93a1; }"
        "QPushButton#stop { font-size: 15px; font-weight: bold; border-radius: 10px; "
        "  background: #7a2626; color: #ffffff; }"
        "QPushButton#stop:disabled { background: #3a1f1f; color: #8b93a1; }"
        "QPushButton#big { font-size: 15px; border-radius: 10px; background: #2a2e36; "
        "  border: 1px solid #3a404b; padding: 0 14px; }"
        "QPushButton#big:pressed { background: #3a4150; }"
        "QPushButton#big:disabled { color: #6a717d; border-color: #2a2e36; }"
        "QToolButton#step { padding: 6px 0; border-radius: 8px; background: transparent; "
        "  color: #a3abb8; font-family: monospace; font-size: 14px; }"
        "QToolButton#step:checked { background: #f2a536; color: #1a1a1a; font-weight: bold; }"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *body = new QWidget(scroll);
    body->setObjectName(QStringLiteral("machineBody"));
    scroll->setWidget(body);
    outer->addWidget(scroll, 3);
    // The scroll area asks a layout with word-wrapped labels for its height
    // at the current width and gets the preferred height, which counts the
    // cards' stretch space, so the page scrolled with room to spare. Without
    // height-for-width it goes by the minimum: scroll only when the cards
    // really do not fit.
    auto *lay = new BodyLayout(body);
    body->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    lay->setSizeConstraint(QLayout::SetMinimumSize);
    lay->setContentsMargins(14, 10, 14, 8);
    lay->setSpacing(12);
    auto caption = [](const QString &t, QWidget *parent) {
        auto *l = new QLabel(t.toUpper(), parent);
        l->setObjectName(QStringLiteral("caption"));
        return l;
    };
    auto card = [](QWidget *parent) {
        auto *f = new QFrame(parent);
        f->setObjectName(QStringLiteral("card"));
        auto *v = new QVBoxLayout(f);
        v->setContentsMargins(18, 16, 18, 16);
        v->setSpacing(10);
        return f;
    };
    auto bigBtn = [](QPushButton *b, int h) {
        b->setObjectName(QStringLiteral("big"));
        b->setMinimumHeight(h);
        return b;
    };

    // ---- State pill (top of the position card) and connection (top of the
    // job card): no header row, so the cards get the whole height.
    m_state = new QLabel(QStringLiteral("Not connected"), body);
    m_state->setObjectName(QStringLiteral("statePill"));
    QFont bold = m_state->font();
    bold.setBold(true);
    bold.setPointSizeF(bold.pointSizeF() + 1);
    m_state->setFont(bold);
    m_state->setMinimumHeight(36);
    // Says which pad is driving the jog, and stays out of the way when there
    // is none. Button numbers are logged to the console on every press so a
    // clone with a different layout can be remapped without guessing.
    m_padLabel = new QLabel(QStringLiteral("gamepad: none"), body);
    m_padLabel->setEnabled(false);
    m_padLabel->setStyleSheet(QStringLiteral("font-size: 12px;"));
    m_ports = new QComboBox(body);
    m_ports->setEditable(true);            // type /dev/ttyACM0 or a pty by hand
    m_ports->setMinimumWidth(150);
    m_ports->setMaximumWidth(340);
    m_ports->setMinimumHeight(34);
    m_ports->setToolTip(QStringLiteral("Serial port of the GRBL controller. "
                                       "Pick one, or type a device path."));
    auto *refresh = new QPushButton(QStringLiteral("⟳"), body);
    refresh->setFixedSize(34, 34);
    refresh->setToolTip(QStringLiteral("Rescan serial ports"));
    m_connectBtn = new QPushButton(QStringLiteral("Connect"), body);
    m_connectBtn->setMinimumSize(110, 34);
    auto *connRow = new QHBoxLayout;           // placed in the job card below
    connRow->setSpacing(6);
    connRow->addWidget(m_ports, 1);
    connRow->addWidget(refresh);
    connRow->addWidget(m_connectBtn);
    connect(refresh, &QPushButton::clicked, this, [this] { refreshPorts(); });
    connect(m_connectBtn, &QPushButton::clicked, this, [this] { toggleConnect(); });

    // ---- Initialize (shown while the controller is in Alarm) --------------
    m_initBanner = new QWidget(body);
    m_initBanner->setObjectName(QStringLiteral("initBanner"));
    m_initBanner->setAttribute(Qt::WA_StyledBackground);
    m_initBanner->setStyleSheet(QStringLiteral(
        "#initBanner { background: #4a3413; border: 1px solid #f2a536; border-radius: 10px; }"));
    auto *initLay = new QHBoxLayout(m_initBanner);
    initLay->setContentsMargins(16, 10, 16, 10);
    auto *initText = new QLabel(QStringLiteral(
        "<b>Machine needs to be initialized.</b> It homes to its corner switches "
        "first; until then GRBL refuses jogging and zeroing. Keep hands and "
        "clamps clear of the gantry."), m_initBanner);
    initText->setWordWrap(true);
    m_initBtn = new QPushButton(QStringLiteral("Initialize machine (home)"), m_initBanner);
    QFont big = m_initBtn->font();
    big.setBold(true);
    big.setPointSizeF(big.pointSizeF() + 2);
    m_initBtn->setFont(big);
    m_initBtn->setMinimumSize(260, 48);
    initLay->addWidget(initText, 1);
    initLay->addWidget(m_initBtn);
    m_initBanner->hide();
    lay->addWidget(m_initBanner);
    connect(m_initBtn, &QPushButton::clicked, this, [this] { initializeMachine(); });

    auto *cols = new QHBoxLayout;
    cols->setSpacing(12);
    lay->addLayout(cols, 1);

    // ---- Position card -----------------------------------------------------
    QFrame *posCard = card(body);
    auto *pv = static_cast<QVBoxLayout *>(posCard->layout());
    pv->addWidget(m_state);
    pv->addWidget(caption(QStringLiteral("Work position"), posCard));
    QFont mono(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::Monospace);
    mono.setPointSizeF(mono.pointSizeF() * 2.6);
    mono.setWeight(QFont::DemiBold);
    const char *axisNames[] = {"X", "Y", "Z"};
    const char *axisTips[] = {"Current X becomes work X0  (G10 L20)",
                              "Current Y becomes work Y0",
                              "Current Z becomes work Z0. With the BitSetter enabled the "
                              "tool is measured afterwards as the reference tool."};
    for (int i = 0; i < 3; ++i) {
        auto *row = new QHBoxLayout;
        row->setSpacing(12);
        auto *name = new QLabel(QString::fromLatin1(axisNames[i]), posCard);
        name->setObjectName(QStringLiteral("axis"));
        name->setFixedWidth(26);
        auto *value = new FitLabel(posCard);
        value->setText(QStringLiteral("—"));
        value->setMaxFont(mono);
        m_wAxis[i] = value;
        m_wAxis[i]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto *z = new QPushButton(QStringLiteral("Zero"), posCard);
        z->setMinimumSize(72, 40);
        z->setToolTip(QString::fromLatin1(axisTips[i]));
        const QString axes = QString::fromLatin1(axisNames[i]);
        connect(z, &QPushButton::clicked, this, [this, axes] { zero(axes); });
        row->addWidget(name);
        row->addWidget(m_wAxis[i], 1);
        row->addWidget(z);
        pv->addLayout(row);
    }
    m_mpos = new QLabel(QStringLiteral("machine —"), posCard);
    m_extra = new QLabel(QString(), posCard);
    m_mpos->setStyleSheet(QStringLiteral("color: #8b93a1; font-family: monospace;"));
    m_extra->setStyleSheet(QStringLiteral("color: #8b93a1; font-family: monospace;"));
    pv->addWidget(m_mpos);
    pv->addWidget(m_extra);
    pv->addStretch(1);
    auto *zeroRow = new QHBoxLayout;
    auto *zeroXY = bigBtn(new QPushButton(QStringLiteral("Zero X and Y here"), posCard), 46);
    zeroXY->setToolTip(QStringLiteral("Current X and Y become work X0 Y0"));
    connect(zeroXY, &QPushButton::clicked, this, [this] { zero(QStringLiteral("XY")); });
    auto *zeroAll = bigBtn(new QPushButton(QStringLiteral("Zero all"), posCard), 46);
    zeroAll->setToolTip(QStringLiteral("Current position becomes work X0 Y0 Z0"));
    connect(zeroAll, &QPushButton::clicked, this, [this] { zero(QStringLiteral("XYZ")); });
    zeroRow->addWidget(zeroXY, 2);
    zeroRow->addWidget(zeroAll, 1);
    pv->addLayout(zeroRow);
    auto *gotoBtn = bigBtn(new QPushButton(QStringLiteral("Go to X0 Y0"), posCard), 46);
    gotoBtn->setToolTip(QStringLiteral("Raise to safe Z, then rapid to work X0 Y0"));
    connect(gotoBtn, &QPushButton::clicked, this, [this] { goToXY0(); });
    pv->addWidget(gotoBtn);
    auto *measureBig = bigBtn(new QPushButton(QStringLiteral("Measure bit (BitSetter)"), posCard), 46);
    measureBig->setToolTip(QStringLiteral(
        "Go to the button, probe twice, come back. The first measurement after "
        "zeroing Z is the reference; later ones set the G43.1 length offset."));
    connect(measureBig, &QPushButton::clicked, this, [this] { measureTool(Phase::MeasureManual); });
    pv->addWidget(measureBig);
    auto *probeBig = bigBtn(new QPushButton(QStringLiteral("Probe zero (BitZero)…"), posCard), 46);
    probeBig->setToolTip(QStringLiteral(
        "Find the stock corner, or one edge, or the surface, with Carbide's BitZero V2 "
        "touch plate and set work zero there."));
    connect(probeBig, &QPushButton::clicked, this, [this] { openBitZero(); });
    pv->addWidget(probeBig);
    cols->addWidget(posCard, 10);

    // ---- Jog card ----------------------------------------------------------
    QFrame *jogCard = card(body);
    auto *jv = static_cast<QVBoxLayout *>(jogCard->layout());
    jv->addWidget(caption(QStringLiteral("Jog  ·  tap = step, hold = continuous"), jogCard));
    jv->addStretch(1);
    auto *grid = new QGridLayout;
    grid->setSpacing(8);
    auto jogBig = [this](const QString &t, char axis, int dir, int w, int h) {
        QPushButton *b = jogButton(t, axis, dir);
        b->setObjectName(QStringLiteral("jog"));
        b->setFixedSize(w, h);
        return b;
    };
    grid->addWidget(jogBig(QStringLiteral("Y+"), 'Y', +1, 76, 76), 0, 1);
    grid->addWidget(jogBig(QStringLiteral("X−"), 'X', -1, 76, 76), 1, 0);
    grid->addWidget(jogBig(QStringLiteral("X+"), 'X', +1, 76, 76), 1, 2);
    grid->addWidget(jogBig(QStringLiteral("Y−"), 'Y', -1, 76, 76), 2, 1);
    auto *stopJog = new QPushButton(QStringLiteral("STOP"), jogCard);
    stopJog->setObjectName(QStringLiteral("jogStop"));
    stopJog->setFixedSize(76, 76);
    stopJog->setFocusPolicy(Qt::NoFocus);
    stopJog->setToolTip(QStringLiteral("Cancel jog"));
    connect(stopJog, &QPushButton::clicked, this, [this] {
        m_deferredJogs.clear();
        m_grbl->jogCancel();
        forgetJogPlan();
    });
    grid->addWidget(stopJog, 1, 1);
    auto *gridRow = new QHBoxLayout;
    gridRow->addStretch(1);
    gridRow->addLayout(grid);
    gridRow->addStretch(1);
    jv->addLayout(gridRow);
    auto *zRow = new QHBoxLayout;
    zRow->addStretch(1);
    zRow->addWidget(jogBig(QStringLiteral("Z ▲"), 'Z', +1, 110, 50));
    zRow->addWidget(jogBig(QStringLiteral("Z ▼"), 'Z', -1, 110, 50));
    zRow->addStretch(1);
    jv->addLayout(zRow);
    jv->addStretch(1);
    // Step size as a segmented row; the combo box behind it keeps the value
    // (the gamepad cycles it) and stays hidden.
    m_step = new QComboBox(jogCard);
    m_step->addItems({QStringLiteral("0.1"), QStringLiteral("1"),
                      QStringLiteral("10"), QStringLiteral("100")});
    m_step->setCurrentIndex(2);
    m_step->hide();
    auto *stepBox = new QWidget(jogCard);
    stepBox->setObjectName(QStringLiteral("stepBox"));
    stepBox->setAttribute(Qt::WA_StyledBackground);
    stepBox->setStyleSheet(QStringLiteral("#stepBox { background: #16181d; border-radius: 12px; }"));
    auto *stepLay = new QHBoxLayout(stepBox);
    stepLay->setContentsMargins(5, 5, 5, 5);
    stepLay->setSpacing(4);
    auto *stepLbl = new QLabel(QStringLiteral("step mm"), stepBox);
    stepLbl->setStyleSheet(QStringLiteral("color: #8b93a1; padding-left: 6px;"));
    stepLay->addWidget(stepLbl);
    auto *stepGrp = new QButtonGroup(this);
    stepGrp->setExclusive(true);
    for (int i = 0; i < m_step->count(); ++i) {
        auto *b = new QToolButton(stepBox);
        b->setObjectName(QStringLiteral("step"));
        b->setText(m_step->itemText(i));
        b->setCheckable(true);
        b->setChecked(i == m_step->currentIndex());
        b->setMinimumWidth(52);
        b->setFocusPolicy(Qt::NoFocus);
        stepGrp->addButton(b, i);
        stepLay->addWidget(b, 1);
    }
    connect(stepGrp, &QButtonGroup::idClicked, m_step, &QComboBox::setCurrentIndex);
    connect(m_step, &QComboBox::currentIndexChanged, this, [stepGrp](int i) {
        if (QAbstractButton *b = stepGrp->button(i))
            b->setChecked(true);
    });
    jv->addWidget(stepBox);
    // Jog speed as a toggle row like the step; the combo box behind it keeps
    // the value (jogFeed reads it) and stays hidden.
    m_speed = new QComboBox(jogCard);
    m_speed->addItem(QStringLiteral("slow"), 200);
    m_speed->addItem(QStringLiteral("medium"), 1000);
    m_speed->addItem(QStringLiteral("fast"), 3000);
    m_speed->addItem(QStringLiteral("rapid"), 5000);   // $110/$111 on the Shapeoko 5 Pro
    m_speed->setCurrentIndex(1);
    m_speed->hide();
    auto *speedBox = new QWidget(jogCard);
    speedBox->setObjectName(QStringLiteral("stepBox"));
    speedBox->setAttribute(Qt::WA_StyledBackground);
    speedBox->setStyleSheet(QStringLiteral("#stepBox { background: #16181d; border-radius: 12px; }"));
    auto *speedLay = new QHBoxLayout(speedBox);
    speedLay->setContentsMargins(5, 5, 5, 5);
    speedLay->setSpacing(4);
    auto *speedLbl = new QLabel(QStringLiteral("speed mm/min"), speedBox);
    speedLbl->setStyleSheet(QStringLiteral("color: #8b93a1; padding-left: 6px;"));
    speedLay->addWidget(speedLbl);
    auto *speedGrp = new QButtonGroup(this);
    speedGrp->setExclusive(true);
    for (int i = 0; i < m_speed->count(); ++i) {
        auto *b = new QToolButton(speedBox);
        b->setObjectName(QStringLiteral("step"));
        b->setToolTip(QStringLiteral("%1 mm/min").arg(m_speed->itemData(i).toInt()));
        b->setCheckable(true);
        b->setMinimumWidth(52);
        b->setFocusPolicy(Qt::NoFocus);
        // Name over the feed in small print. A tool button has one font, so
        // the two lines are labels laid on it that let clicks through.
        auto *bl = new QVBoxLayout(b);
        bl->setContentsMargins(2, 4, 2, 4);
        bl->setSpacing(0);
        auto *name = new QLabel(m_speed->itemText(i), b);
        auto *feed = new QLabel(QString::number(m_speed->itemData(i).toInt()), b);
        for (QLabel *l : {name, feed}) {
            l->setAlignment(Qt::AlignCenter);
            l->setAttribute(Qt::WA_TransparentForMouseEvents);
            bl->addWidget(l);
        }
        auto paint = [name, feed](bool on) {
            name->setStyleSheet(on ? QStringLiteral("color: #1a1a1a; font: bold 14px monospace;")
                                   : QStringLiteral("color: #a3abb8; font: 14px monospace;"));
            feed->setStyleSheet(on ? QStringLiteral("color: #3a2a0a; font: 10px monospace;")
                                   : QStringLiteral("color: #6b7380; font: 10px monospace;"));
        };
        connect(b, &QToolButton::toggled, b, paint);
        b->setChecked(i == m_speed->currentIndex());
        paint(b->isChecked());
        // The labels do not count toward a tool button's size: give it room
        // for the bold (checked) name and let it share out the row.
        QFont boldName = name->font();
        boldName.setBold(true);
        b->setMinimumWidth(QFontMetrics(boldName).horizontalAdvance(name->text()) + 22);
        b->setMinimumHeight(name->sizeHint().height() + feed->sizeHint().height() + 12);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        speedGrp->addButton(b, i);
        speedLay->addWidget(b, 1);
    }
    connect(speedGrp, &QButtonGroup::idClicked, m_speed, &QComboBox::setCurrentIndex);
    connect(m_speed, &QComboBox::currentIndexChanged, this, [speedGrp](int i) {
        if (QAbstractButton *b = speedGrp->button(i))
            b->setChecked(true);
    });
    jv->addWidget(speedBox);
    auto *padMap = new QPushButton(jogCard);
    padMap->setObjectName(QStringLiteral("big"));
    padMap->setIcon(padIcon());
    m_padMapBtn = padMap;
    padMap->setIconSize(QSize(40, 24));
    padMap->setMinimumSize(64, 40);
    padMap->setToolTip(QStringLiteral("Map the gamepad: choose what each button does"));
    connect(padMap, &QPushButton::clicked, this, [this] { editGamepadMapping(); });
    jv->addWidget(padMap, 0, Qt::AlignRight);
    cols->addWidget(jogCard, 11);

    // ---- Job card ----------------------------------------------------------
    QFrame *jobCard = card(body);
    auto *cardLay = static_cast<QVBoxLayout *>(jobCard->layout());
    cardLay->setContentsMargins(0, 0, 0, 0);
    // Connection: its own card (island) above the job card.
    QFrame *connCard = card(body);
    auto *connLay = static_cast<QVBoxLayout *>(connCard->layout());
    connLay->setContentsMargins(16, 12, 16, 12);
    connLay->setSpacing(4);
    connLay->addLayout(connRow);
    connLay->addWidget(m_padLabel);
    connCard->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    m_rightStack = new CurrentPageStack(jobCard);
    cardLay->addWidget(m_rightStack);
    // Let only the current page count (see CurrentPageStack).
    connect(m_rightStack, &QStackedWidget::currentChanged, this, [this](int cur) {
        for (int i = 0; i < m_rightStack->count(); ++i) {
            QWidget *w = m_rightStack->widget(i);
            QSizePolicy sp = w->sizePolicy();
            sp.setVerticalPolicy(i == cur ? QSizePolicy::Preferred : QSizePolicy::Ignored);
            w->setSizePolicy(sp);
        }
        m_rightStack->updateGeometry();
        m_rightStack->adjustSize();
    });
    m_jobPage = new QWidget(m_rightStack);
    auto *bv = new QVBoxLayout(m_jobPage);
    bv->setContentsMargins(18, 10, 18, 16);
    bv->setSpacing(10);
    m_rightStack->addWidget(m_jobPage);
    bv->addWidget(caption(QStringLiteral("Job"), m_jobPage));
    m_jobName = new QLabel(QStringLiteral("No design open"), jobCard);
    QFont jobFont = m_jobName->font();
    jobFont.setBold(true);
    jobFont.setPointSizeF(jobFont.pointSizeF() + 4);
    m_jobName->setFont(jobFont);
    m_jobName->setWordWrap(true);
    m_jobInfo = new QLabel(QString(), jobCard);
    m_jobInfo->setStyleSheet(QStringLiteral("color: #a3abb8;"));
    bv->addWidget(m_jobName);
    bv->addWidget(m_jobInfo);
    m_progress = new QProgressBar(jobCard);
    m_progress->setTextVisible(true);
    m_progress->setMinimumHeight(22);
    bv->addWidget(m_progress);
    bv->addStretch(1);
    m_runBtn = new QPushButton(QStringLiteral("▶  Start"), jobCard);
    m_runBtn->setObjectName(QStringLiteral("start"));
    m_runBtn->setMinimumHeight(64);
    bv->addWidget(m_runBtn);
    auto *runRow = new QHBoxLayout;
    m_pauseBtn = bigBtn(new QPushButton(QStringLiteral("⏸ Hold"), jobCard), 52);
    // Cancel job: hold, ask, then reset standing still and lift - the job
    // ends with the position kept. Stop stays the instant reset.
    auto *cancelBtn = bigBtn(new QPushButton(QStringLiteral("✕ Cancel job"), jobCard), 52);
    cancelBtn->setToolTip(QStringLiteral("Hold the machine, then end the job: spindle off and Z "
                                         "up to the top, position kept"));
    connect(cancelBtn, &QPushButton::clicked, this, [this] { cancelJob(); });
    m_stopBtn = new QPushButton(QStringLiteral("⏹ Stop"), jobCard);
    m_stopBtn->setObjectName(QStringLiteral("stop"));
    m_stopBtn->setMinimumHeight(52);
    m_stopBtn->setToolTip(QStringLiteral("Reset the controller at once (emergency). Moving, it "
                                         "loses the position: Initialize afterwards"));
    runRow->addWidget(m_pauseBtn);
    runRow->addWidget(cancelBtn);
    runRow->addWidget(m_stopBtn);
    bv->addLayout(runRow);
    // "More": feed override and air cut, folded so the big buttons stay big.
    auto *moreToggle = new QToolButton(jobCard);
    moreToggle->setCheckable(true);
    moreToggle->setAutoRaise(true);
    moreToggle->setArrowType(Qt::RightArrow);
    moreToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    moreToggle->setText(QStringLiteral("Feed override, air cut, surface map"));
    moreToggle->setFocusPolicy(Qt::NoFocus);
    bv->addWidget(moreToggle);
    auto *more = new QWidget(jobCard);
    auto *mv = new QVBoxLayout(more);
    mv->setContentsMargins(0, 0, 0, 0);
    mv->setSpacing(6);
    // GRBL realtime feed override — takes effect mid-program, no queue flush.
    auto *ovRow = new QHBoxLayout;
    ovRow->addWidget(new QLabel(QStringLiteral("feed ovr"), more));
    auto ovBtn = [&](const QString &t, char code) {
        auto *b = new QPushButton(t, more);
        b->setMinimumSize(60, 34);
        ovRow->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, code] { m_grbl->sendRealtime(code); });
    };
    ovBtn(QStringLiteral("−10%"), char(0x92));
    ovBtn(QStringLiteral("100%"), char(0x90));
    ovBtn(QStringLiteral("+10%"), char(0x91));
    ovRow->addStretch(1);
    mv->addLayout(ovRow);
    auto *airRow = new QHBoxLayout;
    m_airCut = new QCheckBox(QStringLiteral("Air cut"), more);
    m_airCut->setToolTip(QStringLiteral(
        "Rehearse: spindle commands removed, every Z raised by the lift amount"));
    m_airLift = mmSpin(more, 0, 100, 10, 1, QStringLiteral(" mm lift"));
    m_airLift->setEnabled(false);
    connect(m_airCut, &QCheckBox::toggled, m_airLift, &QWidget::setEnabled);
    airRow->addWidget(m_airCut);
    airRow->addWidget(m_airLift);
    airRow->addStretch(1);
    mv->addLayout(airRow);
    // Surface map, measured on the BitZero page; on or off for each job.
    auto *mapRow = new QHBoxLayout;
    m_useMap = new QCheckBox(QStringLiteral("Surface map"), more);
    m_useMap->setToolTip(QStringLiteral(
        "Raise or lower every Z by the measured surface at its XY, so the job "
        "follows a table or board that is not flat. Measure it under Probe zero "
        "(BitZero) → Surface map."));
    m_mapLabel = new QLabel(more);
    m_mapLabel->setStyleSheet(QStringLiteral("color: #8b93a1; font-size: 12px;"));
    m_mapLabel->setWordWrap(true);
    mapRow->addWidget(m_useMap);
    mapRow->addWidget(m_mapLabel, 1);
    mv->addLayout(mapRow);
    connect(m_useMap, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_loading)
            QSettings().setValue(QStringLiteral("machine/heightmap/use"), on);
    });
    more->hide();
    bv->addWidget(more);
    connect(moreToggle, &QToolButton::toggled, this, [more, moreToggle](bool on) {
        more->setVisible(on);
        moreToggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
    });
    auto *rightCol = new QVBoxLayout;
    rightCol->setSpacing(12);
    rightCol->addWidget(connCard);
    rightCol->addWidget(jobCard, 1);
    cols->addLayout(rightCol, 9);

    // ---- Machine actions ---------------------------------------------------
    auto *foot = new QHBoxLayout;
    foot->setSpacing(8);
    auto *homeBtn = bigBtn(new QPushButton(QStringLiteral("Initialize"), body), 44);
    homeBtn->setToolTip(QStringLiteral("Run the homing cycle ($H), then park"));
    connect(homeBtn, &QPushButton::clicked, this, [this] { initializeMachine(); });
    auto *parkBtn = bigBtn(new QPushButton(QStringLiteral("Park front right"), body), 44);
    parkBtn->setToolTip(QStringLiteral(
        "Raise to safe Z and rapid to the front-right corner, where bits are "
        "changed. Initialize does this after homing."));
    connect(parkBtn, &QPushButton::clicked, this, [this] { park(); });
    auto *unlockBtn = bigBtn(new QPushButton(QStringLiteral("Unlock"), body), 44);
    unlockBtn->setToolTip(QStringLiteral("Clear alarm lock ($X)"));
    connect(unlockBtn, &QPushButton::clicked, this, [this] { unlock(); });
    auto *bsBtn = bigBtn(new QPushButton(QStringLiteral("BitSetter"), body), 44);
    bsBtn->setToolTip(QStringLiteral("BitSetter position, probe feeds, tool-change spot, "
                                     "reference and rehearsal"));
    foot->addWidget(homeBtn);
    foot->addWidget(parkBtn);
    foot->addWidget(unlockBtn);
    foot->addWidget(bsBtn);
    foot->addStretch(1);
    lay->addLayout(foot);

    // ---- BitSetter + tool change (a page of the right-hand card) -----------
    auto *bsPage = new QWidget(m_rightStack);
    m_bsPage = bsPage;
    auto *dv = new QVBoxLayout(bsPage);
    dv->setContentsMargins(18, 12, 18, 16);
    dv->setSpacing(10);
    // On / off as one wide toggle beside Back; everything below dims when off.
    auto *bsOnBox = new QWidget(bsPage);
    bsOnBox->setObjectName(QStringLiteral("stepBox"));
    bsOnBox->setAttribute(Qt::WA_StyledBackground);
    bsOnBox->setStyleSheet(QStringLiteral("#stepBox { background: #16181d; border-radius: 12px; }"));
    auto *bsOnLay = new QHBoxLayout(bsOnBox);
    bsOnLay->setContentsMargins(5, 5, 5, 5);
    m_bsEnabled = new QToolButton(bsOnBox);
    m_bsEnabled->setObjectName(QStringLiteral("step"));
    m_bsEnabled->setText(QStringLiteral("BitSetter on"));
    m_bsEnabled->setCheckable(true);
    m_bsEnabled->setMinimumHeight(40);
    m_bsEnabled->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_bsEnabled->setFocusPolicy(Qt::NoFocus);
    m_bsEnabled->setToolTip(QStringLiteral(
        "Measure tool lengths on the BitSetter at every tool change. Positions are "
        "MACHINE coordinates (G53) — they stay valid when work zero moves."));
    bsOnLay->addWidget(m_bsEnabled);
    {
        auto *top = new QHBoxLayout;
        top->setSpacing(10);
        auto *back = new QPushButton(QStringLiteral("← Back to job"), bsPage);
        back->setObjectName(QStringLiteral("big"));
        back->setMinimumHeight(40);
        back->setFocusPolicy(Qt::NoFocus);
        connect(back, &QPushButton::clicked, this,
                [this] { m_rightStack->setCurrentWidget(m_jobPage); });
        top->addWidget(back);
        top->addWidget(bsOnBox, 1);
        dv->addLayout(top);
    }
    m_bsBody = new QWidget(bsPage);
    auto *bb = new QVBoxLayout(m_bsBody);
    bb->setContentsMargins(0, 0, 0, 0);
    bb->setSpacing(8);
    dv->addWidget(m_bsBody);
    dv->addStretch(1);
    connect(m_bsEnabled, &QToolButton::toggled, m_bsBody, &QWidget::setEnabled);
    auto xyRow = [&](const QString &title, QDoubleSpinBox *&x, QDoubleSpinBox *&y,
                     double dx, double dy, const QString &useTip) {
        bb->addWidget(caption(title, m_bsBody));
        auto *row = new QHBoxLayout;
        row->setSpacing(6);
        x = mmSpin(m_bsBody, -5000, 5000, dx, 3, QString());
        y = mmSpin(m_bsBody, -5000, 5000, dy, 3, QString());
        auto *lx = new QLabel(QStringLiteral("X"), m_bsBody);
        auto *ly = new QLabel(QStringLiteral("Y"), m_bsBody);
        lx->setObjectName(QStringLiteral("axis"));
        ly->setObjectName(QStringLiteral("axis"));
        auto *use = bigBtn(new QPushButton(QStringLiteral("Use here"), m_bsBody), 40);
        use->setToolTip(useTip);
        row->addWidget(lx);
        row->addWidget(x, 1);
        row->addWidget(ly);
        row->addWidget(y, 1);
        row->addWidget(use);
        bb->addLayout(row);
        return use;
    };
    QPushButton *bsUse = xyRow(QStringLiteral("Button position"), m_bsX, m_bsY, -20, -20,
                               QStringLiteral("Jog the bit over the BitSetter button, then click"));
    connect(bsUse, &QPushButton::clicked, this, [this] {
        const MachineStatus &st = m_grbl->status();
        if (!st.valid || !requireHomed(QStringLiteral("Setting the BitSetter position"))) return;
        m_bsX->setValue(st.mx);
        m_bsY->setValue(st.my);
        saveSettings();
    });
    QPushButton *tcUse = xyRow(QStringLiteral("Tool-change spot"), m_tcX, m_tcY, -120, -20,
                               QStringLiteral("Where the spindle parks for changing bits"));
    connect(tcUse, &QPushButton::clicked, this, [this] {
        const MachineStatus &st = m_grbl->status();
        if (!st.valid || !requireHomed(QStringLiteral("Setting the tool-change spot"))) return;
        m_tcX->setValue(st.mx);
        m_tcY->setValue(st.my);
        saveSettings();
    });
    bb->addWidget(caption(QStringLiteral("Travel and probing"), m_bsBody));
    auto *bsZRow = new QHBoxLayout;
    bsZRow->setSpacing(6);
    auto *zLbl = new QLabel(QStringLiteral("safe Z"), m_bsBody);
    zLbl->setObjectName(QStringLiteral("field"));
    m_bsSafeZ = mmSpin(m_bsBody, -1000, 0, -5, 2);
    m_bsSafeZ->setToolTip(QStringLiteral("Machine Z used for all travel to / from the button"));
    m_bsFast = mmSpin(m_bsBody, 10, 3000, 600, 0, QStringLiteral(" fast"));
    m_bsFast->setToolTip(QStringLiteral("First touch, mm/min"));
    m_bsSlow = mmSpin(m_bsBody, 5, 500, 50, 0, QStringLiteral(" slow"));
    m_bsSlow->setToolTip(QStringLiteral("Measuring touch, mm/min"));
    bsZRow->addWidget(zLbl);
    bsZRow->addWidget(m_bsSafeZ, 1);
    bsZRow->addWidget(m_bsFast, 1);
    bsZRow->addWidget(m_bsSlow, 1);
    bb->addLayout(bsZRow);
    m_measureBtn = bigBtn(new QPushButton(QStringLiteral("Measure tool"), m_bsBody), 46);
    m_measureBtn->setToolTip(measureBig->toolTip());
    auto *rehearse = bigBtn(new QPushButton(QStringLiteral("Rehearse tool change"), m_bsBody), 46);
    rehearse->setToolTip(QStringLiteral(
        "Run one tool change on its own, spindle off: park at the tool-change spot, "
        "prompt, measure on the BitSetter, apply the offset. The same sequence a job "
        "runs, to check it on the machine before trusting it with a cut."));
    auto *clearRef = bigBtn(new QPushButton(QStringLiteral("Clear reference"), m_bsBody), 46);
    auto *bsBtns = new QGridLayout;
    bsBtns->setSpacing(8);
    bsBtns->addWidget(m_measureBtn, 0, 0);
    bsBtns->addWidget(clearRef, 0, 1);
    bsBtns->addWidget(rehearse, 1, 0, 1, 2);
    bb->addLayout(bsBtns);
    connect(rehearse, &QPushButton::clicked, this, [this] { rehearseToolChange(); });
    connect(m_measureBtn, &QPushButton::clicked, this, [this] { measureTool(Phase::MeasureManual); });
    connect(clearRef, &QPushButton::clicked, this, [this] {
        m_haveRef = false;
        m_grbl->applyToolLengthOffset(0);
        saveSettings();
        updateOffsetLabels();
    });
    m_refLabel = new QLabel(m_bsBody);
    m_tloLabel = new QLabel(m_bsBody);
    for (QLabel *l : {m_refLabel, m_tloLabel}) {
        l->setStyleSheet(QStringLiteral("color: #8b93a1; font-family: monospace; font-size: 12px;"));
        l->setWordWrap(true);
        bb->addWidget(l);
    }
    for (QDoubleSpinBox *s : {m_bsX, m_bsY, m_bsSafeZ, m_bsFast, m_bsSlow, m_tcX, m_tcY})
        connect(s, &QDoubleSpinBox::valueChanged, this, [this] { saveSettings(); });
    connect(m_bsEnabled, &QToolButton::toggled, this, [this] { saveSettings(); });
    m_bsEnabled->setChecked(false);
    m_bsBody->setEnabled(false);
    m_rightStack->addWidget(bsPage);
    connect(bsBtn, &QPushButton::clicked, this,
            [this, bsPage] { m_rightStack->setCurrentWidget(bsPage); });

    // ---- BitZero probing (another page of the card) ------------------------
    m_bzPanel = new BitZeroPanel(m_rightStack);
    m_rightStack->addWidget(m_bzPanel);
    connect(m_bzPanel, &BitZeroPanel::beginRequested, this, [this] { bitZeroBegin(); });
    connect(m_bzPanel, &BitZeroPanel::backRequested, this,
            [this] { m_rightStack->setCurrentWidget(m_jobPage); });

    // ---- Surface map (run from the BitZero page) ---------------------------
    m_mapper = new SurfaceMapProber(m_grbl, this);
    connect(m_mapper, &SurfaceMapProber::waitingForPlate, this, [this](int i, int n) {
        m_bzPanel->setBusy(false);
        m_bzPanel->setMapWaiting(i, n);
        log(QStringLiteral("surface map: over point %1 of %2. Put the plate under the "
                           "tool, then press Probe point").arg(i).arg(n));
    });
    connect(m_mapper, &SurfaceMapProber::probing, this, [this](int, int) {
        m_bzPanel->setBusy(true);
    });
    connect(m_mapper, &SurfaceMapProber::pointMeasured, this,
            [this](int i, int n, double x, double y, double z) {
        log(QStringLiteral("surface map: point %1 of %2 at X%3 Y%4 → stock top machine Z%5")
                .arg(i).arg(n).arg(x, 0, 'f', 1).arg(y, 0, 'f', 1).arg(z, 0, 'f', 3));
    });
    connect(m_mapper, &SurfaceMapProber::finished, this, [this](const SurfaceMap &map) {
        m_bzPanel->setMapWaiting(0, 0);
        m_bzPanel->setBusy(false);
        setSurfaceMap(map);
        m_useMap->setChecked(true);
        log(QStringLiteral("surface map: done, %1. It is on for the job; remove the plate "
                           "and the magnet before cutting").arg(map.summary()));
    });
    connect(m_mapper, &SurfaceMapProber::failed, this, [this](const QString &why) {
        log(QStringLiteral("!! surface map: %1").arg(why));
        if (!m_mapper->isRunning())
            m_bzPanel->setMapWaiting(0, 0);
        m_bzPanel->setBusy(false);
    });
    connect(m_bzPanel, &BitZeroPanel::cancelMapRequested, this, [this] {
        m_mapper->cancel();
        m_bzPanel->setMapWaiting(0, 0);
        m_bzPanel->setBusy(false);
        log(QStringLiteral("surface map: stopped; the map in use is unchanged"));
    });

    // ---- Program -----------------------------------------------------------
    connect(m_runBtn, &QPushButton::clicked, this, [this] {
        if (!m_grbl->isParkedForTool()) {
            runProgram();
            return;
        }
        if (m_grbl->isMacroRunning() || m_phase != Phase::Idle) {
            log(QStringLiteral("== still moving — wait for it to finish, then Continue =="));
            return;
        }
        // The park "finishes" when its last move is acked, which is when
        // the move starts; the prompt comes up once the machine stands at
        // the tool-change spot. Resuming before it skipped the prompt (and
        // so the new bit) and opened it later, in the middle of the cut.
        if (!m_toolPromptAnswered) {
            if (m_onIdle) {
                log(QStringLiteral("== still on the way to the tool-change spot — answer the "
                                   "tool prompt there first =="));
                return;
            }
            // The park did not get there (an alarm on the way): go again,
            // and the prompt follows.
            log(QStringLiteral("parking for the tool change again"));
            parkForTool();
            return;
        }
        // GRBL refuses G-code while a jog runs (error:9): the rise to the
        // top would be rejected and the job aborted.
        if (!standingStill()) {
            log(QStringLiteral("== %1 — wait until it stands still, then Continue ==")
                    .arg(stillMovingWhy()));
            return;
        }
        // BitSetter off: nothing measures the new tool; the operator either
        // said it has the same length, or closed the prompt to zero Z.
        if (!m_bsEnabled->isChecked()) {
            if (m_manualZero && !m_zeroedForTool) {
                ShopAsk a;
                a.title = QStringLiteral("Z not zeroed");
                a.headline = QStringLiteral("Z has not been zeroed for T%1").arg(m_pendingTool);
                a.body = QStringLiteral("Jog the new bit down and press Zero beside Z (or BitZero, "
                                        "Z only) before continuing.");
                a.note = QStringLiteral("Continuing keeps the previous tool's Z zero, so the "
                                        "program will cut as deep as the two tools differ in "
                                        "length.");
                a.noteDanger = true;
                a.okText = QStringLiteral("Continue anyway");
                a.okDanger = true;
                a.defaultOk = false;
                if (!shopAsk(this, a) || !m_grbl->isParkedForTool())
                    return;
            }
            continueAfterToolChange();
            return;
        }
        // Continuing without a successful measurement resumes with the
        // *previous* tool's G43.1 still applied: a tool 10 mm longer then
        // cuts 10 mm deeper on its first plunge. The message after a failed
        // probe points at this very button, so say it plainly here.
        if (!m_toolMeasured && !(m_manualZero && m_zeroedForTool)) {
            ShopAsk a;
            a.title = QStringLiteral("Tool not measured");
            a.headline = QStringLiteral("T%1 has not been measured").arg(m_pendingTool);
            a.body = QStringLiteral("The BitSetter did not measure the new tool.");
            a.note = QStringLiteral("Continuing keeps the previous tool's length offset, so "
                                    "the program will cut as deep as the two tools differ "
                                    "in length.");
            a.noteDanger = true;
            a.okText = QStringLiteral("Continue anyway");
            a.okDanger = true;
            a.defaultOk = false;
            if (!shopAsk(this, a) || !m_grbl->isParkedForTool())
                return;
        }
        continueAfterToolChange();
    });
    connect(m_pauseBtn, &QPushButton::clicked, this, [this] {
        // A refused resume (reset after an error, port gone) leaves the
        // machine held, and the button must keep saying so.
        if (!m_held) {
            m_grbl->pauseStream();
            setHeld(true);
        } else if (m_resetPending) {
            // Cancel job / Disconnect is waiting for the hold to settle to
            // reset: resuming would cut on, then reset in the middle of it.
            log(QStringLiteral("resume refused: the job is being cancelled"));
        } else if (m_grbl->resumeStream()) {
            setHeld(false);
        }
    });
    connect(m_stopBtn, &QPushButton::clicked, this, [this] {
        ++m_stopSeq;
        resetFlow();
        m_grbl->stopStream();
    });

    // The log sits in the Connect card under the port row and folds away to
    // one header row; the last line stays visible there, so an alarm is still
    // seen with the log closed. The header stays put and the log opens below it.
    auto *logHead = new QHBoxLayout;
    logHead->setContentsMargins(0, 4, 0, 0);
    m_logToggle = new QToolButton(this);
    m_logToggle->setCheckable(true);
    m_logToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_logToggle->setArrowType(Qt::RightArrow);
    m_logToggle->setText(QStringLiteral("Log"));
    m_logToggle->setAutoRaise(true);
    m_logToggle->setFocusPolicy(Qt::NoFocus);
    m_logLast = new QLabel(this);
    m_logLast->setStyleSheet(QStringLiteral("color: #8b93a1; font-family: monospace;"));
    m_logLast->setTextInteractionFlags(Qt::NoTextInteraction);
    m_logLast->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    logHead->addWidget(m_logToggle);
    logHead->addWidget(m_logLast, 1);
    connLay->addLayout(logHead);
    m_console = new QPlainTextEdit(this);
    m_console->setReadOnly(true);
    m_console->setMaximumBlockCount(800);
    m_console->setFixedHeight(m_console->fontMetrics().lineSpacing() * 8 + 12);
    connLay->addWidget(m_console, 0);
    auto showLog = [this](bool on) {
        m_console->setVisible(on);
        m_logToggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        m_logLast->setVisible(!on);
        QSettings().setValue(QStringLiteral("machine/logOpen"), on);
    };
    connect(m_logToggle, &QToolButton::toggled, this, showLog);
    const bool logOpen = QSettings().value(QStringLiteral("machine/logOpen"), false).toBool();
    m_logToggle->setChecked(logOpen);
    showLog(logOpen);

    // ---- Streamer feedback -----------------------------------------------
    connect(m_grbl, &GrblStreamer::connected, this, [this](const QString &p) {
        m_connectBtn->setText(QStringLiteral("Disconnect"));
        setStatePill(QStringLiteral("Connected to %1 — waiting for status").arg(p), QStringLiteral("#2a2e36"));
        m_grbl->sendCommand(QString());   // wake newline
        m_homed = false;                  // until a homing cycle on this connection
        m_unhomedWhy = QStringLiteral("not homed since connecting");
        m_resumeCheck = true;             // ...or the controller is where we left it
        m_holdChecked = false;
        m_homingStarted = false;
        m_lastLimitPins.clear();
        // Travel ($130-$132) for the jog limits, and $21 (hard limits).
        m_settingsAsked.start();
        QTimer::singleShot(600, this, [this] { m_grbl->sendCommand(QStringLiteral("$$")); });
        saveSettings();
    });
    connect(m_grbl, &GrblStreamer::disconnected, this, [this] {
        ++m_stopSeq;
        resetFlow();
        stopHoldJog();
        m_deferredJogs.clear();
        m_connectBtn->setText(QStringLiteral("Connect"));
        setStatePill(QStringLiteral("Not connected"), QStringLiteral("#2a2e36"));
        for (QLabel *l : m_wAxis)
            l->setText(QStringLiteral("—"));
        m_mpos->setText(QStringLiteral("machine —"));
        m_extra->clear();
        m_machineState.clear();
        m_initBanner->hide();
        if (m_bzPanel) {
            m_bzPanel->setContact(false, false);
            m_bzPanel->setConnected(false);
            m_bzPanel->setBusy(false);
        }
        emit livePosition(0, 0, 0, false);
    });
    connect(m_grbl, &GrblStreamer::statusChanged, this, [this](const MachineStatus &st) { onStatus(st); });
    connect(m_grbl, &GrblStreamer::progressChanged, this, [this](int a, int t) {
        m_progress->setMaximum(qMax(1, t));
        m_progress->setValue(a);
    });
    connect(m_grbl, &GrblStreamer::consoleLine, this, [this](const QString &l) {
        log(l);
        // Homing failed on a switch. On the Shapeoko 5 the second Y sensor
        // (back left, `A` in the Pn: field) is the usual culprit, and while
        // it is out the gantry grinds in the corner until GRBL gives up.
        // A probe that found nothing (ALARM:4/5) keeps its position: GRBL
        // only wants $X, not a new homing cycle. Send it, say so.
        // The controller (re)started: a reset, or a board that reboots when
        // the port opens and was still booting when the first $$ went out.
        // Ask again, or the jog limits, park spot and $21 fall back to
        // defaults for the whole session.
        if (l.startsWith(QLatin1String("Grbl "))) {
            // Nothing from before the reset moves the machine after it.
            ++m_resets;
            m_onIdle = nullptr;
            m_deferredJogs.clear();
            stopHoldJog();
            QTimer::singleShot(400, this, [this] { m_grbl->sendCommand(QStringLiteral("$$")); });
        }
        if (l.startsWith(QLatin1String("ALARM:4")) || l.startsWith(QLatin1String("ALARM:5"))) {
            log(QStringLiteral("!! the probe did not touch anything within its travel. "
                               "Position is kept — unlocking ($X); no need to Initialize. "
                               "Jog closer and probe again."));
            QTimer::singleShot(300, this, [this] { m_grbl->sendCommand(QStringLiteral("$X")); });
        }
        // Every other alarm (hard/soft limit, reset while moving, homing
        // failures) loses the position: no machine-coordinate move until the
        // next homing cycle.
        if (l.startsWith(QLatin1String("ALARM:"))
            && !l.startsWith(QLatin1String("ALARM:4")) && !l.startsWith(QLatin1String("ALARM:5"))) {
            m_homed = false;
            static const QHash<QString, QString> why = {
                {QStringLiteral("1"), QStringLiteral("an end stop fired (ALARM:1)")},
                {QStringLiteral("2"), QStringLiteral("a move went past the travel (ALARM:2)")},
                {QStringLiteral("3"), QStringLiteral("the controller was reset while moving (ALARM:3)")}};
            const QString n = l.mid(6, 1);
            m_unhomedWhy = why.value(n, QStringLiteral("homing failed (%1)").arg(l.trimmed()));
            m_homingStarted = false;
            forgetJogPlan();
            m_onIdle = nullptr;   // a step waiting for Idle (BitZero's last) is void now
        }
        // Hard limits: the end stops stop all motion in the controller
        // itself. Shipped off on the Shapeoko; turned on here, once.
        if (l == QLatin1String("$21=0")) {
            log(QStringLiteral("turning hard limits on ($21=1): an end stop now stops all "
                               "motion at once"));
            m_grbl->sendCommand(QStringLiteral("$21=1"));
            QTimer::singleShot(400, this, [this] { m_grbl->sendCommand(QStringLiteral("$$")); });
        }
        if (l.startsWith(QLatin1String("ALARM:8")) || l.startsWith(QLatin1String("ALARM:9"))) {
            m_parkAfterHome = false;
            log(QStringLiteral("!! homing could not find a limit switch. Check that every "
                               "homing sensor's LED lights when metal is in front of it — "
                               "on the Shapeoko 5 both Y sensors, including the back-left one."));
        }
    });
    connect(m_grbl, &GrblStreamer::errorOccurred, this, [this](const QString &e) {
        log(QStringLiteral("!! %1").arg(e));
    });
    // A manual zero for the new bit counts once the controller took it:
    // refused (error:9 while still jogging, say), Z still belongs to the
    // old bit and Continue has to say so.
    connect(m_grbl, &GrblStreamer::commandAnswered, this, [this](const QString &line, bool ok) {
        if (!m_awaitToolZero || !line.startsWith(QLatin1String("G10 L20"))
            || !line.contains(QChar('Z')))
            return;
        m_awaitToolZero = false;
        if (!m_manualZero || !m_grbl->isParkedForTool())
            return;
        if (ok) {
            m_zeroedForTool = true;
            log(QStringLiteral("Z zeroed for the new tool — press Continue to resume the job"));
        } else {
            log(QStringLiteral("!! the controller refused the Z zero — Z is still the previous "
                               "tool's. Wait until the machine stands still and zero again"));
        }
    });
    // A refused $H (error:5 with homing disabled, say) never homes: without
    // this the pending park blocked every jog until Stop or a disconnect.
    connect(m_grbl, &GrblStreamer::commandAnswered, this, [this](const QString &line, bool ok) {
        if (ok || line != QLatin1String("$H") || !m_parkAfterHome)
            return;
        m_parkAfterHome = false;
        m_homingStarted = false;
        log(QStringLiteral("!! the controller refused the homing cycle — no park follows"));
    });
    connect(m_grbl, &GrblStreamer::probeResult, this,
            [this](double x, double y, double z, bool ok) {
        log(QStringLiteral("probe %1 at machine X%2 Y%3 Z%4")
                .arg(ok ? QStringLiteral("contact") : QStringLiteral("FAILED"))
                .arg(x, 0, 'f', 3).arg(y, 0, 'f', 3).arg(z, 0, 'f', 3));
        const bool bz = m_phase == Phase::BitZeroX || m_phase == Phase::BitZeroY
                        || m_phase == Phase::BitZeroZ;
        if (bz && ok)
            m_hits.append({x, y, z});
        else if (bz) {
            // A G38.3 search ran its whole travel without touching: stop
            // here, before the back-off and the slow touch that follow.
            log(QStringLiteral("!! BitZero: nothing within the search travel — stopping"));
            m_grbl->cancelMacro();
        }
    });
    connect(m_grbl, &GrblStreamer::macroFinished, this, [this](bool ok) { onMacroFinished(ok); });
    connect(m_grbl, &GrblStreamer::toolChangeRequested, this,
            [this](int t, int k) { onToolChange(t, k); });
    connect(m_grbl, &GrblStreamer::streamFinished, this, [this](bool ok) {
        const bool rehearsal = m_rehearsal;
        const bool measured = m_toolMeasured;
        log(ok ? QStringLiteral("== program finished ==")
               : QStringLiteral("== program stopped =="));
        const bool endJob = m_endOfJobPending && ok && !rehearsal;
        m_endOfJobPending = false;
        const bool wasHeld = m_held;
        resetFlow();
        // Held in the last seconds: the last lines went into the planner
        // and the stream is done, but the machine still holds (spindle on,
        // moves queued). The button keeps saying Resume. A stopped program
        // was reset, which ends any hold.
        if (ok && (wasHeld || m_machineState.startsWith(QLatin1String("Hold"))))
            setHeld(true);
        // From the event loop: this runs inside the serial read, and the
        // summary's dialog would stop status and alarms being read.
        if (rehearsal)
            QTimer::singleShot(0, this, [this, ok, measured] { finishRehearsal(ok, measured); });
        // A finished job: retract to the top of Z, then stop the spindle,
        // the way Carbide Motion ends a run. A stopped one was reset, which
        // already killed the spindle.
        if (endJob) {
            log(QStringLiteral("job done: up to safe Z, then spindle off"));
            // Once the machine stands still: the last lines are acked when
            // planned, so the position read now is from before the program's
            // own final retract, and a rise from it could go down.
            m_endRisePending = true;
            whenIdle([this] {
                m_endRisePending = false;
                m_grbl->startMacro({QStringLiteral("G90"),
                                    QStringLiteral("G53 G0 Z%1").arg(riseZ(), 0, 'f', 3),
                                    QStringLiteral("M5")});
            });
        }
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
    {
        const SurfaceMap saved = SurfaceMap::fromJson(
            QSettings().value(QStringLiteral("machine/heightmap/map")).toString());
        m_loading = true;
        m_map = saved;
        m_useMap->setChecked(saved.isValid()
                             && QSettings().value(QStringLiteral("machine/heightmap/use"), false).toBool());
        m_loading = false;
        refreshMapUi();
    }
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
        // A release we will never see. A held jog stops the way a release
        // stops it (with the second cancel for a $J= still in GRBL's buffer);
        // a press not yet held is dropped, not taken as a tap.
        if (m_holdJogging) endHoldJog(); else stopHoldJog();
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

// Everything the log shows also goes to <AppLocalData>/machine.log with a
// time stamp, so what happened at the machine can be read afterwards.
void MachinePanel::log(const QString &line)
{
    m_console->appendPlainText(line);
    if (m_logLast)
        m_logLast->setText(line);
    static QFile file;
    if (!file.isOpen()) {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        QDir().mkpath(dir);
        file.setFileName(dir + QStringLiteral("/machine.log"));
        file.open(QIODevice::Append | QIODevice::Text);
    }
    if (file.isOpen()) {
        file.write(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz  ")).toUtf8());
        file.write(line.toUtf8());
        file.write("\n");
        file.flush();
    }
}

QPushButton *MachinePanel::jogButton(const QString &text, char axis, int dir)
{
    auto *b = new QPushButton(text, this);
    b->setFixedSize(40, 32);
    b->setFocusPolicy(Qt::NoFocus);          // keep keyboard jogging on the panel
    connect(b, &QPushButton::pressed, this, [this, axis, dir] {
        setFocus();                       // arrow keys jog from here on
        beginHoldJog(axis, dir);
    });
    connect(b, &QPushButton::released, this, [this] { endHoldJog(); });
    return b;
}

double MachinePanel::jogFeed(char axis) const
{
    // GRBL clamps a jog to $110-$112 anyway; Z is kept well under its 3000.
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
    // GRBL acks a $J= as soon as it is in the planner, so the limit above
    // never bites while the planner has room: it filled to 15 blocks, seconds
    // of motion if a release were lost. Stay at most two increments (about
    // four ticks, enough to cover the status lag) ahead of where the last
    // report saw the machine.
    const int i = m_holdAxis == 'X' ? 0 : m_holdAxis == 'Y' ? 1 : 2;
    const MachineStatus &st = m_grbl->status();
    if (st.valid) {
        const double pos = i == 0 ? st.mx : i == 1 ? st.my : st.mz;
        double ahead = 0;
        if (m_homed && m_jogPlanValid) {
            ahead = std::abs(m_jogPlan[i] - pos);
        } else if (!m_homed) {
            if (!m_holdOriginValid) {
                m_holdOrigin = pos;
                m_holdSent = 0;
                m_holdOriginValid = true;
            }
            ahead = std::abs(m_holdOrigin + m_holdSent - pos);
        }
        if (ahead > 2.0 * mm)
            return;
    }
    if (jogBy(m_holdAxis, m_holdDir * mm, f, false) && !m_homed)
        m_holdSent += m_holdDir * mm;
}

void MachinePanel::jogStep(char axis, int dir)
{
    // Rapid taps of a big step while GRBL's planner is full: its acks stop,
    // and every further 28-byte $J= goes into a 128-byte RX buffer. A lost
    // character corrupts a jog line (a dropped minus is a jog the wrong
    // way), so a tap is refused while two jogs are still unanswered or
    // waiting for a fresh position.
    if (m_grbl->pendingCommands() >= 2 || m_deferredJogs.size() >= 2) {
        log(QStringLiteral("jog tap ignored: the controller has not caught up yet"));
        return;
    }
    const double s = m_step->currentText().toDouble();
    jogBy(axis, dir * s, jogFeed(axis));
}

void MachinePanel::forgetJogPlan()
{
    m_jogPlanValid = false;
    m_jogSeedOk = false;
    m_jogFreshAfter = m_statusSeq + 2;
}

bool MachinePanel::jogBy(char axis, double delta, double feed, bool defer)
{
    // A jog sent while homing waits in GRBL's buffer and runs after it,
    // unlimited (a + tap into the switch), and its answer let the tool
    // offset go out mid-move to be refused.
    if (m_parkAfterHome || m_homingStarted || m_machineState.startsWith(QLatin1String("Home"))) {
        log(QStringLiteral("!! jog refused while homing"));
        return false;
    }
    if (m_jogCancelTimer) {
        if (defer)
            m_deferredJogs.append({axis, delta, feed});
        return false;
    }
    const int i = axis == 'X' ? 0 : axis == 'Y' ? 1 : 2;
    const MachineStatus &st = m_grbl->status();
    if (!m_homed || !st.valid) {
        // Not homed: machine coordinates say nothing about where the ends
        // are, so the jog goes as asked.
        if (!m_limitWarned) {
            log(QStringLiteral("!! not homed — jogs are NOT limited to the machine's travel. "
                               "Initialize (home) to turn the limits on."));
            m_limitWarned = true;
        }
        if (!m_grbl->sendCommand(QStringLiteral("$J=G91 %1%2 F%3")
                                     .arg(QChar::fromLatin1(axis)).arg(delta, 0, 'f', 3).arg(feed)))
            return false;
        m_moveFreshAfter = m_statusSeq + 2;
        return true;
    }
    if (!m_jogPlanValid) {
        // An absolute jog from a stale position goes the wrong way: a tap
        // right after letting go of a hold, seeded from a report taken while
        // still decelerating, lands behind the machine. Wait for a fresh one.
        if (!m_jogSeedOk || m_grbl->pendingCommands() > 0) {
            if (defer) {
                m_deferredJogs.append({axis, delta, feed});
                m_grbl->requestStatus();
            }
            return false;
        }
        m_jogPlan[0] = st.mx;
        m_jogPlan[1] = st.my;
        m_jogPlan[2] = st.mz;
        m_jogPlanValid = true;
    }
    // Homed towards + on all three axes: the switch is at 0 and the far end
    // at -travel. Stay out of the switch's trigger zone (pull-off + 1 mm:
    // with hard limits on, entering it is an alarm) and 1 mm off the far end.
    QSettings sset;
    const auto machine = appsettings::machine(sset);
    const double travel[3] = {m_grbl->setting(130, machine.width),
                              m_grbl->setting(131, machine.height),
                              m_grbl->setting(132, 75.0)};
    constexpr double kEdge = 1.0;
    // Z has no floor in manual: the operator may take the bit down through
    // the table (spoilboard surfacing, a deep part). Only its top, the end
    // stop, is limited; X and Y are limited at both ends (switch at one end,
    // a hard stop at the other).
    const double lo = i == 2 ? -1e6 : -travel[i] + kEdge, hi = -switchClearance();
    const double from = m_jogPlan[i];
    const double to = qBound(lo, from + delta, hi);
    if (std::abs(to - from) < 0.001) {
        if (m_holdAxis != axis || !m_holdJogging)
            log(QStringLiteral("%1 is at the end of its travel").arg(QChar::fromLatin1(axis)));
        return false;
    }
    // Refused (a macro owns the line): the plan stays where the machine is.
    if (!m_grbl->sendCommand(QStringLiteral("$J=G53 G90 %1%2 F%3")
                                 .arg(QChar::fromLatin1(axis)).arg(to, 0, 'f', 3).arg(feed))) {
        forgetJogPlan();
        return false;
    }
    m_jogPlan[i] = to;
    m_jogSeedOk = false;
    m_moveFreshAfter = m_statusSeq + 2;
    return true;
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
    if (needsInit())
        return;
    if (m_parkAfterHome || m_homingStarted || m_machineState.startsWith(QLatin1String("Home"))) {
        log(QStringLiteral("!! jog refused while homing"));
        return;
    }
    m_holdAxis = axis;
    m_holdDir = dir;
    m_holdJogging = false;
    m_holdOriginValid = false;
    // Nothing moves on the press. Let go before the hold delay: one step of
    // the set distance (a tap). Hold past it: continuous at the set speed,
    // stopped dead on release (endHoldJog) - never "the rest of the step".
    m_holdTimer->start();
}

void MachinePanel::endHoldJog()
{
    m_holdTimer->stop();
    m_holdRepeat->stop();
    if (m_holdAxis && !m_holdJogging) {
        jogStep(m_holdAxis, m_holdDir);       // a tap: one step
    } else if (m_holdAxis) {
        // Held: stop now. Jog cancel (0x85) empties GRBL's planner, but a $J=
        // still waiting unread in its serial buffer is read after the cancel
        // and would start moving again - the "rest of the step" after letting
        // go. So cancel again once every jog sent has been answered.
        // Jogs sent before that second cancel would be cancelled with it,
        // so jogBy() holds new ones back while it is pending.
        m_grbl->jogCancel();
        forgetJogPlan();
        delete m_jogCancelTimer;
        auto *t = new QTimer(this);
        m_jogCancelTimer = t;
        t->setInterval(15);
        auto waited = std::make_shared<int>(0);
        connect(t, &QTimer::timeout, this, [this, t, waited] {
            if (m_grbl->pendingCommands() > 0 && ++*waited < 70)
                return;
            m_grbl->jogCancel();
            forgetJogPlan();
            t->stop();
            t->deleteLater();
            m_jogCancelTimer = nullptr;
            if (!m_deferredJogs.isEmpty())
                m_grbl->requestStatus();     // the held-back taps go on a fresh report
        });
        t->start();
    }
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
    if (m_holdAxis)                      // a step or a hold may still be moving
        { m_grbl->jogCancel(); forgetJogPlan(); }
    m_holdAxis = 0;
    m_holdJogging = false;
}

void MachinePanel::focusOutEvent(QFocusEvent *event)
{
    stopHoldJog();                        // a release we will never see
    QWidget::focusOutEvent(event);
}

// ---- BitZero V2 ---------------------------------------------------------
//
// The bore of the plate is centred over the stock corner, so each axis is
// found as the middle of its two bore walls; the pin's diameter cancels out.
// The plate top is a known height over the stock, so one touch gives Z.
// Every touch is made twice, Carbide-style: fast to find the wall, back off,
// slow for the number that counts. Only the slow contacts are used.

void MachinePanel::openBitZero()
{
    if (m_doc && !m_mapper->isRunning())
        m_bzPanel->setStockSize(m_doc->boardWidth(), m_doc->boardHeight());
    m_bzPanel->setConnected(m_grbl->isConnected());
    if (m_grbl->status().valid)
        m_bzPanel->setContact(m_grbl->status().probePin, true);
    m_rightStack->setCurrentWidget(m_bzPanel);
}

QStringList MachinePanel::bitZeroWallLines(char axis, const BitZeroConfig &c)
{
    const QChar a = QChar::fromLatin1(axis);
    const QString fast = QString::number(c.fastFeed, 'f', 0);
    const QString slow = QString::number(c.slowFeed, 'f', 0);
    const QString back = QString::number(c.retract, 'f', 3);
    const QString again = QString::number(c.retract + 2.0, 'f', 3);
    const QString far = QString::number(c.search, 'f', 3);
    return {QStringLiteral("G91"),
            // + wall (G38.3: a miss is reported, not an alarm)
            QStringLiteral("G38.3 %1%2 F%3").arg(a, far, fast),
            QStringLiteral("G0 %1-%2").arg(a, back),
            QStringLiteral("G38.2 %1%2 F%3").arg(a, again, slow),
            QStringLiteral("G0 %1-%2").arg(a, back),
            // - wall
            QStringLiteral("G38.3 %1-%2 F%3").arg(a, far, fast),
            QStringLiteral("G0 %1%2").arg(a, back),
            QStringLiteral("G38.2 %1-%2 F%3").arg(a, again, slow),
            QStringLiteral("G0 %1%2").arg(a, back),
            QStringLiteral("G90")};
}

// V1: from the start (a little above the block top, over its circle), go
// sideways past the block, down beside it, and probe back toward it. Ends
// back at the start, at the height the plate probe left (top + 5 mm).
QStringList MachinePanel::bitZeroSideLines(char axis, const BitZeroConfig &c, double startAbs)
{
    const QChar a = QChar::fromLatin1(axis);
    const QString fast = QString::number(c.fastFeed, 'f', 0);
    const QString slow = QString::number(c.slowFeed, 'f', 0);
    const QString back = QString::number(c.retract, 'f', 3);
    const QString again = QString::number(c.retract + 2.0, 'f', 3);
    const QString out = QString::number(c.sideClearance, 'f', 3);
    const QString in = QString::number(c.sideClearance + 5.0, 'f', 3);
    const QString down = QString::number(5.0 + c.sideDepth, 'f', 3);
    return {QStringLiteral("G91"),
            QStringLiteral("G0 %1-%2").arg(a, out),
            QStringLiteral("G0 Z-%1").arg(down),
            QStringLiteral("G38.3 %1%2 F%3").arg(a, in, fast),
            QStringLiteral("G0 %1-%2").arg(a, back),
            QStringLiteral("G38.2 %1%2 F%3").arg(a, again, slow),
            QStringLiteral("G0 %1-%2").arg(a, back),
            QStringLiteral("G0 Z%1").arg(down),
            QStringLiteral("G90"),
            QStringLiteral("G53 G0 %1%2").arg(a).arg(startAbs, 0, 'f', 3)};
}

QStringList MachinePanel::bitZeroPlateLines(const BitZeroConfig &c)
{
    const QString fast = QString::number(c.fastFeed, 'f', 0);
    const QString slow = QString::number(c.slowFeed, 'f', 0);
    return {QStringLiteral("G91"),
            QStringLiteral("G38.3 Z-%1 F%2").arg(c.zTravel, 0, 'f', 3).arg(fast),
            QStringLiteral("G0 Z%1").arg(c.retract, 0, 'f', 3),
            QStringLiteral("G38.2 Z-%1 F%2").arg(c.retract + 2.0, 0, 'f', 3).arg(slow),
            QStringLiteral("G0 Z5"),
            QStringLiteral("G90")};
}

void MachinePanel::bitZeroBegin()
{
    if (!m_grbl->isConnected()) {
        log(QStringLiteral("!! not connected"));
        return;
    }
    if (needsInit())
        return;
    // Probing sets a zero and moves the head about: not in the middle of a
    // job, except a Z touch-off while the operator zeroes a new bit by hand.
    if (m_grbl->isStreaming()
        && !(m_bzPanel->mode() == BitZeroPanel::ZOnly && !m_mapper->isWaiting()
             && zeroAllowed(false))) {
        if (m_bzPanel->mode() != BitZeroPanel::ZOnly)
            log(QStringLiteral("!! BitZero refused while a job is running"));
        return;
    }
    if (!requireHomed(QStringLiteral("BitZero probing")))
        return;
    if (endRiseWaiting()) {
        log(QStringLiteral("!! BitZero: the job is still finishing — wait until the machine stands still"));
        return;
    }
    // A move still waiting for Idle (V1's way back over the block after a
    // failed side probe) would go out after this probe started.
    if (m_onIdle && !m_grbl->isStreaming()) {
        log(QStringLiteral("!! BitZero: a move is still waiting to go out — wait until the machine stands still"));
        return;
    }
    if (!m_grbl->isStreaming() && movingStill(QStringLiteral("BitZero")))
        return;
    if (!m_grbl->canSendCommand()) {
        log(QStringLiteral("!! busy — cannot probe now"));
        return;
    }
    if (m_mapper->isWaiting()) {
        m_bzPanel->setBusy(true);
        m_mapper->probeNext();
        return;
    }
    if (m_bzPanel->mode() == BitZeroPanel::Map) {
        stopHoldJog();
        startSurfaceMap();
        return;
    }
    const MachineStatus st = m_grbl->status();
    if (st.probePin) {
        log(QStringLiteral("!! the probe input is already closed — is the pin touching the "
                           "plate, or the magnet on the plate itself? Move clear and retry"));
        return;
    }
    // Z with the BitSetter on ends in a reference measure on the button.
    if ((m_bzPanel->mode() == BitZeroPanel::Corner || m_bzPanel->mode() == BitZeroPanel::ZOnly)
        && bitSetter().enabled && !bitSetterReachable(bitSetter(), QStringLiteral("BitZero")))
        return;
    stopHoldJog();
    m_bz = m_bzPanel->config();
    m_bzMode = m_bzPanel->mode();
    m_bzWantX = m_bzMode == BitZeroPanel::Corner || m_bzMode == BitZeroPanel::XOnly;
    m_bzWantY = m_bzMode == BitZeroPanel::Corner || m_bzMode == BitZeroPanel::YOnly;
    m_bzWantZ = m_bzMode == BitZeroPanel::Corner || m_bzMode == BitZeroPanel::ZOnly;
    m_bzCx = st.mx;
    m_bzCy = st.my;
    m_bzPanel->setBusy(true);
    log(QStringLiteral("BitZero: probing %1 from machine X%2 Y%3 Z%4")
            .arg(m_bzMode == BitZeroPanel::Corner ? QStringLiteral("the corner (X, Y, Z)")
                 : m_bzMode == BitZeroPanel::ZOnly ? QStringLiteral("Z only")
                 : m_bzMode == BitZeroPanel::XOnly ? QStringLiteral("X only")
                                                    : QStringLiteral("Y only"))
            .arg(st.mx, 0, 'f', 3).arg(st.my, 0, 'f', 3).arg(st.mz, 0, 'f', 3));
    m_bzStartX = st.mx;
    m_bzStartY = st.my;
    // V1 always touches the top first: the side probes descend from there.
    if (m_bz.v1 || !(m_bzWantX || m_bzWantY))
        bitZeroStage(Phase::BitZeroZ);
    else if (m_bzWantX)
        bitZeroStage(Phase::BitZeroX);
    else
        bitZeroStage(Phase::BitZeroY);
}

void MachinePanel::bitZeroStage(Phase stage, QStringList prefix)
{
    m_bzStageResets = m_resets;
    m_hits.clear();
    m_phase = stage;
    QStringList lines = {QStringLiteral("G90")};
    lines += prefix;
    switch (stage) {
    case Phase::BitZeroX:
        lines += m_bz.v1 ? bitZeroSideLines('X', m_bz, m_bzStartX) : bitZeroWallLines('X', m_bz);
        break;
    case Phase::BitZeroY:
        lines += m_bz.v1 ? bitZeroSideLines('Y', m_bz, m_bzStartY) : bitZeroWallLines('Y', m_bz);
        break;
    case Phase::BitZeroZ:
        lines += bitZeroPlateLines(m_bz);
        break;
    default:
        break;
    }
    m_grbl->startMacro(lines);
}

void MachinePanel::bitZeroFail(const QString &why)
{
    log(QStringLiteral("!! BitZero probe failed: %1. Unlock if alarmed, check the magnet "
                       "clip and that the pin sits in the bore, then retry").arg(why));
    m_hits.clear();
    if (m_bzPanel)
        m_bzPanel->setBusy(false);
}

void MachinePanel::bitZeroAfter(Phase stage, bool ok)
{
    // A stage ended by a controller reset reports its failure after the
    // reset was seen: the position is gone, so nothing may move back.
    if (!ok && m_bz.v1 && (stage == Phase::BitZeroX || stage == Phase::BitZeroY)
        && m_resets == m_bzStageResets) {
        // A V1 side probe stops beside the block, below its top: Begin
        // again from there would search Z downward past the block, into
        // the stock or the spoilboard. Once unlocked, back up over the
        // block and over to where Begin was pressed.
        log(QStringLiteral("!! BitZero V1 probe failed: no contact on the block's %1 side. "
                           "Check the magnet clip and that the block sits on the corner; the "
                           "pin goes back up over the block, then Begin again")
                .arg(stage == Phase::BitZeroX ? QStringLiteral("left") : QStringLiteral("front")));
        m_hits.clear();
        const double z = m_bzOverZ, x = m_bzStartX, y = m_bzStartY;
        const quint64 resets = m_resets;
        whenIdle([this, z, x, y, resets] {
            if (resets != m_resets || !m_homed)
                return;
            m_grbl->startMacro({QStringLiteral("G90"),
                                QStringLiteral("G53 G0 Z%1").arg(z, 0, 'f', 3),
                                QStringLiteral("G53 G0 X%1 Y%2").arg(x, 0, 'f', 3).arg(y, 0, 'f', 3)});
        });
        if (m_bzPanel)
            m_bzPanel->setBusy(false);
        return;
    }
    if (!ok) {
        bitZeroFail(stage == Phase::BitZeroZ ? QStringLiteral("no contact on the plate")
                    : stage == Phase::BitZeroHome ? QStringLiteral("could not move back over the corner")
                    : m_bz.v1 ? QStringLiteral("the side probe beside the block stopped")
                                                  : QStringLiteral("no contact on a bore wall"));
        return;
    }
    if (m_bz.v1) {
        bitZeroAfterV1(stage);
        return;
    }
    switch (stage) {
    case Phase::BitZeroX: {
        if (m_hits.size() < 4) {
            bitZeroFail(QStringLiteral("expected four X contacts, got %1").arg(m_hits.size()));
            return;
        }
        const double xp = m_hits.at(1).x, xn = m_hits.at(3).x;
        m_bzCx = (xp + xn) / 2.0;
        log(QStringLiteral("BitZero: X walls at %1 and %2 → bore is %3 mm wide, centre X %4")
                .arg(xp, 0, 'f', 3).arg(xn, 0, 'f', 3).arg(xp - xn, 0, 'f', 2).arg(m_bzCx, 0, 'f', 3));
        const QStringList toCentre = {QStringLiteral("G53 G0 X%1").arg(m_bzCx, 0, 'f', 3)};
        if (m_bzWantY)
            bitZeroStage(Phase::BitZeroY, toCentre);
        else {
            m_phase = Phase::BitZeroHome;
            m_grbl->startMacro(QStringList{QStringLiteral("G90")} + toCentre);
        }
        return;
    }
    case Phase::BitZeroY: {
        if (m_hits.size() < 4) {
            bitZeroFail(QStringLiteral("expected four Y contacts, got %1").arg(m_hits.size()));
            return;
        }
        const double yp = m_hits.at(1).y, yn = m_hits.at(3).y;
        m_bzCy = (yp + yn) / 2.0;
        log(QStringLiteral("BitZero: Y walls at %1 and %2 → bore is %3 mm wide, centre Y %4")
                .arg(yp, 0, 'f', 3).arg(yn, 0, 'f', 3).arg(yp - yn, 0, 'f', 2).arg(m_bzCy, 0, 'f', 3));
        const QStringList toCentre = {QStringLiteral("G53 G0 Y%1").arg(m_bzCy, 0, 'f', 3)};
        if (m_bzWantZ) {
            // Up out of the bore (the pin can be as deep as the plate is
            // high, so lift that plus a margin), over to the plate, down.
            const QStringList over = toCentre + QStringList{
                QStringLiteral("G91 G0 Z%1").arg(m_bz.thickness + 5.0, 0, 'f', 3),
                QStringLiteral("G90"),
                QStringLiteral("G53 G0 X%1 Y%2").arg(m_bzCx + m_bz.zSpotX, 0, 'f', 3)
                                                .arg(m_bzCy + m_bz.zSpotY, 0, 'f', 3)};
            bitZeroStage(Phase::BitZeroZ, over);
        } else {
            m_phase = Phase::BitZeroHome;
            m_grbl->startMacro(QStringList{QStringLiteral("G90")} + toCentre);
        }
        return;
    }
    case Phase::BitZeroZ: {
        if (m_hits.size() < 2) {
            bitZeroFail(QStringLiteral("expected two Z contacts, got %1").arg(m_hits.size()));
            return;
        }
        const double plate = m_hits.at(1).z;
        // Z only: the plate stands on its lips, so its top is the lip drop
        // higher than when the lips hang over the stock edge.
        const double over = m_bz.thickness
                            + (m_bzMode == BitZeroPanel::ZOnly ? m_bz.lipDrop : 0.0);
        m_bzStockZ = plate - over;
        log(QStringLiteral("BitZero: plate top at machine Z %1, %2 mm over the stock → stock top Z %3")
                .arg(plate, 0, 'f', 3).arg(over, 0, 'f', 2).arg(m_bzStockZ, 0, 'f', 3));
        if (m_bzMode == BitZeroPanel::Corner) {
            // Back over the corner - but first all the way up. Moving across
            // at the height the plate probe left (a couple of mm over the
            // plate) skims the pin over the plate, its lip and the magnet.
            m_phase = Phase::BitZeroHome;
            m_grbl->startMacro(riseToTop()
                               + QStringList{QStringLiteral("G90"),
                                             QStringLiteral("G53 G0 X%1 Y%2").arg(m_bzCx, 0, 'f', 3)
                                                                             .arg(m_bzCy, 0, 'f', 3)});
        } else {
            bitZeroApply();
        }
        return;
    }
    case Phase::BitZeroHome:
        bitZeroApply();
        return;
    default:
        return;
    }
}

// V1 block: top first, then its left side, then its front side, from outside.
// Stock edge = block side + lip width; the side contact is the tool's centre,
// so the tool radius comes in too.
void MachinePanel::bitZeroAfterV1(Phase stage)
{
    const double r = m_bz.toolDiameter / 2.0;
    switch (stage) {
    case Phase::BitZeroZ: {
        if (m_hits.size() < 2) {
            bitZeroFail(QStringLiteral("expected two Z contacts, got %1").arg(m_hits.size()));
            return;
        }
        const double top = m_hits.at(1).z;
        const double over = m_bz.thickness
                            + (m_bzMode == BitZeroPanel::ZOnly ? m_bz.lipDrop : 0.0);
        m_bzStockZ = top - over;
        m_bzOverZ = top + 5.0;   // where the plate lines leave the pin
        log(QStringLiteral("BitZero V1: block top at machine Z %1, %2 mm over the stock → stock top Z %3")
                .arg(top, 0, 'f', 3).arg(over, 0, 'f', 2).arg(m_bzStockZ, 0, 'f', 3));
        if (m_bzWantX)
            bitZeroStage(Phase::BitZeroX);
        else if (m_bzWantY)
            bitZeroStage(Phase::BitZeroY);
        else
            bitZeroApply();
        return;
    }
    case Phase::BitZeroX: {
        if (m_hits.size() < 2) {
            bitZeroFail(QStringLiteral("expected two X contacts, got %1").arg(m_hits.size()));
            return;
        }
        const double side = m_hits.at(1).x + r;
        m_bzCx = side + m_bz.lipWidth;
        log(QStringLiteral("BitZero V1: left side at machine X %1 → stock edge X %2")
                .arg(side, 0, 'f', 3).arg(m_bzCx, 0, 'f', 3));
        if (m_bzWantY)
            bitZeroStage(Phase::BitZeroY);
        else
            bitZeroApply();
        return;
    }
    case Phase::BitZeroY: {
        if (m_hits.size() < 2) {
            bitZeroFail(QStringLiteral("expected two Y contacts, got %1").arg(m_hits.size()));
            return;
        }
        const double side = m_hits.at(1).y + r;
        m_bzCy = side + m_bz.lipWidth;
        log(QStringLiteral("BitZero V1: front side at machine Y %1 → stock edge Y %2")
                .arg(side, 0, 'f', 3).arg(m_bzCy, 0, 'f', 3));
        bitZeroApply();
        return;
    }
    default:
        bitZeroApply();
        return;
    }
}

// All the way up in Z, before any sideways move after probing. Homed, that
// is machine Z just under the switch (the jog limit); not homed, machine
// coordinates mean nothing, so a fixed lift instead.
QStringList MachinePanel::riseToTop() const
{
    if (m_homed)
        return {QStringLiteral("G90"),
                QStringLiteral("G53 G0 Z%1").arg(-switchClearance(), 0, 'f', 3)};
    // Not homed: up until the Z end stop fires. That needs hard limits on,
    // or the move would run on into the stop; without them, nothing moves.
    if (m_grbl->setting(21, 0) >= 1)
        return {QStringLiteral("G91"), QStringLiteral("G0 Z%1").arg(m_grbl->setting(132, 155) + 10, 0, 'f', 1),
                QStringLiteral("G90")};
    return {};
}

// Resume a program parked at a tool change. Whoever pressed Continue may
// have left the new bit just above the stock (jogged down to re-zero Z, or a
// collet tightened low). The program lifts first after a change, but only to
// its own safe height and only if it came from our post, so go all the way
// up first. The rise is handed to the stream as program lines: ad-hoc
// commands here would be one more writer on a line the stream owns.
// Not homed, the only way up is into the Z switch, and with hard limits that
// is an alarm that ends the job; then the program's own lift has to do.
void MachinePanel::continueAfterToolChange()
{
    if (!m_homed)
        log(QStringLiteral("not homed: no rise to the top before continuing — the program "
                           "lifts to its safe height first"));
    m_manualZero = false;
    m_zeroedForTool = false;
    m_awaitToolZero = false;
    m_toolPromptAnswered = false;
    m_onIdle = nullptr;                  // a prompt still waiting for Idle is void now
    m_grbl->continueAfterToolChange(m_homed ? riseToTop() : QStringList{});
}

// What work Z the stock top at (wx, wy) reads, so Z0 stays the surface at
// X0 Y0 that the map's offsets are measured from (0 with the map off).
double MachinePanel::mapZeroAt(double wx, double wy) const
{
    return m_useMap->isChecked() && m_map.isValid() ? m_map.offsetAt(wx, wy) : 0.0;
}

void MachinePanel::noteZZero(double mx, double my, double sent, double wcoZ)
{
    m_zZeroKnown = true;
    m_zZeroWithMap = m_useMap->isChecked() && m_map.isValid();
    m_zZeroSent = sent;
    m_zZeroMx = mx;
    m_zZeroMy = my;
    m_zZeroWco = wcoZ;
    QSettings s;
    s.beginGroup(QStringLiteral("machine"));
    s.setValue(QStringLiteral("zzero/withMap"), m_zZeroWithMap);
    s.setValue(QStringLiteral("zzero/sent"), sent);
    s.setValue(QStringLiteral("zzero/mx"), mx);
    s.setValue(QStringLiteral("zzero/my"), my);
    s.setValue(QStringLiteral("zzero/wco"), wcoZ);
}

// Set the work offset from where the machine now stands and the corner it
// found: G10 L20 makes the current position read as the given work value.
void MachinePanel::bitZeroApply()
{
    whenIdle([this] {
        const MachineStatus st = m_grbl->status();
        QStringList cmds;
        QString g10 = QStringLiteral("G10 L20 P1");
        if (m_bzWantX)
            g10 += QStringLiteral(" X%1").arg(st.mx - m_bzCx, 0, 'f', 3);
        if (m_bzWantY)
            g10 += QStringLiteral(" Y%1").arg(st.my - m_bzCy, 0, 'f', 3);
        if (m_bzWantZ) {
            // A fresh Z zero belongs to the pin in the spindle: drop any old
            // length offset first, as the Zero Z button does.
            cmds << QStringLiteral("G49");
            m_grbl->applyToolLengthOffset(0);
            // The map's offset where it touched, as Zero Z does.
            const double wx = m_bzWantX ? st.mx - m_bzCx : st.wx;
            const double wy = m_bzWantY ? st.my - m_bzCy : st.wy;
            const double mz = mapZeroAt(wx, wy);
            g10 += QStringLiteral(" Z%1").arg(st.mz - m_bzStockZ + mz, 0, 'f', 3);
            noteZZero(st.mx, st.my, mz, m_bzStockZ - mz);
            if (mz != 0)
                log(QStringLiteral("surface map: Z0 set to the surface at X0 Y0, %1 mm from the plate"
                                   " (keep the map on for this zero)").arg(-mz, 0, 'f', 3));
        }
        cmds << g10;
        QString what;
        if (m_bzWantX) what += QStringLiteral("X0 ");
        if (m_bzWantY) what += QStringLiteral("Y0 ");
        if (m_bzWantZ) what += QStringLiteral("Z0 ");
        BitSetterConfig bs = bitSetter();
        if (m_homed)
            bs.safeZ = -switchClearance();   // risen to the top first, below
        if (m_bzWantZ && bs.enabled) {
            m_phase = Phase::MeasureForRef;
            m_grbl->startMacro(cmds + riseToTop()
                               + GrblStreamer::measureToolLines(bs, true, st.mx, st.my));
            log(QStringLiteral("BitZero: %1set — measuring the tool now in the spindle on the "
                               "BitSetter as the reference").arg(what));
            log(QStringLiteral("!! If that was the probing pin: the cutting bit must be measured "
                               "before it cuts. The job's first tool-change prompt does it — "
                               "answer OK there, never Ignore — or press Measure bit after "
                               "changing."));
        } else {
            // Then straight up, out of the way of the plate and the magnet.
            for (const QString &c : cmds + riseToTop())
                m_grbl->sendCommand(c);
            if (m_bzWantZ)
                forgetReference();   // see zero()
            if (m_bzWantZ && m_manualZero)
                m_awaitToolZero = true;      // m_zeroedForTool on the G10's ok
            log(QStringLiteral("BitZero: %1set. Remove the plate and the magnet before cutting")
                    .arg(what));
        }
        m_hits.clear();
        if (m_bzPanel)
            m_bzPanel->setBusy(false);
        // Done: back to the job card. A failed cycle stays on the probe
        // page so it can be run again.
        if (m_rightStack)
            m_rightStack->setCurrentWidget(m_jobPage);
    });
}

// ---- Surface map --------------------------------------------------------
//
// The plate (or the stock itself, when it conducts) touched at a grid of
// points; SurfaceMapProber runs the moves and touches, this keeps the result.

void MachinePanel::startSurfaceMap()
{
    if (m_grbl->status().valid && m_grbl->status().probePin) {
        log(QStringLiteral("!! the probe input is already closed — move the tool clear "
                           "of the plate and retry"));
        return;
    }
    const SurfaceMapPlan plan = m_bzPanel->mapPlan();
    // The map travels at work Z plate + travel: with Z zeroed high (tall
    // stock, a short bit) that is past the Z switch, a hard-limit alarm and
    // the position lost. Every other rise is kept under the top; this one
    // cannot be lowered without hitting the plate, so it is refused.
    if (m_grbl->status().valid) {
        const MachineStatus st = m_grbl->status();
        const double machineZ = plan.travelZ() + (st.mz - st.wz);
        if (machineZ > -switchClearance() + 1e-6) {
            log(QStringLiteral("!! surface map refused: its travel height (work Z%1) is machine Z%2, "
                               "above the top of Z (%3). Lower the plate's height or the travel "
                               "above it, or use a longer bit")
                    .arg(plan.travelZ(), 0, 'f', 1).arg(machineZ, 0, 'f', 1)
                    .arg(-switchClearance(), 0, 'f', 1));
            return;
        }
    }
    log(QStringLiteral("surface map: %1 × %2 points over X%3–%4 Y%5–%6, travelling at "
                       "work Z%7%8")
            .arg(plan.cols).arg(plan.rows)
            .arg(plan.x0, 0, 'f', 1).arg(plan.x1, 0, 'f', 1)
            .arg(plan.y0, 0, 'f', 1).arg(plan.y1, 0, 'f', 1)
            .arg(plan.travelZ(), 0, 'f', 1)
            .arg(plan.pauseEachPoint ? QStringLiteral(", stopping at each point for the plate")
                                     : QStringLiteral(", touching the stock directly")));
    m_bzPanel->setBusy(true);
    m_mapper->start(plan);
}

void MachinePanel::setSurfaceMap(const SurfaceMap &map)
{
    m_map = map;
    QSettings().setValue(QStringLiteral("machine/heightmap/map"),
                         map.isValid() ? map.toJson() : QString());
    refreshMapUi();
}

void MachinePanel::refreshMapUi()
{
    const bool have = m_map.isValid();
    if (!have)
        m_useMap->setChecked(false);
    m_useMap->setEnabled(have);
    const QString text = have ? m_map.summary()
                              : QStringLiteral("not measured: Probe zero (BitZero) → Surface map");
    m_mapLabel->setText(text);
    m_bzPanel->setMapSummary(have ? QStringLiteral("Map in use: %1").arg(text) : QString());
}

void MachinePanel::showPage(const QString &name)
{
    // "demo": wide sample readings, so --shot shows the page as tall and wide
    // as it gets on a connected machine. Leaves the card page as it is.
    if (name == QLatin1String("demo")) {
        MachineStatus st;
        st.state = QStringLiteral("Idle");
        st.mx = -1232.000; st.my = -1232.000; st.mz = -105.000;
        st.wx = -1107.100; st.wy = -1107.100; st.wz = 144.900;
        st.feed = 5000; st.spindle = 24000;
        st.valid = true;
        onStatus(st);
        return;
    }
    if (name == QLatin1String("bitzero"))
        openBitZero();
    else if (name == QLatin1String("bitsetter"))
        m_rightStack->setCurrentWidget(m_bsPage);
    else
        m_rightStack->setCurrentWidget(m_jobPage);
}

void MachinePanel::setDocument(Document *doc)
{
    m_doc = doc;
    refreshJobInfo();
}

// The job card names the design and counts its toolpaths; re-read whenever
// the page comes up, since both change behind it in Design/Toolpaths.
void MachinePanel::refreshJobInfo()
{
    if (!m_jobName)
        return;
    const QString path = m_doc ? m_doc->filePath() : QString();
    QString name = QFileInfo(path).fileName();
    if (name.startsWith(QLatin1String("untitled-")))
        name = QStringLiteral("Untitled design");
    m_jobName->setText(name.isEmpty() ? QStringLiteral("No design open") : name);
    const int n = m_doc ? m_doc->toolpaths().size() : 0;
    m_jobInfo->setText(n == 0 ? QStringLiteral("No toolpaths yet")
                              : QStringLiteral("%1 toolpath%2").arg(n).arg(n == 1 ? "" : "s"));
}

void MachinePanel::showEvent(QShowEvent *event)
{
    refreshJobInfo();
    QWidget::showEvent(event);
}

void MachinePanel::hideEvent(QHideEvent *event)
{
    stopHoldJog();
    QWidget::hideEvent(event);
}

// End the job under control: feed hold first, so the machine decelerates
// and stands still; ask; then a soft reset while standing (GRBL keeps the
// position, no ALARM:3), which also stops the spindle, and Z up to the top.
void MachinePanel::cancelJob()
{
    // Held or running with the stream done counts too: held in the last
    // seconds, the last lines are in the planner and the spindle is on.
    const bool active = m_grbl->isConnected()
        && (m_grbl->isStreaming() || m_grbl->isMacroRunning() || m_phase != Phase::Idle
            || m_held || m_machineState.startsWith(QLatin1String("Hold"))
            || m_machineState == QLatin1String("Run"));
    if (!active) {
        log(QStringLiteral("nothing to cancel"));
        return;
    }
    // Hold whenever a program or macro is running, not only when the last
    // report said Run or Jog: GRBL reports Idle during every G4 it executes
    // (the spin-up after each spindle start, drill dwells), and the program
    // carries on cutting when the dwell ends. `!` while Idle just parks GRBL
    // in Hold:0, which Keep running lifts again.
    const bool moving = m_machineState == QLatin1String("Run")
                        || m_machineState == QLatin1String("Jog")
                        || m_grbl->isStreaming() || m_grbl->isMacroRunning();
    const bool wasHeld = m_held;
    if (moving)
        m_grbl->pauseStream();
    setHeld(true);
    ShopAsk a;
    a.title = QStringLiteral("Cancel job");
    a.headline = QStringLiteral("Cancel the job?");
    a.body = QStringLiteral("%1 Cancelling stops the spindle and raises Z to "
                            "the top. The job cannot be resumed afterwards.")
                 .arg(moving || wasHeld ? QStringLiteral("The machine is held.")
                                        : QStringLiteral("The machine is stopped."));
    a.okText = QStringLiteral("Cancel job");
    a.okDanger = true;
    a.cancelText = QStringLiteral("Keep running");
    a.defaultOk = false;
    if (!shopAsk(this, a)) {
        // Back as it was: running again only if this held it (and the
        // resume went out); a hold from before stays, Resume still lifts it.
        if (moving && !wasHeld)
            setHeld(!m_grbl->resumeStream());
        else
            setHeld(wasHeld || m_machineState.startsWith(QLatin1String("Hold")));
        return;
    }
    m_endOfJobPending = false;   // cancelled: no end-of-job moves behind the hold
    holdResetThen([this] {
        log(QStringLiteral("job cancelled: spindle off, Z up to the top"));
        // Not after a Stop in the meantime: nothing moves after Stop.
        QTimer::singleShot(800, this, [this, seq = m_flowSeq] {
            if (seq != m_flowSeq)
                return;
            const QStringList up = riseToTop();
            if (!up.isEmpty() && m_grbl->canSendCommand())
                m_grbl->startMacro(up);
        });
    });
}

void MachinePanel::holdResetThen(std::function<void()> then, bool evenAfterStop)
{
    m_grbl->pauseStream();               // again: harmless when already held
    m_resetPending = true;
    // Wait for the hold to finish decelerating (Hold:0), at most 5 s. The
    // first 300 ms are not trusted: the last report may predate the hold.
    auto *t = new QTimer(this);
    t->setInterval(100);
    auto tries = std::make_shared<int>(0);
    // A Stop while waiting has already reset the controller: nothing more
    // goes out after it. (The job finishing meanwhile is no Stop: its last
    // lines went into the held planner, and the reset is still needed.)
    connect(t, &QTimer::timeout, this, [this, t, tries, then, evenAfterStop, seq = m_stopSeq] {
        if (seq != m_stopSeq) {
            m_resetPending = false;
            t->stop();
            t->deleteLater();
            if (evenAfterStop && then)
                then();   // no motion in it (a disconnect): still wanted
            return;
        }
        const bool still = m_machineState == QLatin1String("Hold:0")
                           || m_machineState == QLatin1String("Idle");
        ++*tries;
        if (m_grbl->isConnected() && (*tries < 3 || !still) && *tries < 50)
            return;
        t->stop();
        t->deleteLater();
        m_resetPending = false;
        resetFlow();
        m_grbl->stopStream();
        if (then)
            then();
    });
    t->start();
}

void MachinePanel::resetFlow()
{
    ++m_flowSeq;
    m_phase = Phase::Idle;
    m_hits.clear();
    if (m_bzPanel)
        m_bzPanel->setBusy(false);
    m_pendingTool = -1;
    m_pendingOrdinal = -1;
    m_toolMeasured = false;
    m_manualZero = false;
    m_zeroedForTool = false;
    m_awaitToolZero = false;
    m_toolPromptAnswered = false;
    m_rehearsal = false;
    m_onIdle = nullptr;
    // Taps held back for a fresh report would go out on the first Idle
    // report after the reset: a jog after Stop.
    m_deferredJogs.clear();
    stopHoldJog();
    m_parkAfterHome = false;             // ...and so would the park after a homing
    setHeld(false);
    m_runBtn->setText(QStringLiteral("▶ Run"));
}

void MachinePanel::setHeld(bool held)
{
    m_held = held;
    m_holdSeen = false;
    m_heldClock.restart();
    m_pauseBtn->setText(held ? QStringLiteral("⏵ Resume") : QStringLiteral("⏸ Hold"));
}

// ---- gamepad -----------------------------------------------------------------
//
// The mapping lives in gamepad.cpp (gamepadmap::defaults / load / save) and is
// edited from the "Map buttons…" dialog. Every press is still logged with its
// number, so a clone with an odd layout can be worked out without the dialog.
void MachinePanel::loadGamepadMapping()
{
    QSettings s;
    m_padMap = gamepadmap::load(s);
}

void MachinePanel::editGamepadMapping()
{
    // Pressing a button to find it in the table must not also jog, zero or
    // stop the machine, so the pad is ignored here while the dialog is open.
    stopHoldJog();
    m_padAxisDir[0] = m_padAxisDir[1] = 0;
    m_padMapping = true;
    GamepadMapDialog dlg(m_padMap, m_pad, this);
    const bool save = dlg.exec() == QDialog::Accepted;
    m_padMapping = false;
    if (!save)
        return;
    QSettings s;
    gamepadmap::save(s, dlg.mapping());
    s.setValue(QStringLiteral("ui/padStyle"), dlg.padStyle());
    if (m_padMapBtn)
        m_padMapBtn->setIcon(padIcon());
    loadGamepadMapping();
    log(QStringLiteral("gamepad: button mapping saved (%1 button(s) mapped)")
            .arg(m_padMap.size()));
}

void MachinePanel::gamepadAxis(int number, int value)
{
    if (m_padMapping)
        return;
    if (number > 1)
        return;
    // A D-pad reports its rest position as 0 and a direction as full scale;
    // an analogue stick needs the same treatment with a dead zone, so both are
    // reduced to -1, 0 or +1 here. A stick held about half way wobbles round
    // the threshold: it lets go only well below it, or each wobble was a tap
    // and a full jog step.
    const int was = m_padAxisDir[number];
    const int dir = value > 16000 ? 1 : (value < -16000 ? -1
                  : (was != 0 && value * was > 8000 ? was : 0));
    if (dir == was)
        return;
    // Kept while not connected too: a direction held across a dropped link
    // and let go there, then pressed again, was taken for no change.
    m_padAxisDir[number] = dir;
    if (!m_grbl->isConnected())
        return;
    if (dir == 0) {
        endHoldJog();
        return;
    }
    // Axis 1 is the pad's vertical: up reads negative, and up should be +Y.
    beginHoldJog(number == 0 ? 'X' : 'Y', number == 0 ? dir : -dir);
}

void MachinePanel::gamepadButton(int number, bool pressed)
{
    if (m_padMapping)
        return;
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

    if (action == QLatin1String("step+") || action == QLatin1String("step-")) {
        // Up and down stop at the ends instead of wrapping: one press too
        // many must not jump from 0.1 mm to 100 mm.
        const int n = m_step->count();
        const int i = m_step->currentIndex() + (action == QLatin1String("step+") ? 1 : -1);
        if (n > 0)
            m_step->setCurrentIndex(qBound(0, i, n - 1));
        log(QStringLiteral("gamepad: jog step %1 mm").arg(m_step->currentText()));
    } else if (action == QLatin1String("hold")) {
        m_pauseBtn->click();              // same path as the on-screen button
    } else if (action == QLatin1String("stop")) {
        m_stopBtn->click();
    } else if (action == QLatin1String("zeroxy")) {
        zero(QStringLiteral("XY"));
    } else if (action == QLatin1String("zeroz")) {
        zero(QStringLiteral("Z"));
    } else if (action == QLatin1String("home")) {
        initializeMachine();
    } else if (action == QLatin1String("unlock")) {
        unlock();
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
    // Show the start of a long port description, not its tail.
    if (m_ports->lineEdit())
        m_ports->lineEdit()->setCursorPosition(0);
}

void MachinePanel::toggleConnect()
{
    if (m_grbl->isConnected()) {
        // Mid-job, closing the port is not stopping the machine: GRBL runs on
        // through its buffer and planner, then stands with the spindle on,
        // while this side can no longer hold, stop or watch the end stops.
        // Ask; then hold, reset once it stands (spindle off, position kept)
        // and only then let go of the line.
        if (isBusy() || m_grbl->isParkedForTool()) {
            ShopAsk a;
            a.title = QStringLiteral("Disconnect");
            a.headline = QStringLiteral("Disconnect during the job?");
            a.body = QStringLiteral("The machine is working. Disconnecting holds it, resets the "
                                    "controller (spindle off, the rest of the job discarded) "
                                    "and then closes the connection.");
            a.okText = QStringLiteral("Stop and disconnect");
            a.okDanger = true;
            a.cancelText = QStringLiteral("Keep running");
            a.defaultOk = false;
            if (!shopAsk(this, a) || !m_grbl->isConnected())
                return;
            stopHoldJog();
            setHeld(true);
            log(QStringLiteral("disconnecting: holding, then resetting the controller"));
            m_endOfJobPending = false;
            holdResetThen([this] {
                // Let the reset go out before the port closes.
                QTimer::singleShot(300, this, [this] { m_grbl->disconnectPort(); });
            }, true);
            return;
        }
        m_grbl->disconnectPort();
        return;
    }
    const QString port = currentPortName();
    if (port.isEmpty()) {
        shopTell(this, QStringLiteral("Machine"), QStringLiteral("No serial port found."),
                 QStringLiteral("Plug in the controller and rescan, or type a device path."));
        return;
    }
    m_grbl->connectPort(port);
}

BitSetterConfig MachinePanel::bitSetter() const
{
    BitSetterConfig c;
    c.enabled = m_bsEnabled->isChecked();
    c.x = m_bsX->value();
    c.y = m_bsY->value();
    c.safeZ = riseZ();
    c.fastFeed = m_bsFast->value();
    c.slowFeed = m_bsSlow->value();
    return c;
}

void MachinePanel::loadSettings()
{
    m_loading = true;                     // widget setters must not save back
    QSettings s;
    s.beginGroup(QStringLiteral("machine"));
    m_bsEnabled->setChecked(s.value(QStringLiteral("bitsetter/enabled"), false).toBool());
    m_zZeroKnown = s.contains(QStringLiteral("zzero/wco"));
    m_zZeroWithMap = s.value(QStringLiteral("zzero/withMap")).toBool();
    m_zZeroSent = s.value(QStringLiteral("zzero/sent")).toDouble();
    m_zZeroMx = s.value(QStringLiteral("zzero/mx")).toDouble();
    m_zZeroMy = s.value(QStringLiteral("zzero/my")).toDouble();
    m_zZeroWco = s.value(QStringLiteral("zzero/wco")).toDouble();
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
    s.setValue(QStringLiteral("bitsetter/enabled"), m_bsEnabled->isChecked());
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

// Z was re-zeroed without measuring the tool: the stored reference (and its
// saved copy) no longer describes the tool the zero belongs to.
void MachinePanel::forgetReference()
{
    if (!m_haveRef)
        return;
    m_haveRef = false;
    saveSettings();
    updateOffsetLabels();
    log(QStringLiteral("BitSetter reference cleared: Z was zeroed without measuring the tool"));
}

void MachinePanel::updateOffsetLabels()
{
    m_refLabel->setText(m_haveRef ? QStringLiteral("reference: tool tripped at machine Z %1")
                                        .arg(m_refZ, 0, 'f', 3)
                                  : QStringLiteral("reference: none — zero Z or measure to set"));
    const double tlo = m_grbl->toolLengthOffset();
    m_tloLabel->setText(QStringLiteral("offset: G43.1 Z%1%2").arg(tlo, 0, 'f', 3)
                            .arg(tlo == 0.0 ? QStringLiteral("  (reference tool)") : QString()));
}

// ---- status ------------------------------------------------------------------

// The status pill says what the machine is doing in words, coloured by how
// much attention it wants: green idle, amber moving, red alarm.
void MachinePanel::setStatePill(const QString &text, const QString &border)
{
    m_state->setText(text);
    m_state->setStyleSheet(QStringLiteral("border-color: %1;").arg(border));
}

void MachinePanel::onStatus(const MachineStatus &st)
{
    m_machineState = st.state;
    ++m_statusSeq;
    // The pause button follows the machine: a hold stays until a report
    // shows it lifted (or it never took, e.g. `!` in Alarm).
    if (m_held) {
        if (st.state.startsWith(QLatin1String("Hold")) || st.state.startsWith(QLatin1String("Door")))
            m_holdSeen = true;
        else if (!st.state.isEmpty() && (m_holdSeen || m_heldClock.elapsed() > 2000))
            setHeld(false);
    }
    // Busy file: touched every 10 s while the machine works, so a
    // screensaver (or anything else) can stay out of the way.
    if (isBusy() && (!m_busyClock.isValid() || m_busyClock.elapsed() > 10000)) {
        m_busyClock.restart();
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        QDir().mkpath(dir);
        QFile f(dir + QStringLiteral("/busy"));
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(QDateTime::currentDateTime().toString(Qt::ISODate).toUtf8());
    }
    // Reconnecting (the app restarted, the cable was replugged): GRBL keeps
    // its position as long as it was not reset or power-cycled - and then it
    // boots in Alarm. Not in Alarm, and exactly where the last homed session
    // left it: still homed, no Initialize needed.
    // GRBL answers $$ with error:8 while it runs or holds (moves left from
    // before the link dropped): asked again once it is Idle, or the travel,
    // $21 and the $13 inch reports went unread for the whole session.
    if (st.valid && !m_grbl->haveSettings() && m_settingsAsked.isValid() && m_settingsAsked.elapsed() > 2000
        && (st.state == QLatin1String("Idle") || st.state.startsWith(QLatin1String("Alarm")))
        && !m_grbl->isStreaming()) {
        m_settingsAsked.start();
        m_grbl->sendCommand(QStringLiteral("$$"));
    }
    if (m_resumeCheck && st.valid && !st.state.isEmpty() && !m_holdChecked) {
        m_holdChecked = true;
        // Paused by a job this session never sent (the link dropped, the app
        // restarted, on a board that does not reset when the port opens):
        // its leftover moves are still in GRBL's buffer with the spindle on,
        // and a Resume would run them with nothing tracking them. Clear them
        // the way a Stop of a held job does; GRBL keeps its position.
        if ((st.state.startsWith(QLatin1String("Hold")) || st.state.startsWith(QLatin1String("Door")))
            && !m_grbl->isStreaming()) {
            log(QStringLiteral("!! the controller was paused in a job this session did not send — "
                               "clearing what is left of it (the spindle stops). Start the job again "
                               "from here if it is to be finished"));
            holdResetThen({});
        }
    }
    // The position only after the $$ listing: with $13=1 it reads in inches
    // until that is known, and never matches where the last session left it.
    // (In Hold GRBL answers $$ with error:8, so the clear above cannot wait.)
    if (m_resumeCheck && st.valid && !st.state.isEmpty() && m_grbl->haveSettings()) {
        m_resumeCheck = false;
        QSettings s;
        const QVariantList last = s.value(QStringLiteral("machine/homedAt")).toList();
        if (last.size() == 3 && !st.state.startsWith(QLatin1String("Alarm"))
            && std::abs(last[0].toDouble() - st.mx) < 0.005
            && std::abs(last[1].toDouble() - st.my) < 0.005
            && std::abs(last[2].toDouble() - st.mz) < 0.005) {
            m_homed = true;
            m_limitWarned = false;
            log(QStringLiteral("controller still where the last homed session left it "
                               "(X%1 Y%2 Z%3): homed, no Initialize needed")
                    .arg(st.mx, 0, 'f', 3).arg(st.my, 0, 'f', 3).arg(st.mz, 0, 'f', 3));
        }
    }
    // While homed, remember where the machine is (on every change), so the
    // next connection can tell it has not moved since.
    if (m_homed && st.valid) {
        const QVariantList now{st.mx, st.my, st.mz};
        if (now != m_savedHomedAt) {
            m_savedHomedAt = now;
            QSettings().setValue(QStringLiteral("machine/homedAt"), now);
        }
    } else if (!m_homed && !m_savedHomedAt.isEmpty() && !m_resumeCheck) {
        m_savedHomedAt.clear();
        QSettings().remove(QStringLiteral("machine/homedAt"));
    }
    const bool alarm = st.state.startsWith(QLatin1String("Alarm"));
    const bool homing = st.state.startsWith(QLatin1String("Home"));
    {
        QString words = st.state, color = QStringLiteral("#f2a536");
        if (st.state == QLatin1String("Idle")) {
            words = QStringLiteral("Idle — ready");
            color = QStringLiteral("#6fcf97");
        } else if (alarm) {
            words = QStringLiteral("Alarm — press Initialize");
            color = QStringLiteral("#e05a5a");
        } else if (homing) {
            words = QStringLiteral("Homing…");
        } else if (st.state == QLatin1String("Run")) {
            words = QStringLiteral("Running");
        } else if (st.state == QLatin1String("Jog")) {
            words = QStringLiteral("Jogging");
        } else if (st.state.startsWith(QLatin1String("Hold"))) {
            words = QStringLiteral("Feed hold");
        } else if (st.state.startsWith(QLatin1String("Door"))) {
            words = QStringLiteral("Door open");
        } else if (st.state == QLatin1String("Check")) {
            words = QStringLiteral("Check mode");
        }
        setStatePill(words, color);
        m_state->setToolTip(st.state);
    }
    if (m_bzPanel) {
        m_bzPanel->setContact(st.probePin, true);
        m_bzPanel->setConnected(true);
    }
    m_initBanner->setVisible(alarm || homing);
    m_initBtn->setEnabled(alarm);
    m_initBtn->setText(homing ? QStringLiteral("Homing…")
                              : QStringLiteral("Initialize machine (home)"));
    if (homing) {
        m_sawHoming = true;
        m_homingStarted = true;
    }
    if (m_homingStarted && st.state == QLatin1String("Idle")) {
        m_homingStarted = false;
        m_homed = true;                  // machine coordinates are real again
        m_limitWarned = false;
        forgetJogPlan();
        log(QStringLiteral("homed: machine position known, jog limits on"));
    }
    if (alarm)
        m_homingStarted = false;
    // End stop watchdog. Hard limits ($21=1) stop the controller by
    // themselves; this is the second layer: any limit input that comes on
    // while the machine moves - except during homing, which needs them -
    // resets the controller at once, whatever sequence is running.
    {
        const bool moving = st.state == QLatin1String("Run") || st.state == QLatin1String("Jog")
                            || st.state.startsWith(QLatin1String("Hold"));
        QString fresh;
        for (const QChar a : st.limitPins)
            if (!m_lastLimitPins.contains(a))
                fresh += a;
        if (!homing && moving && !fresh.isEmpty()) {
            ++m_stopSeq;
            m_onIdle = nullptr;
            m_grbl->stopStream();
            m_homed = false;
            m_unhomedWhy = QStringLiteral("an end stop fired during a move");
            forgetJogPlan();
            log(QStringLiteral("!! END STOP %1 — all motion stopped (controller reset). "
                               "Initialize (home) before moving again.").arg(fresh));
        }
        m_lastLimitPins = st.limitPins;
    }
    {
        const bool settled = st.state == QLatin1String("Idle") && m_grbl->pendingCommands() == 0;
        if (settled)
            m_jogPlanValid = false;          // the next jog starts from this report
        m_jogSeedOk = settled && m_statusSeq >= m_jogFreshAfter;
        if (!m_deferredJogs.isEmpty() && m_onIdle) {
            // A step waits for the machine to stand still (BitZero's zero,
            // the end-of-job rise): a jog sent now locks out its G-code
            // (error:9), or moves the spot it is about to zero.
            m_deferredJogs.clear();
            log(QStringLiteral("jog tap dropped: a step is waiting for the machine to stand still"));
        } else if (!m_deferredJogs.isEmpty() && m_jogSeedOk && !m_jogCancelTimer
            && m_grbl->canSendCommand()) {
            const QVector<PendingJog> taps = std::exchange(m_deferredJogs, {});
            for (const PendingJog &j : taps)
                jogBy(j.axis, j.delta, j.feed, false);
        } else if (!m_deferredJogs.isEmpty() && (st.state.startsWith(QLatin1String("Alarm"))
                                                 || !m_grbl->canSendCommand())) {
            m_deferredJogs.clear();          // nothing to jog into any more
        }
    }
    if (m_parkAfterHome && st.state == QLatin1String("Idle")
        && (m_sawHoming || m_lastState.startsWith(QLatin1String("Home")))) {
        m_parkAfterHome = false;
        park(true);
    }
    if (alarm)
        m_parkAfterHome = false;
    m_wAxis[0]->setText(QString::number(st.wx, 'f', 3));
    m_wAxis[1]->setText(QString::number(st.wy, 'f', 3));
    m_wAxis[2]->setText(QString::number(st.wz, 'f', 3));
    m_mpos->setText(QStringLiteral("machine  X %1  Y %2  Z %3")
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
    // Run from the event loop, not here inside the serial read: a step that
    // opens a prompt (the tool change) left the panel reading nothing from
    // the controller while it was open - no status, no alarms, pad jogs
    // refused. The step stays pending (and can still be voided) until then.
    if (m_onIdle && !m_idlePosted) {
        if (st.state == QLatin1String("Idle") && m_lastState == QLatin1String("Idle")) {
            m_idlePosted = true;
            const quint64 seq = m_idleSeq;
            QTimer::singleShot(0, this, [this, seq] {
                m_idlePosted = false;
                if (!m_onIdle || seq != m_idleSeq)
                    return;
                // Something moved it since the report (a jog): wait again.
                if (!standingStill()) {
                    m_lastState.clear();
                    return;
                }
                auto fn = std::move(m_onIdle);
                m_onIdle = nullptr;
                fn();
            });
        }
    }
    m_lastState = st.state;
}

bool MachinePanel::needsInit()
{
    if (!m_machineState.startsWith(QLatin1String("Alarm")))
        return false;
    log(QStringLiteral("!! machine is in Alarm — press \"Initialize machine\" (home) first"));
    return true;
}

bool MachinePanel::isBusy() const
{
    if (!m_grbl->isConnected())
        return false;
    const QString &st = m_machineState;
    // A surface map waiting for the plate counts: the operator's hand and
    // the plate are under the tool.
    // A step waiting for Idle (the end-of-job rise, BitZero's zero) counts:
    // a job started under it replaced the step, and the zero was never set.
    return m_grbl->isStreaming() || m_grbl->isMacroRunning() || m_phase != Phase::Idle
        || m_onIdle
        || (m_mapper && m_mapper->isRunning())
        || st == QLatin1String("Run") || st == QLatin1String("Jog")
        || st.startsWith(QLatin1String("Home")) || st.startsWith(QLatin1String("Hold"));
}

// Refuse a machine-coordinate move until the machine is homed on this
// connection: before that, G53 positions (the top of Z, the BitSetter, the
// tool-change spot) can be anywhere.
bool MachinePanel::requireHomed(const QString &what)
{
    if (m_homed)
        return true;
    log(QStringLiteral("!! %1 needs a homed machine (%2) — press Initialize first")
            .arg(what, m_unhomedWhy));
    shopTell(this, QStringLiteral("Machine"), QStringLiteral("Initialize first"),
             QStringLiteral("The machine position is not known: %1.\n\n%2 uses machine "
                            "positions, so press Initialize to home, then try again.")
                 .arg(m_unhomedWhy, what));
    return false;
}

// How far to stay from a home switch: its pull-off ($27, the distance the
// switch needs to let go) plus a millimetre.
double MachinePanel::switchClearance() const
{
    return m_grbl->setting(27, 1.5) + 1.0;
}

bool MachinePanel::insideTravel(double x, double y) const
{
    QSettings sset;
    const auto machine = appsettings::machine(sset);
    const double tx = m_grbl->setting(130, machine.width), ty = m_grbl->setting(131, machine.height);
    const double c = switchClearance();
    return x <= -c && x >= -tx && y <= -c && y >= -ty;
}

// $X out of an Alarm skips homing, so machine coordinates no longer say
// where the ends of travel are: jogs stop being limited until the next home.
void MachinePanel::unlock()
{
    if (m_machineState.startsWith(QLatin1String("Alarm"))) {
        m_homed = false;
        m_unhomedWhy = QStringLiteral("unlocked out of an alarm without homing");
        m_limitWarned = false;
        forgetJogPlan();
    }
    // After an end stop or a soft limit GRBL listens to nothing until it is
    // reset: $X now would only wait for ever for its answer. So does one that
    // was locked before this connection opened.
    if (m_grbl->maybeLocked()) {
        log(QStringLiteral("resetting the controller (it stopped on a limit), then unlocking ($X)"));
        m_grbl->resetThen(QStringLiteral("$X"));
        return;
    }
    m_grbl->sendCommand(QStringLiteral("$X"));
}

void MachinePanel::initializeMachine()
{
    if (!m_grbl->isConnected()) {
        log(QStringLiteral("!! not connected"));
        return;
    }
    // A homing cycle and the rapid to the park corner, with nobody asking:
    // only out of an Alarm (what the on-screen banner is for) or from a
    // machine standing idle. Never at a tool change, where the operator's
    // hands are at the collet and a pad button is easily bumped.
    const bool alarm = m_machineState.startsWith(QLatin1String("Alarm"));
    if (!alarm && (isBusy() || m_grbl->isParkedForTool()
                   || m_machineState != QLatin1String("Idle"))) {
        log(m_grbl->isParkedForTool()
                ? QStringLiteral("!! Initialize refused: a job is parked at a tool change. "
                                 "Stop the job first")
                : QStringLiteral("!! Initialize refused: the machine is busy (%1). Wait until it "
                                 "stands idle, or stop it first")
                      .arg(m_machineState.isEmpty() ? QStringLiteral("no status yet")
                                                    : m_machineState));
        return;
    }
    stopHoldJog();
    if (m_grbl->maybeLocked()) {
        // An end stop or soft limit: GRBL ignores $H until a soft reset.
        log(QStringLiteral("initializing: resetting the controller (it stopped on a limit), "
                           "then homing cycle ($H), then park"));
        m_grbl->resetThen(QStringLiteral("$H"));
    } else {
        log(QStringLiteral("initializing: homing cycle ($H), then park"));
        if (!m_grbl->sendCommand(QStringLiteral("$H"))) {
            log(QStringLiteral("!! busy — cannot home now"));
            return;
        }
    }
    m_parkAfterHome = true;
    m_sawHoming = false;
}

// Up to safe Z, then at rapid speed to the front-right corner, where the
// collet is in reach for changing bits. Machine X0 is the RIGHT end and Y0 the
// BACK on this Shapeoko, so front right is X just off its switch and Y the full
// cutting height of the machine picked in Settings forward, less a margin so
// the front end stop is never touched.
void MachinePanel::park(bool afterHoming)
{
    if (jobWindingDown(QStringLiteral("Park")))
        return;
    if (!m_grbl->canSendCommand()) {
        log(QStringLiteral("!! busy — cannot park now"));
        return;
    }
    if (needsInit())
        return;
    if (!requireHomed(QStringLiteral("Park")))
        return;
    // After homing nothing jogged: the tool offset re-sent on the $H answer
    // may still be pending, and the park simply queues behind it.
    if (!afterHoming && movingStill(QStringLiteral("Park")))
        return;
    QSettings st;
    const auto machine = appsettings::machine(st);
    constexpr double kMargin = 5.0;
    // Inside the travel the controller reports ($130-$132) when it is
    // smaller than the Settings size (which defaults to a 4x4): soft limits
    // are off, so a target past the end is a rapid into the hard stop.
    const double tx = qMin(machine.width, m_grbl->setting(130, machine.width));
    const double ty = qMin(machine.height, m_grbl->setting(131, machine.height));
    const double tz = m_grbl->setting(132, 75.0);
    const double clear = switchClearance();
    const double x = -qMin(qMax(kMargin, clear), tx / 2), y = -qMax(clear, ty - kMargin);
    const double z = riseZ();
    log(QStringLiteral("parking front right (X%1 Y%2, machine)").arg(x, 0, 'f', 1).arg(y, 0, 'f', 1));
    m_grbl->startMacro({QStringLiteral("G90"),
                        QStringLiteral("G53 G0 Z%1").arg(z, 0, 'f', 3),
                        QStringLiteral("G53 G0 X%1 Y%2").arg(x, 0, 'f', 3).arg(y, 0, 'f', 3)});
}

double MachinePanel::safeZ() const
{
    // The box allows 0 down to -1000: above the switch clearance a move trips
    // the Z limit switch (hard limits are on), below the travel it drives into
    // the stock or the bottom stop. Every safe-Z move uses this bound.
    constexpr double kMargin = 5.0;
    const double tz = m_grbl->setting(132, 75.0);
    return qBound(-(tz - kMargin), m_bsSafeZ->value(), -switchClearance());
}

double MachinePanel::riseZ() const
{
    // A rise to safe Z never goes down: with safe Z set below where the
    // spindle already is (a high job retract over tall stock) the move would
    // plunge at the current X Y. Up to the top at most.
    const double z = safeZ();
    if (!m_homed || !m_grbl->isConnected())
        return z;
    return qMax(z, qMin(m_grbl->status().mz, -switchClearance()));
}

void MachinePanel::whenIdle(std::function<void()> fn)
{
    m_lastState.clear();
    ++m_idleSeq;
    m_onIdle = std::move(fn);
}

// ---- zero / go ---------------------------------------------------------------

// A job's last lines are still running, or its end-of-job rise has not been
// sent yet: a move now would start from where the program has not left the
// tool, with the spindle on.
bool MachinePanel::endRiseWaiting() const
{
    return m_endRisePending && m_onIdle;
}

bool MachinePanel::jobWindingDown(const QString &what)
{
    const QString &st = m_machineState;
    if (!endRiseWaiting() && st != QLatin1String("Run") && !st.startsWith(QLatin1String("Hold")))
        return false;
    log(QStringLiteral("!! %1: the job is still finishing — wait until the machine stands still").arg(what));
    return true;
}

bool MachinePanel::standingStill() const
{
    return m_machineState == QLatin1String("Idle") && m_grbl->pendingCommands() == 0
           && m_deferredJogs.isEmpty() && !m_jogCancelTimer && m_statusSeq >= m_moveFreshAfter;
}

// GRBL refuses G-code while a jog runs (error:9), and a tap just sent still
// reads Idle until the next report: say so instead of a bare error, or a
// "no contact" from a probe that never ran.
bool MachinePanel::movingStill(const QString &what)
{
    if (standingStill())
        return false;
    log(QStringLiteral("!! %1 refused: %2 — wait until it stands still").arg(what, stillMovingWhy()));
    return true;
}

QString MachinePanel::stillMovingWhy() const
{
    if (m_machineState.isEmpty())
        return QStringLiteral("no status from the controller yet");
    if (m_machineState != QLatin1String("Idle"))
        return QStringLiteral("the machine is not idle (%1)").arg(m_machineState);
    if (!m_deferredJogs.isEmpty() || m_jogCancelTimer)
        return QStringLiteral("a jog is still to go out");
    return QStringLiteral("the last jog may still be moving");
}

bool MachinePanel::zeroAllowed(bool xy)
{
    // The surface map files each height under a grid point in work
    // coordinates: a new zero mid-map puts the rest of the points somewhere
    // else, and the finished map corrects the job at the wrong places.
    if (m_mapper && m_mapper->isRunning()) {
        log(QStringLiteral("!! zeroing refused while the surface map runs — stop the map first"));
        return false;
    }
    if (!m_grbl->isStreaming())
        return true;
    // A job is running or parked at a tool change. Zeroing XY there moves
    // the rest of the job to wherever the machine stands (the tool-change
    // corner); zeroing Z at park height sends the next cut into the air or
    // the table. Pad buttons sit under the operator's hands at a tool change,
    // so a bump must not do either. Z alone is fine while the operator has
    // the prompt closed to zero the new bit by hand (BitSetter off).
    if (!xy && m_manualZero && m_grbl->isParkedForTool() && !m_grbl->isMacroRunning())
        return true;
    log(m_grbl->isParkedForTool() && !xy && !m_bsEnabled->isChecked()
            ? QStringLiteral("!! zeroing refused: a job is parked at a tool change. Answer the "
                             "prompt with \"Close: I'll zero Z\" to zero the new bit by hand")
            : QStringLiteral("!! zeroing refused while a job is running — it would move the "
                             "rest of the job. Stop the job first"));
    return false;
}

void MachinePanel::zero(const QString &axes)
{
    if (needsInit())
        return;
    if (!zeroAllowed(axes.contains(QChar('X')) || axes.contains(QChar('Y'))))
        return;
    // The end-of-job rise would land in the middle of a BitSetter reference
    // measure and fail it.
    if (jobWindingDown(QStringLiteral("Zero")))
        return;
    if (!m_grbl->canSendCommand()) {
        log(QStringLiteral("!! busy — cannot zero now"));
        return;
    }
    // GRBL refuses G-code while jogging (error:9), and a tap still waiting
    // for a fresh report would run after the zero: Z0 a step off the bit.
    if (!standingStill()) {
        log(QStringLiteral("!! zeroing refused: %1 — wait until it stands still, then zero")
                .arg(stillMovingWhy()));
        return;
    }
    QStringList cmds;
    const bool zeroZ = axes.contains(QChar('Z'));
    // With the BitSetter on, a Z zero measures the reference there: checked
    // before any zero goes out, as Measure bit checks it.
    if (zeroZ && bitSetter().enabled
        && !bitSetterReachable(bitSetter(), QStringLiteral("Zeroing Z (it measures on the BitSetter)")))
        return;
    if (zeroZ) {
        // A fresh Z zero belongs to the tool in the spindle, so any old length
        // offset must go first; the measurement below re-establishes it as 0.
        cmds << QStringLiteral("G49");
        m_grbl->applyToolLengthOffset(0);
    }
    QString g10 = QStringLiteral("G10 L20 P1");
    for (const QChar ax : axes) {
        if (ax != QChar('Z')) {
            g10 += QStringLiteral(" %1").arg(ax) + QStringLiteral("0");   // " X0"
            continue;
        }
        // With the surface map on, Z0 stays the surface at X0 Y0: zeroed
        // here, the job followed the map from the wrong height (a 2 mm warp
        // left a 1 mm cut in the air).
        const MachineStatus st = m_grbl->status();
        const double z = mapZeroAt(axes.contains(QChar('X')) ? 0.0 : st.wx,
                                   axes.contains(QChar('Y')) ? 0.0 : st.wy);
        g10 += z == 0 ? QStringLiteral(" Z0") : QStringLiteral(" Z%1").arg(z, 0, 'f', 3);
        noteZZero(st.mx, st.my, z, st.mz - z);
        // The readout shows that height here, not 0: say why (and that it
        // holds only with the map on).
        if (z != 0)
            log(QStringLiteral("surface map: Z0 set to the surface at X0 Y0, %1 mm from here"
                               " (keep the map on for this zero)").arg(-z, 0, 'f', 3));
    }
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
        // A Z zero with no measurement: the reference belonged to whatever
        // tool was zeroed before, and offsets from it would be wrong.
        if (zeroZ)
            forgetReference();
        if (zeroZ && m_manualZero)
            m_awaitToolZero = true;          // m_zeroedForTool on the G10's ok
    }
    updateOffsetLabels();
}

// The BitSetter measure rapids to the button in machine coordinates: the
// machine must be homed and the button inside the travel.
bool MachinePanel::bitSetterReachable(const BitSetterConfig &cfg, const QString &what)
{
    if (!requireHomed(what))
        return false;
    if (!insideTravel(cfg.x, cfg.y)) {
        shopTell(this, QStringLiteral("BitSetter"), QStringLiteral("BitSetter position is off the machine"),
                 QStringLiteral("X %1  Y %2 is outside the machine's travel. It was probably "
                                "set while the machine was not homed. Jog over the button and "
                                "press Use here again.").arg(cfg.x, 0, 'f', 1).arg(cfg.y, 0, 'f', 1));
        return false;
    }
    return true;
}

void MachinePanel::goToXY0()
{
    if (jobWindingDown(QStringLiteral("Go to X0 Y0")))
        return;
    if (!m_grbl->canSendCommand())
        return;
    if (!requireHomed(QStringLiteral("Go to X0 Y0")))
        return;
    if (movingStill(QStringLiteral("Go to X0 Y0")))
        return;
    m_phase = Phase::GoToZero;
    m_grbl->startMacro({QStringLiteral("G90"),
                        QStringLiteral("G53 G0 Z%1").arg(riseZ(), 0, 'f', 3),
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
    if (!bitSetterReachable(cfg, QStringLiteral("Measuring on the BitSetter")))
        return;
    if (!cfg.enabled) {
        log(QStringLiteral("!! BitSetter is disabled — enable it and set the button position"));
        return;
    }
    if (why == Phase::MeasureManual && jobWindingDown(QStringLiteral("Measure")))
        return;
    if (!m_grbl->canSendCommand()) {
        log(QStringLiteral("!! busy — cannot probe now"));
        return;
    }
    if (why == Phase::MeasureManual && movingStill(QStringLiteral("Measure")))
        return;
    m_phase = why;
    const MachineStatus st = m_grbl->status();
    // Manual measurements return to where the tool was; after a tool change
    // the program's first rapid takes over from safe Z anyway.
    m_grbl->measureTool(cfg, why == Phase::MeasureManual, st.mx, st.my);
}

void MachinePanel::onToolChange(int tool, int ordinal)
{
    m_pendingTool = tool;
    m_pendingOrdinal = ordinal;
    m_toolMeasured = false;
    m_manualZero = false;
    m_zeroedForTool = false;
    m_awaitToolZero = false;
    m_toolPromptAnswered = false;
    m_runBtn->setText(QStringLiteral("▶ Continue"));
    parkForTool();
}

void MachinePanel::parkForTool()
{
    if (needsInit() || !requireHomed(QStringLiteral("Parking for the tool change")))
        return;
    m_phase = Phase::ParkForTool;
    // Spindle off, up to the top, and over to where the operator can reach
    // the collet. The top, not safe Z: the position reported at the pause
    // can be from before the program's last retract, and room to swap the
    // bit is what this park is for.
    m_grbl->startMacro({QStringLiteral("M5"), QStringLiteral("G90"),
                        QStringLiteral("G53 G0 Z%1").arg(-switchClearance(), 0, 'f', 3),
                        QStringLiteral("G53 G0 X%1 Y%2")
                            .arg(m_tcX->value(), 0, 'f', 3).arg(m_tcY->value(), 0, 'f', 3)});
}

void MachinePanel::onMacroFinished(bool ok)
{
    // The macro moved the machine: the next absolute jog starts from a
    // report taken after it, never from a plan made before it.
    forgetJogPlan();
    const Phase phase = m_phase;
    m_phase = Phase::Idle;
    switch (phase) {
    case Phase::Idle:
    case Phase::GoToZero:
        return;

    case Phase::BitZeroX:
    case Phase::BitZeroY:
    case Phase::BitZeroZ:
    case Phase::BitZeroHome:
        bitZeroAfter(phase, ok);
        return;

    case Phase::ParkForTool:
        if (!ok) {
            log(QStringLiteral("!! could not park for the tool change — fix the alarm, "
                               "then press Continue to park again"));
            return;
        }
        whenIdle([this] { askForTool(); });
        return;

    case Phase::MeasureForRef:
        // Zeroed by hand at a tool change with the BitSetter on: the macro
        // starts with G49 and the G10, so Z belongs to the new bit once those
        // two were answered, measured or not; Continue need not warn.
        if (m_manualZero && m_grbl->isParkedForTool() && (ok || m_grbl->macroLinesDone() >= 2))
            m_zeroedForTool = true;
        if (ok && m_grbl->lastProbeOk()) {
            m_haveRef = true;
            m_refZ = m_grbl->lastProbeZ();
            m_grbl->applyToolLengthOffset(0);
            log(QStringLiteral("reference tool: trips at machine Z %1").arg(m_refZ, 0, 'f', 3));
        } else {
            m_haveRef = false;   // the old one is for the tool zeroed before
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
            // Run refuses a job without a reference, so this is a reference
            // lost on the way. Its Z zero may belong to another bit (the
            // BitZero pin, the last tool): do not carry on by ourselves.
            if (phase == Phase::MeasureAfterChange) {
                saveSettings();
                updateOffsetLabels();
                log(QStringLiteral("!! program stays parked: make sure Z was zeroed with this "
                                   "bit (zero it now if not), then press Continue"));
                return;
            }
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
            whenIdle([this] { continueAfterToolChange(); });
        return;
    }
}

// The tool-change prompt, once the machine stands at the tool-change spot.
void MachinePanel::askForTool()
{
    if (!m_grbl->isParkedForTool())
        return;
    const bool bs = m_bsEnabled->isChecked();
    ShopAsk a;
    a.title = QStringLiteral("Tool change");
    a.headline = m_rehearsal
        ? QStringLiteral("Rehearsal: change the tool now")
        : QStringLiteral("Insert tool T%1").arg(m_pendingTool);
    a.body = m_rehearsal
        ? QStringLiteral("Or leave the same one in to check the measurement repeats.")
        : QStringLiteral("Tighten the collet and set the dust boot height before you continue.");
    if (!m_rehearsal && m_doc) {
        // Which bit: its diagram with the sizes on it, and the sizes in words.
        // Two bits can share a number, so the bit is the one of this very
        // tool change of the program being streamed, not the first toolpath
        // that happens to use the number.
        const bool known = m_pendingOrdinal >= 0 && m_pendingOrdinal < m_jobTools.size()
                           && m_jobTools.at(m_pendingOrdinal).ival == m_pendingTool;
        const LibraryTool lt = known ? toolForChange(*m_doc, m_jobTools.at(m_pendingOrdinal))
                                     : toolFromToolpaths(*m_doc, m_pendingTool);
        if (lt.diameter > 0) {
            QString what = lt.name.isEmpty() ? QStringLiteral("T%1").arg(m_pendingTool) : lt.name;
            what += QStringLiteral("\n\u00d8 %1 mm").arg(lt.diameter, 0, 'g', 4);
            if (lt.length > 0)
                what += QStringLiteral(" \u00b7 cut length %1 mm").arg(lt.length, 0, 'g', 4);
            if (lt.angle > 0)
                what += QStringLiteral(" \u00b7 %1\u00b0").arg(lt.angle);
            if (lt.flutes > 0)
                what += QStringLiteral(" \u00b7 %1 flute%2").arg(lt.flutes).arg(lt.flutes == 1 ? "" : "s");
            a.body = what + QStringLiteral("\n\n") + a.body;
            a.picture = toolPicture(lt, 150, 225, devicePixelRatioF());
        }
    }
    a.note = bs ? QStringLiteral("The BitSetter measures it, then the program "
                                 "carries on.")
                : QStringLiteral("BitSetter is off: if the tool length changed, close this, "
                                 "jog down and zero Z, then press Continue.");
    a.okText = bs ? QStringLiteral("Measure and continue")
                  : QStringLiteral("Same length: continue");
    // BitSetter off, the new bit has to be zeroed by hand, and that needs the
    // panel (jog, Zero Z, BitZero) which this modal prompt blocks. Close it
    // with the program left parked; Continue resumes once Z is zeroed.
    if (!bs)
        a.altText = QStringLiteral("Close: I'll zero Z");
    a.cancelText = QStringLiteral("Stop the job");
    const ShopAnswer ans = shopChoose(this, a);
    // "Stop the job" stops whatever is still streaming, parked or not.
    if (ans == ShopAnswer::Cancel && m_grbl->isStreaming()) {
        ++m_stopSeq;
        resetFlow();
        m_grbl->stopStream();
        return;
    }
    if (!m_grbl->isParkedForTool())
        return;                          // stopped meanwhile (Stop, a pad, the cable)
    m_toolPromptAnswered = true;
    if (ans == ShopAnswer::Alt) {
        m_manualZero = true;
        m_zeroedForTool = false;
        log(QStringLiteral("program parked at the tool change: jog down, zero Z for the new "
                           "tool, then press Continue"));
        return;
    }
    // A gamepad jogs with the prompt open: G-code would be refused while it
    // moves (error:9), so go on once it stands still.
    auto proceed = [this, bs] {
        if (!m_grbl->isParkedForTool())
            return;
        if (bs)
            measureTool(Phase::MeasureAfterChange);
        else
            continueAfterToolChange();
    };
    if (standingStill()) {
        proceed();
    } else {
        stopHoldJog();
        m_deferredJogs.clear();
        log(QStringLiteral("waiting for the machine to stand still before carrying on"));
        whenIdle(proceed);
    }
}

// A real tool change with nothing around it: the rehearsal program goes through
// startStream() like a job, so onToolChange(), the prompt, the BitSetter
// measurement, the G43.1 and the continue are the code a job runs, not a copy.
// Only the prompt wording and the summary at the end know it is a rehearsal.
void MachinePanel::rehearseToolChange()
{
    if (!requireHomed(QStringLiteral("The tool-change rehearsal")))
        return;
    // The park rapids there in machine coordinates, as a job's tool change
    // does: off the travel trips a limit switch (Run refuses it the same way).
    if (!insideTravel(m_tcX->value(), m_tcY->value())) {
        shopTell(this, QStringLiteral("Rehearse tool change"),
                 QStringLiteral("The tool-change spot (X %1  Y %2) is outside the machine's travel.")
                     .arg(m_tcX->value(), 0, 'f', 1).arg(m_tcY->value(), 0, 'f', 1),
                 QStringLiteral("Set it again on the BitSetter page (Use here) with the machine homed."));
        return;
    }
    if (!m_grbl->isConnected()) {
        shopTell(this, QStringLiteral("Rehearse tool change"),
                 QStringLiteral("Connect to the controller first."));
        return;
    }
    if (!m_bsEnabled->isChecked()) {
        shopTell(this, QStringLiteral("Rehearse tool change"),
                 QStringLiteral("Enable the BitSetter first."),
                 QStringLiteral("Turn BitSetter on and set its button position."));
        return;
    }
    // Without a reference, the measurement would quietly become the reference
    // and the rehearsal would prove nothing about the offset.
    if (!m_haveRef) {
        shopTell(this, QStringLiteral("Rehearse tool change"),
                 QStringLiteral("There is no reference tool yet."),
                 QStringLiteral("Zero Z with the BitSetter enabled (or press Measure tool) "
                                "first, then rehearse."));
        return;
    }
    // Held (Hold:0 with nothing moving) counts as busy: the rehearsal would
    // park behind the hold and never get to the prompt.
    // A jog tap just sent still reads Idle: the first line would be refused
    // (error:9) and the rehearsal stopped with a reset.
    auto busy = [this] {
        if (isBusy() || m_held || m_grbl->isStreaming() || !m_grbl->canSendCommand()
            || m_machineState != QLatin1String("Idle") || !m_homed)
            return true;
        if (!standingStill()) {
            log(QStringLiteral("!! %1 — wait until it stands still").arg(stillMovingWhy()));
            return true;
        }
        return false;
    };
    if (busy()) {
        log(QStringLiteral("!! busy — cannot rehearse a tool change now"));
        return;
    }
    ShopAsk ask;
    ask.title = QStringLiteral("Rehearse tool change");
    ask.headline = QStringLiteral("Start the rehearsal?");
    ask.body = QStringLiteral(
        "The machine will:\n"
        "  1. turn the spindle off and rise to safe Z (machine %1)\n"
        "  2. move to the tool-change spot (machine X%2 Y%3) and ask for the tool\n"
        "  3. measure it on the BitSetter (machine X%4 Y%5) and apply the offset\n"
        "  4. return to safe Z\n\n"
        "The spindle never starts and nothing is cut.")
        .arg(safeZ(), 0, 'f', 1)
        .arg(m_tcX->value(), 0, 'f', 1).arg(m_tcY->value(), 0, 'f', 1)
        .arg(m_bsX->value(), 0, 'f', 1).arg(m_bsY->value(), 0, 'f', 1);
    ask.note = QStringLiteral("Keep a hand near the e-stop the first time.");
    ask.okText = QStringLiteral("Start rehearsal");
    ask.defaultOk = false;
    if (!shopAsk(this, ask))
        return;
    // The dialog ran the event loop: a hold, an alarm or a pad may have come.
    if (busy()) {
        log(QStringLiteral("!! busy — cannot rehearse a tool change now"));
        return;
    }
    log(QStringLiteral("== tool-change rehearsal (spindle off, no cut) =="));
    setHeld(false);
    m_rehearsal = true;
    m_jobTools.clear();
    m_grbl->startStream(GrblStreamer::toolChangeRehearsalLines(bitSetter(), 1));
}

void MachinePanel::finishRehearsal(bool ok, bool measured)
{
    const double tlo = m_grbl->toolLengthOffset();
    QString head, msg;
    if (ok && measured) {
        head = QStringLiteral("Rehearsal complete");
        msg = QStringLiteral("The tool change ran end to end: parked, measured, offset "
                             "applied, continued.\n\nLength offset now G43.1 Z%1 against "
                             "the reference tool. With the same tool still in, this should "
                             "be within a few hundredths of a millimetre of zero.")
                  .arg(tlo, 0, 'f', 3);
        log(QStringLiteral("rehearsal OK: G43.1 Z%1").arg(tlo, 0, 'f', 3));
    } else {
        head = QStringLiteral("Rehearsal did not complete");
        msg = QStringLiteral("The rehearsal did not complete%1. Check the console, fix "
                             "the cause and rehearse again before running a job with "
                             "tool changes.")
                  .arg(ok ? QStringLiteral(" (the tool was not measured)") : QString());
        log(QStringLiteral("!! rehearsal did not complete"));
    }
    shopTell(this, QStringLiteral("Rehearse tool change"), head, msg);
}

// ---- program -------------------------------------------------------------------

bool MachinePanel::runProgram(QString *remoteWhy)
{
    const bool remote = remoteWhy != nullptr;
    // Here a refusal is a message on screen; from a pad it goes back there.
    auto refuse = [&](const QString &headline, const QString &body = QString()) {
        if (remote) {
            *remoteWhy = body.isEmpty() ? headline : headline + QLatin1Char(' ') + body;
            log(QStringLiteral("!! start from pad refused: %1").arg(*remoteWhy));
        } else {
            shopTell(this, QStringLiteral("Machine"), headline, body);
        }
        return false;
    };
    if (remote) {
        if (!m_grbl->isConnected())
            return refuse(QStringLiteral("The machine is not connected."));
        if (!m_homed)
            return refuse(QStringLiteral("The machine needs Initialize (homing) first: %1.").arg(m_unhomedWhy));
        if (isBusy() || m_grbl->isParkedForTool())
            return refuse(QStringLiteral("The machine is busy."));
        if (m_machineState != QLatin1String("Idle"))
            return refuse(QStringLiteral("The machine is not idle (%1).")
                              .arg(m_machineState.isEmpty() ? QStringLiteral("no status yet") : m_machineState));
    } else if (!requireHomed(QStringLiteral("Running a program"))) {
        return false;
    }
    if (m_rightStack)
        m_rightStack->setCurrentWidget(m_jobPage);
    if (!m_grbl->isConnected())
        return refuse(QStringLiteral("Connect to the controller first."));
    if (!m_doc)
        return refuse(QStringLiteral("Open a .c2d file first."));
    const QString docAtStart = m_doc->filePath();
    const quint64 loadAtStart = m_doc->loadCount();
    // Re-exported at the moment Run is pressed, so it is the current document
    // that gets streamed - and a 3D finish is minutes of work, which used to
    // freeze the window with the machine sitting idle.
    const GcodeResult r = exportWithProgress(this, *m_doc, QStringLiteral("Preparing program"));
    if (r.cancelled) {
        log(QStringLiteral("program preparation cancelled"));
        if (remote)
            *remoteWhy = QStringLiteral("Preparing the program was cancelled.");
        return false;
    }
    if (r.done.isEmpty())
        return refuse(QStringLiteral("No exportable toolpaths."));
    // A toolpath left out (a V-bit in a pocket, a depth it cannot reach)
    // and the rest cut anyway can free the part before it: from a pad,
    // where nobody sees the list, that start is refused.
    if (remote && !r.skipped.isEmpty())
        return refuse(QStringLiteral("Some toolpaths cannot be cut: %1.").arg(r.skipped.join(QStringLiteral(", "))),
                      QStringLiteral("Fix or turn them off, then send again."));
    // A document set up for tiling whose job needs more than one tile is
    // taller than the machine: streamed whole it drove Y past the far end.
    {
        const double tileH = m_doc->params().value("tile_height", "0").toDouble();
        if (m_doc->params().value("tiling_enabled") == QLatin1String("1") && tileH > 0
            && tileCount(r.ops, tileH) > 1)
            return refuse(QStringLiteral("This design is tiled: the job needs %1 tiles of %2 mm.")
                              .arg(QString::number(tileCount(r.ops, tileH)), QString::number(tileH)),
                          QStringLiteral("Export G-code makes one program per tile; run those one at a time."));
    }
    // Settings made at the computer for one run, which nobody at a pad sees:
    // an old surface map bent the new design's depths, and a left-on air
    // cut ran with the spindle off while the pad said "cutting started".
    if (remote && m_useMap->isChecked() && m_map.isValid())
        return refuse(QStringLiteral("The surface map is switched on at the computer."),
                      QStringLiteral("Turn it off there, or start the job at the computer."));
    // Z zeroed with the map on puts Z0 at the surface under X0 Y0, with it
    // off at the touched spot: run the other way, or with a map measured
    // again since, every depth was off by the map's height there (a pad
    // start, which wants the map off, cut 1.75 mm too deep).
    // GRBL's reported WCO Z carries the tool length offset (G43.1 after a
    // bit is measured); the zero left none. Either, as a G43.1 may still be
    // on its way.
    const double wcoNow = m_grbl->hasWorkOffset() ? m_grbl->workOffsetZ() : qQNaN();
    if (m_zZeroKnown && (std::abs(wcoNow - m_zZeroWco) < 0.005
                         || std::abs(wcoNow - m_grbl->toolLengthOffset() - m_zZeroWco) < 0.005
                         || std::abs(wcoNow - m_grbl->toolLengthOffsetAtWco() - m_zZeroWco) < 0.005)) {
        const MachineStatus st = m_grbl->status();
        const bool mapNow = m_useMap->isChecked() && m_map.isValid();
        const double want = mapNow ? m_map.offsetAt(m_zZeroMx - (st.mx - st.wx), m_zZeroMy - (st.my - st.wy)) : 0.0;
        const double off = want - m_zZeroSent;
        if (std::abs(off) > 0.05) {
            const bool same = m_zZeroWithMap == mapNow;
            const QString why = same
                ? QStringLiteral("Z was zeroed with another surface map than the one measured since")
                : QStringLiteral("Z was zeroed with the surface map %1, and it is %2 now")
                      .arg(m_zZeroWithMap ? QStringLiteral("on") : QStringLiteral("off"),
                           mapNow ? QStringLiteral("on") : QStringLiteral("off"));
            return refuse(QStringLiteral("%1: every depth would be %2 mm off.").arg(why).arg(std::abs(off), 0, 'f', 2),
                          same ? QStringLiteral("Zero Z again.") : QStringLiteral("Zero Z again, or switch the map back."));
        }
    }
    if (remote && m_airCut->isChecked())
        return refuse(QStringLiteral("Air cut is switched on at the computer."),
                      QStringLiteral("Turn it off there, or start the job at the computer."));
    // Surface map: every Z follows the measured surface. Applied to the
    // operations and posted again, so feeds are cut up along the way.
    QVector<Op> ops = r.ops;
    QString gcode = r.gcode;
    const bool useMap = m_useMap->isChecked() && m_map.isValid();
    if (useMap) {
        ops = applySurfaceMap(r.ops, m_map);
        gcode = GrblPost(true).generate(ops);
    }
    QStringList lines = gcode.split(QChar('\n'), Qt::SkipEmptyParts);
    const bool air = m_airCut->isChecked();
    if (air)
        lines = airCutTransform(lines, m_airLift->value());
    // Into the job from the top: all the way up first, then every rapid
    // that goes down travels across before it descends.
    lines = QStringList{QStringLiteral("G21")} + riseToTop() + safeRapidsTransform(lines);

    // The program inside the travel, at the current work zero (with the air
    // cut lift and the map's lifted rapids): jogs, Park and the map keep to
    // it, the job did not, and drove past the Z switch or into a hard stop.
    if (m_homed && m_grbl->status().valid) {
        const auto &st = m_grbl->status();
        const double off[3] = {st.mx - st.wx, st.my - st.wy, st.mz - st.wz};
        double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
        auto grow = [&](double x, double y, double z) {
            const double v[3] = {x, y, z};
            for (int k = 0; k < 3; ++k) {
                lo[k] = qMin(lo[k], v[k]);
                hi[k] = qMax(hi[k], v[k]);
            }
        };
        double px = 0, py = 0, pz = 0;
        for (const Op &o : ops) {
            if (o.kind != Op::Rapid && o.kind != Op::Feed && o.kind != Op::Arc)
                continue;
            grow(o.x, o.y, o.z);
            if (o.kind == Op::Arc) {
                // The arc's own extremes: where it crosses 0, 90, 180, 270 deg.
                const double cx = px + o.ci, cy = py + o.cj, r = std::hypot(o.ci, o.cj);
                const double a0 = std::atan2(py - cy, px - cx), a1 = std::atan2(o.y - cy, o.x - cx);
                double sweep = o.cw ? a0 - a1 : a1 - a0;
                while (sweep <= 1e-9)
                    sweep += 2 * M_PI;
                for (int q = 0; q < 4; ++q) {
                    const double a = q * M_PI / 2;
                    double t = o.cw ? a0 - a : a - a0;
                    while (t < 0)
                        t += 2 * M_PI;
                    while (t >= 2 * M_PI)
                        t -= 2 * M_PI;
                    if (t <= sweep)
                        grow(cx + r * std::cos(a), cy + r * std::sin(a), qMax(pz, o.z));
                }
            }
            px = o.x;
            py = o.y;
            pz = o.z;
        }
        if (lo[0] <= hi[0]) {
            QSettings sset;
            const auto machine = appsettings::machine(sset);
            const double travel[2] = {m_grbl->setting(130, machine.width), m_grbl->setting(131, machine.height)};
            const double top = -switchClearance(), lift = air ? m_airLift->value() : 0.0;
            QStringList over;
            for (int k = 0; k < 2; ++k) {
                const QChar ax = QChar::fromLatin1("XY"[k]);
                if (hi[k] + off[k] > top + 1e-6)
                    over << QStringLiteral("%1 %2 mm past its home switch").arg(ax).arg(hi[k] + off[k] - top, 0, 'f', 1);
                if (lo[k] + off[k] < -travel[k] - 1e-6)
                    over << QStringLiteral("%1 %2 mm past the far end of its travel")
                                .arg(ax).arg(-travel[k] - (lo[k] + off[k]), 0, 'f', 1);
            }
            if (hi[2] + lift + off[2] > top + 1e-6)
                over << QStringLiteral("Z %1 mm above the top of its travel")
                            .arg(hi[2] + lift + off[2] - top, 0, 'f', 1);
            if (!over.isEmpty())
                return refuse(QStringLiteral("The program goes off the machine."),
                              QStringLiteral("From the current work zero it reaches %1. Set the zero "
                                             "again, move the design, or lower the air cut lift.")
                                  .arg(over.join(QStringLiteral(", "))));
        }
    }

    // Confirmation gate: extents + time estimate, and an unmissable note about
    // whether this run will spin the spindle.
    const int npause = int(lines.filter(QStringLiteral("M0 ;")).size());
    if (npause > 0) {
        const BitSetterConfig bs = bitSetter();
        QString bad;
        if (!insideTravel(m_tcX->value(), m_tcY->value()))
            bad = QStringLiteral("tool-change spot (X %1  Y %2)").arg(m_tcX->value(), 0, 'f', 1)
                      .arg(m_tcY->value(), 0, 'f', 1);
        else if (bs.enabled && !insideTravel(bs.x, bs.y))
            bad = QStringLiteral("BitSetter position (X %1  Y %2)").arg(bs.x, 0, 'f', 1)
                      .arg(bs.y, 0, 'f', 1);
        if (!bad.isEmpty())
            return refuse(QStringLiteral("Position off the machine."),
                          QStringLiteral("This program changes tools, and the %1 is outside the "
                                         "machine's travel. Set it again on the BitSetter page "
                                         "(Use here) with the machine homed.").arg(bad));
        // Without a reference the first bit measured silently became it, and
        // the job carried on with a Z zero that may belong to another bit
        // (the BitZero pin, a failed reference probe): cut too deep by the
        // difference. The rehearsal refuses this too.
        if (bs.enabled && !m_haveRef)
            return refuse(QStringLiteral("There is no BitSetter reference yet."),
                          QStringLiteral("Zero Z with the BitSetter on (or press Measure tool "
                                         "with the bit Z was zeroed with), then run again."));
    }
    ShopAsk ask;
    ask.title = QStringLiteral("Run program");
    ask.headline = QStringLiteral("Stream %1 to the machine?")
                       .arg(m_doc->filePath().isEmpty()
                                ? QStringLiteral("the project")
                                : QFileInfo(m_doc->filePath()).fileName());
    ask.body = QStringLiteral("%1 toolpath(s), %2 lines\n%3")
                   .arg(r.done.size()).arg(lines.size())
                   .arg(statsSummary(computeStats(ops)));
    if (npause > 0)
        ask.body += QStringLiteral("\n\n%1 tool change(s): the machine parks at the "
                                   "tool-change spot and prompts%2.")
                        .arg(npause)
                        .arg(m_bsEnabled->isChecked()
                                 ? QStringLiteral(", then measures on the BitSetter")
                                 : QString());
    if (const QString w = toolReuseWarning(ops); !w.isEmpty())
        ask.body += QStringLiteral("\n\n\u26a0 ") + w;
    if (!r.skipped.isEmpty())
        ask.body += QStringLiteral("\n\n\u26a0 Left out (not cut): %1").arg(r.skipped.join(QStringLiteral(", ")));
    if (useMap) {
        ask.body += QStringLiteral("\n\nSurface map on: Z follows the measured surface (%1).")
                        .arg(m_map.summary());
        // Cutting moves only: the rapids to and from X0 Y0 lie outside any
        // grid that keeps clear of the stock edges.
        bool outside = false;
        for (const Op &o : r.ops)
            if ((o.kind == Op::Feed || o.kind == Op::Arc) && !m_map.covers(o.x, o.y, 1.0))
                outside = true;
        if (outside)
            ask.body += QStringLiteral(" The job reaches past the mapped area; the edge "
                                       "heights are held there.");
    }
    if (air) {
        ask.note = QStringLiteral("AIR CUT: spindle stripped, all Z +%1 mm")
                       .arg(m_airLift->value(), 0, 'f', 1);
    } else {
        ask.note = QStringLiteral("LIVE RUN: the spindle WILL start.");
        ask.noteDanger = true;
    }
    ask.okText = QStringLiteral("▶ Start");
    ask.defaultOk = false;
    if (remote) {
        // Asked on the pad instead; what it would have said goes in the log.
        log(QStringLiteral("started from pad: %1 — %2").arg(ask.headline, ask.note));
    } else if (!shopAsk(this, ask)) {
        return false;
    }

    if (useMap)
        log(QStringLiteral("surface map applied: %1").arg(m_map.summary()));
    if (air)
        log(QStringLiteral("air cut: spindle off, Z lifted %1 mm").arg(m_airLift->value(), 0, 'f', 1));
    // The post ends with M05 M02, and GRBL's M2 stops the spindle too. Take
    // those off so the ending is ours: up to the top first, then spindle off
    // (see streamFinished). Only the trailing lines go; a tool-change M05
    // in the middle stays.
    while (!lines.isEmpty()) {
        const QString last = lines.last().trimmed().toUpper();
        if (last == QLatin1String("M05") || last == QLatin1String("M5")
            || last == QLatin1String("M02") || last == QLatin1String("M2")
            || last == QLatin1String("M30"))
            lines.removeLast();
        else
            break;
    }
    // Preparing and asking ran the event loop: a pad may have started a job
    // or opened another design meanwhile.
    if (isBusy() || m_held || m_grbl->isParkedForTool() || !m_grbl->isConnected())
        return refuse(QStringLiteral("The machine is busy."));
    // An end stop may have fired meanwhile: the controller is locked and
    // would answer none of the program.
    if (m_grbl->needsReset() || m_machineState != QLatin1String("Idle"))
        return refuse(QStringLiteral("The machine is not idle (%1).")
                          .arg(m_grbl->needsReset() ? QStringLiteral("stopped on a limit")
                               : m_machineState.isEmpty() ? QStringLiteral("no status yet")
                                                          : m_machineState),
                      QStringLiteral("In Alarm, press Initialize first."));
    if (!m_homed)
        return refuse(QStringLiteral("The machine needs Initialize (homing) first: %1.").arg(m_unhomedWhy));
    // A jog just sent still reads Idle until the next status report; the
    // program's first lines would be answered error:9 and the job aborted.
    if (!standingStill())
        return refuse(QStringLiteral("The machine is still moving (%1).").arg(stillMovingWhy()));
    // Only now: a refusal above leaves a hold (and its Resume button) as it was.
    setHeld(false);
    if (m_doc->filePath() != docAtStart || m_doc->loadCount() != loadAtStart)
        return refuse(QStringLiteral("Another design was opened meanwhile; run it again."));
    m_endOfJobPending = true;
    m_jobTools = toolChanges(ops);
    // GRBL keeps overrides until M2 or a reset, and the job's M2 is stripped:
    // three +10 % taps ran every later job at 130 %. Each job starts at 100 %.
    m_grbl->sendRealtime(char(0x90));   // feed
    m_grbl->sendRealtime(char(0x95));   // rapids
    m_grbl->sendRealtime(char(0x99));   // spindle
    m_grbl->startStream(lines);
    return true;
}

bool MachinePanel::startFromRemote(QString *why)
{
    QString dummy;
    return runProgram(why ? why : &dummy);
}

bool MachinePanel::reloadMachine(QString *why)
{
    if (isBusy()) {
        if (why) *why = QStringLiteral("the machine is busy");
        return false;
    }
    const bool wasConnected = m_grbl->isConnected();
    if (wasConnected)
        m_grbl->disconnectPort();
    // The last machine's measured tool length is not this one's: it would be
    // sent again on the new controller's startup.
    m_grbl->applyToolLengthOffset(0);
    resetFlow();
    m_savedHomedAt.clear();
    // A machine with no port yet must not inherit the last one's.
    const QString port = QSettings().value(QStringLiteral("machine/port")).toString();
    if (port.isEmpty())
        m_ports->setEditText(QString());
    loadSettings();
    // The BitZero page read its settings once: on the new machine it probed
    // with the old one's plate numbers, and the next edit saved them over.
    if (m_bzPanel)
        m_bzPanel->reload();
    {
        const SurfaceMap saved = SurfaceMap::fromJson(
            QSettings().value(QStringLiteral("machine/heightmap/map")).toString());
        m_loading = true;
        m_map = saved;
        m_useMap->setChecked(saved.isValid()
                             && QSettings().value(QStringLiteral("machine/heightmap/use"), false).toBool());
        m_loading = false;
        refreshMapUi();
    }
    updateOffsetLabels();
    refreshJobInfo();
    if (wasConnected && !port.isEmpty())
        m_grbl->connectPort(currentPortName());
    return true;
}

QJsonObject MachinePanel::statusJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("connected"), m_grbl->isConnected());
    o.insert(QStringLiteral("port"), currentPortName());
    o.insert(QStringLiteral("state"), m_grbl->isConnected() ? m_machineState : QString());
    o.insert(QStringLiteral("homed"), m_grbl->isConnected() && m_homed);
    o.insert(QStringLiteral("busy"), isBusy());
    o.insert(QStringLiteral("streaming"), m_grbl->isStreaming());
    o.insert(QStringLiteral("job"), m_doc && !m_doc->filePath().isEmpty()
                                        ? QFileInfo(m_doc->filePath()).fileName() : QString());
    return o;
}

} // namespace c2d
