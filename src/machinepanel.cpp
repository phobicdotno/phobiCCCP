#include "machinepanel.h"
#include "probecycles.h"
#include "machinemacros.h"
#include "jobview.h"
#include "shopdialog.h"
#include "grblcodes.h"
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
#include "ncprogram.h"
#include "surfacing.h"
#include "jobhistory.h"
#include "machineprofiles.h"
#include "grblsettingsdialog.h"
#include "machinecounters.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPoint>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QFile>
#include <QFileDialog>
#include <QDir>
#include <QDateTime>
#include <QStandardPaths>
#include <QFrame>
#include <QShowEvent>
#include <QLineEdit>
#include <QStackedWidget>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <cmath>
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
#include <algorithm>
#include <iterator>
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

// An Xbox 360 pad: white body with two grips, left stick up and D-pad down on
// the left, the coloured buttons up and the right stick down on the right,
// the green guide button in the middle.
static QIcon xboxIcon()
{
    QPixmap pm(QSize(40, 24) * 4);
    pm.setDevicePixelRatio(4);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath left, right, mid;
    left.addEllipse(QPointF(9.5, 12.5), 8.5, 10.5);
    right.addEllipse(QPointF(30.5, 12.5), 8.5, 10.5);
    mid.addRoundedRect(QRectF(9.5, 3.5, 21, 13), 3, 3);
    p.setPen(QPen(QColor(0x8b, 0x90, 0x9a), 1.0));
    p.setBrush(QColor(0xe2, 0xe4, 0xe9));
    p.drawPath(left.united(right).united(mid));
    p.setPen(Qt::NoPen);
    // sticks
    p.setBrush(QColor(0x3a, 0x3d, 0x44));
    p.drawEllipse(QPointF(9.5, 9.0), 2.7, 2.7);
    p.drawEllipse(QPointF(25.0, 14.6), 2.7, 2.7);
    // d-pad
    p.setBrush(QColor(0x2a, 0x2d, 0x33));
    p.drawRoundedRect(QRectF(14.1, 11.6, 1.8, 6.0), 0.5, 0.5);
    p.drawRoundedRect(QRectF(12.0, 13.7, 6.0, 1.8), 0.5, 0.5);
    // guide
    p.setBrush(QColor(0x6c, 0xb8, 0x3a));
    p.drawEllipse(QPointF(20, 8.6), 1.9, 1.9);
    // buttons
    struct B { QPointF at; QColor col; };
    const B bs[4] = {{QPointF(30.5, 5.9), QColor(0xe8, 0xb8, 0x20)},   // Y
                     {QPointF(27.6, 8.8), QColor(0x3b, 0x6f, 0xd8)},   // X
                     {QPointF(33.4, 8.8), QColor(0xd8, 0x3a, 0x3a)},   // B
                     {QPointF(30.5, 11.7), QColor(0x4f, 0xa8, 0x3a)}}; // A
    for (const B &b : bs) {
        p.setBrush(b.col);
        p.drawEllipse(b.at, 1.7, 1.7);
    }
    return QIcon(pm);
}

// The pad style the mapper last set up ("nes", "snes" or "xbox").
static QIcon padIcon()
{
    const QString style = QSettings().value(QStringLiteral("ui/padStyle")).toString();
    return style == QLatin1String("nes") ? nesIcon()
         : style == QLatin1String("xbox") ? xboxIcon() : snesIcon();
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

// One of the Machine page's own questions or messages (shopAsk, shopTell,
// the export's progress) is on screen.
static bool questionOpen(const QWidget *panel)
{
    const QWidgetList tops = QApplication::topLevelWidgets();
    for (const QWidget *w : tops) {
        const QWidget *p = w->parentWidget();
        if (w->isVisible() && w->isModal() && p && (p == panel || panel->isAncestorOf(p)))
            return true;
    }
    return false;
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
    // The Z-zero record follows the controller: another port, another record.
    connect(m_ports, &QComboBox::currentTextChanged, this, [this] {
        if (!m_loading)
            loadZZero();
    });
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
    // A service that is due: one amber line, never in the way of a job.
    m_dueLabel = new QLabel(body);
    m_dueLabel->setObjectName(QStringLiteral("serviceDue"));
    m_dueLabel->setStyleSheet(QStringLiteral("color: #f2a536; font-weight: bold;"));
    m_dueLabel->setToolTip(QStringLiteral("Service reminder: open Counters… to mark it done"));
    m_dueLabel->hide();
    lay->addWidget(m_dueLabel);
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
        m_idleOnly << z;
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
    m_idleOnly << zeroXY << zeroAll;
    auto *gotoBtn = bigBtn(new QPushButton(QStringLiteral("Go to X0 Y0"), posCard), 46);
    gotoBtn->setToolTip(QStringLiteral("Raise to safe Z, then rapid to work X0 Y0"));
    connect(gotoBtn, &QPushButton::clicked, this, [this] { goToXY0(); });
    pv->addWidget(gotoBtn);
    m_idleOnly << gotoBtn;
    auto *measureBig = bigBtn(new QPushButton(QStringLiteral("Measure bit (BitSetter)"), posCard), 46);
    measureBig->setToolTip(QStringLiteral(
        "Go to the button, probe twice, come back. The first measurement after "
        "zeroing Z is the reference; later ones set the G43.1 length offset."));
    connect(measureBig, &QPushButton::clicked, this, [this] { measureTool(Phase::MeasureManual); });
    pv->addWidget(measureBig);
    m_idleOnly << measureBig;
    auto *probeBig = bigBtn(new QPushButton(QStringLiteral("Probe zero (BitZero)…"), posCard), 46);
    probeBig->setToolTip(QStringLiteral(
        "Find the stock corner, or one edge, or the surface, with Carbide's BitZero V2 "
        "touch plate and set work zero there."));
    connect(probeBig, &QPushButton::clicked, this, [this] { openBitZero(); });
    pv->addWidget(probeBig);
    m_idleOnly << probeBig;
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
    m_step->setCurrentIndex(1);   // 1 mm
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
    m_speed->setCurrentIndex(3);   // rapid, 5000
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
    // The middle column swaps the jog card for the job view while a job
    // streams: what is cutting now, every pass to come, and the time left.
    m_midStack = new CurrentPageStack(body);
    m_midStack->addWidget(jogCard);
    QFrame *jobViewCard = card(body);
    auto *jvv = static_cast<QVBoxLayout *>(jobViewCard->layout());
    jvv->addWidget(caption(QStringLiteral("Job progress"), jobViewCard));
    m_jobView = new JobView(jobViewCard);
    connect(m_jobView, &JobView::startHereRequested, this, [this](int sec, int pass) { startHere(sec, pass); });
    jvv->addWidget(m_jobView, 1);
    m_midStack->addWidget(jobViewCard);
    m_midStack->setCurrentIndex(0);
    cols->addWidget(m_midStack, 11);

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
    // After a crash or a power cut: the job of this design, from the start
    // of the step it stopped in. Shown only when that can be done.
    m_continueBtn = new QPushButton(jobCard);
    m_continueBtn->setObjectName(QStringLiteral("continueStopped"));
    m_continueBtn->setMinimumHeight(56);
    m_continueBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: #f2a536; color: #16181d; border: none; border-radius: 8px;"
        " font-size: 16px; font-weight: 700; padding: 6px 14px; }"
        "QPushButton:pressed { background: #c8861f; }"));
    m_continueBtn->setVisible(false);
    connect(m_continueBtn, &QPushButton::clicked, this, [this] { continueWhereStopped(); });
    bv->addWidget(m_continueBtn);
    {
        // Any G-code file runs here too (a tiled design's tile programs,
        // a program made elsewhere): the same checks, tool changes and end.
        auto *fileRow = new QHBoxLayout;
        m_openNcBtn = bigBtn(new QPushButton(QStringLiteral("Open G-code…"), jobCard), 52);
        m_openNcBtn->setToolTip(QStringLiteral("Run a G-code file (.nc, .gcode, .tap, .ngc) instead of "
                                               "the open design"));
        m_backToDesignBtn = bigBtn(new QPushButton(QStringLiteral("Back to design"), jobCard), 52);
        m_backToDesignBtn->setToolTip(QStringLiteral("Put the G-code file away and run the open design"));
        m_backToDesignBtn->setVisible(false);
        connect(m_openNcBtn, &QPushButton::clicked, this, [this] { openProgramFile(); });
        connect(m_backToDesignBtn, &QPushButton::clicked, this, [this] { backToDesign(); });
        auto *surfBtn = bigBtn(new QPushButton(QStringLiteral("Surfacing…"), jobCard), 52);
        surfBtn->setToolTip(QStringLiteral("Flatten a spoilboard or a slab: a rectangle cut in "
                                           "zigzag lines, no design needed"));
        connect(surfBtn, &QPushButton::clicked, this, [this] { openSurfacing(); });
        auto *historyBtn = bigBtn(new QPushButton(QStringLiteral("History"), jobCard), 52);
        historyBtn->setToolTip(QStringLiteral("The last jobs: when, how long, how they ended"));
        connect(historyBtn, &QPushButton::clicked, this, [this] { jobhistory::showDialog(this); });
        fileRow->addWidget(m_openNcBtn);
        fileRow->addWidget(m_backToDesignBtn);
        fileRow->addWidget(surfBtn);
        fileRow->addWidget(historyBtn);
        auto *countersBtn = bigBtn(new QPushButton(QStringLiteral("Counters…"), jobCard), 52);
        countersBtn->setToolTip(QStringLiteral("Spindle and moving hours, time per bit, and service reminders"));
        connect(countersBtn, &QPushButton::clicked, this, [this] { openCounters(); });
        fileRow->addWidget(countersBtn);
        bv->addLayout(fileRow);
    }
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
    // "More": feed, spindle and rapid overrides and air cut, folded so the big buttons stay big.
    auto *moreToggle = new QToolButton(jobCard);
    moreToggle->setCheckable(true);
    moreToggle->setAutoRaise(true);
    moreToggle->setArrowType(Qt::RightArrow);
    moreToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    moreToggle->setText(QStringLiteral("Overrides, air cut, surface map"));
    moreToggle->setFocusPolicy(Qt::NoFocus);
    bv->addWidget(moreToggle);
    auto *more = new QWidget(jobCard);
    auto *mv = new QVBoxLayout(more);
    mv->setContentsMargins(0, 0, 0, 0);
    mv->setSpacing(6);
    // GRBL realtime overrides (feed, spindle, rapid) — take effect
    // mid-program, no queue flush.
    auto *ovRow = new QHBoxLayout;
    QHBoxLayout *ovTo = ovRow;
    auto ovName = [&](const QString &t) {
        auto *l = new QLabel(t, more);
        l->setMinimumWidth(90);
        ovTo->addWidget(l);
    };
    auto ovBtn = [&](const QString &t, char code, const QString &tip) {
        auto *b = new QPushButton(t, more);
        b->setMinimumSize(60, 34);
        b->setToolTip(tip);
        ovTo->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, code] { m_grbl->sendRealtime(code); });
    };
    ovName(QStringLiteral("feed ovr"));
    ovBtn(QStringLiteral("−10%"), char(0x92), QStringLiteral("Feed 10 % slower"));
    ovBtn(QStringLiteral("100%"), char(0x90), QStringLiteral("Feed back to 100 %"));
    ovBtn(QStringLiteral("+10%"), char(0x91), QStringLiteral("Feed 10 % faster"));
    // What the controller actually runs at (its Ov: report), so three taps
    // show as 130 % instead of having to be counted.
    m_ovLabel = new QLabel(QStringLiteral("feed 100 %"), more);
    m_ovLabel->setObjectName(QStringLiteral("ovLabel"));
    m_ovLabel->setToolTip(QStringLiteral("Feed, rapid and spindle override as the controller "
                                         "reports them. Every job starts at 100 %."));
    m_ovLabel->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 600; padding-left: 8px;"));
    ovRow->addWidget(m_ovLabel);
    ovRow->addStretch(1);
    mv->addLayout(ovRow);
    // Spindle: 10 % steps like the feed. Rapid: GRBL has only 25, 50 and 100 %.
    auto *spinRow = new QHBoxLayout;
    ovTo = spinRow;
    ovName(QStringLiteral("spindle ovr"));
    ovBtn(QStringLiteral("−10%"), char(0x9B), QStringLiteral("Spindle 10 % slower"));
    ovBtn(QStringLiteral("100%"), char(0x99), QStringLiteral("Spindle back to 100 %"));
    ovBtn(QStringLiteral("+10%"), char(0x9A), QStringLiteral("Spindle 10 % faster"));
    spinRow->addStretch(1);
    mv->addLayout(spinRow);
    auto *rapidRow = new QHBoxLayout;
    ovTo = rapidRow;
    ovName(QStringLiteral("rapid ovr"));
    ovBtn(QStringLiteral("25%"), char(0x97), QStringLiteral("Rapid moves at 25 %"));
    ovBtn(QStringLiteral("50%"), char(0x96), QStringLiteral("Rapid moves at 50 %"));
    ovBtn(QStringLiteral("100%"), char(0x95), QStringLiteral("Rapid moves back to 100 %"));
    rapidRow->addStretch(1);
    mv->addLayout(rapidRow);
    // Spindle by hand: check runout, warm the bearings. Idle and homed only.
    auto *spRow = new QHBoxLayout;
    ovTo = spRow;
    ovName(QStringLiteral("spindle"));
    m_rpmSpin = new QSpinBox(more);
    m_rpmSpin->setRange(0, 100000);
    m_rpmSpin->setSingleStep(1000);
    m_rpmSpin->setSuffix(QStringLiteral(" rpm"));
    m_rpmSpin->setMinimumSize(130, 44);
    m_rpmSpin->setValue(int(spindleMaxRpm()));
    m_rpmSpin->setToolTip(QStringLiteral("Speed for On, kept between the controller's "
                                         "lowest and highest spindle speed ($31, $30)"));
    spRow->addWidget(m_rpmSpin);
    m_spinOnBtn = new QPushButton(QStringLiteral("On"), more);
    m_spinOnBtn->setMinimumSize(70, 44);
    m_spinOnBtn->setToolTip(QStringLiteral("Spindle on at this speed (M3). Only when the "
                                           "machine stands still and no job runs"));
    m_spinOffBtn = new QPushButton(QStringLiteral("Off"), more);
    m_spinOffBtn->setMinimumSize(70, 44);
    m_spinOffBtn->setToolTip(QStringLiteral("Spindle off (M5); ends a warm-up too"));
    m_warmBtn = new QPushButton(QStringLiteral("Warm-up"), more);
    m_warmBtn->setMinimumSize(150, 44);
    m_warmBtn->setToolTip(QStringLiteral("Warm the bearings: half speed 2 min, three "
                                         "quarters 2 min, full speed 1 min, then off. "
                                         "Press again to stop"));
    spRow->addWidget(m_spinOnBtn);
    spRow->addWidget(m_spinOffBtn);
    spRow->addWidget(m_warmBtn);
    spRow->addStretch(1);
    mv->addLayout(spRow);
    m_warmTimer = new QTimer(this);
    m_warmTimer->setInterval(1000);
    connect(m_warmTimer, &QTimer::timeout, this, [this] {
        if (--m_warmLeft <= 0)
            warmupStep();
        else
            updateSpindleButtons();
    });
    connect(m_spinOnBtn, &QPushButton::clicked, this, [this] { spindleOn(); });
    connect(m_spinOffBtn, &QPushButton::clicked, this, [this] { spindleOff(); });
    connect(m_warmBtn, &QPushButton::clicked, this, [this] { toggleWarmup(); });
    updateSpindleButtons();              // off until connected, homed and idle
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
    // Dust collector / coolant with the job, kept per machine.
    auto *coolRow = new QHBoxLayout;
    m_coolantAuto = new QCheckBox(QStringLiteral("Dust/coolant on with the job"), more);
    m_coolantAuto->setToolTip(QStringLiteral(
        "Switch the dust collector or coolant on (M8 or M7) after the spindle starts "
        "and off (M9) before it stops: at the end of the job, just before the spindle "
        "goes off at the park spot, and before each tool change."));
    m_coolantCode = new QComboBox(more);
    m_coolantCode->addItem(QStringLiteral("M8 flood"));
    m_coolantCode->addItem(QStringLiteral("M7 mist"));
    m_coolantCode->setMinimumHeight(34);
    m_coolantCode->setToolTip(QStringLiteral(
        "Which output the relay hangs on. Most Shapeoko dust collector relays use M8 (flood)."));
    m_coolantCode->setEnabled(false);
    connect(m_coolantAuto, &QCheckBox::toggled, m_coolantCode, &QWidget::setEnabled);
    connect(m_coolantAuto, &QCheckBox::toggled, this, [this] { saveSettings(); });
    connect(m_coolantCode, &QComboBox::currentIndexChanged, this, [this] { saveSettings(); });
    coolRow->addWidget(m_coolantAuto);
    coolRow->addWidget(m_coolantCode);
    coolRow->addStretch(1);
    mv->addLayout(coolRow);
    // The controller's own $ settings ($$), with a backup file.
    auto *ctlRow = new QHBoxLayout;
    auto *ctlBtn = new QPushButton(QStringLiteral("Controller settings…"), more);
    ctlBtn->setMinimumHeight(44);
    ctlBtn->setToolTip(QStringLiteral(
        "See and change the controller's own settings ($$): speeds, acceleration, "
        "steps/mm, travel, homing. Save them to a file and restore them."));
    connect(ctlBtn, &QPushButton::clicked, this, [this] { openControllerSettings(); });
    m_idleOnly << ctlBtn;
    ctlRow->addWidget(ctlBtn);
    ctlRow->addStretch(1);
    mv->addLayout(ctlRow);
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
    auto *quickBtn = bigBtn(new QPushButton(QStringLiteral("Quick move…"), body), 44);
    quickBtn->setToolTip(QStringLiteral(
        "Pick one of 25 spots over the table: the head goes all the way up "
        "(if it is not already), then straight there. The outer spots are the "
        "ends of the travel."));
    connect(quickBtn, &QPushButton::clicked, this, [this] { openQuickMove(); });
    auto *unlockBtn = bigBtn(new QPushButton(QStringLiteral("Unlock"), body), 44);
    unlockBtn->setToolTip(QStringLiteral("Clear alarm lock ($X)"));
    connect(unlockBtn, &QPushButton::clicked, this, [this] { unlock(); });
    auto *bsBtn = bigBtn(new QPushButton(QStringLiteral("BitSetter"), body), 44);
    bsBtn->setToolTip(QStringLiteral("BitSetter position, probe feeds, tool-change spot, "
                                     "reference and rehearsal"));
    auto *hoverBtn = bigBtn(new QPushButton(QStringLiteral("Hover 5 cm"), body), 44);
    hoverBtn->setToolTip(QStringLiteral(
        "Dry run before cutting: the spindle (off) traces the outer rectangle of "
        "the job 50 mm above the work zero, so you can check the stock and the zero."));
    connect(hoverBtn, &QPushButton::clicked, this, [this] { hoverDryRun(); });
    m_coolantBtn = bigBtn(new QPushButton(QStringLiteral("Dust/Coolant off"), body), 44);
    m_coolantBtn->setObjectName(QStringLiteral("coolant"));
    m_coolantBtn->setCheckable(true);
    m_coolantBtn->setToolTip(QStringLiteral(
        "Dust collector or coolant on (M8, or M7 when set to mist) or off (M9). "
        "Shows what the controller reports."));
    connect(m_coolantBtn, &QPushButton::clicked, this, [this] { toggleCoolant(); });
    m_idleOnly << homeBtn << parkBtn << quickBtn << unlockBtn << bsBtn << hoverBtn << m_coolantBtn;
    foot->addWidget(homeBtn);
    foot->addWidget(parkBtn);
    foot->addWidget(quickBtn);
    foot->addWidget(hoverBtn);
    foot->addWidget(unlockBtn);
    foot->addWidget(bsBtn);
    foot->addWidget(m_coolantBtn);
    foot->addStretch(1);
    lay->addLayout(foot);

    // ---- The operator's macros (machinemacros.h): one button each, and
    // the editor. Hidden with the other idle-only buttons while cutting.
    m_macroRow = new QWidget(body);
    auto *macroLay = new QHBoxLayout(m_macroRow);
    macroLay->setContentsMargins(0, 0, 0, 0);
    macroLay->setSpacing(8);
    for (int i = 0; i < machinemacros::kMax; ++i) {
        auto *b = bigBtn(new QPushButton(m_macroRow), 44);
        b->setObjectName(QStringLiteral("macro"));
        b->hide();
        connect(b, &QPushButton::clicked, this, [this, i] { runMacro(i); });
        macroLay->addWidget(b);
        m_macroBtns << b;
    }
    auto *macroEdit = bigBtn(new QPushButton(QStringLiteral("Macros…"), m_macroRow), 44);
    macroEdit->setToolTip(QStringLiteral(
        "Your own buttons for this machine: a name and a few lines of G-code "
        "each. Add, change, delete and reorder them here."));
    connect(macroEdit, &QPushButton::clicked, this, [this] { editMacros(); });
    macroLay->addWidget(macroEdit);
    macroLay->addStretch(1);
    m_idleOnly << m_macroRow;
    lay->addWidget(m_macroRow);

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
        if (m_liftHold) {
            // Back down: spindle on and up to speed, to where the program
            // was, its feed and motion mode, then on with the cut.
            if (!m_grbl->isHeldAtPoint()) {
                log(QStringLiteral("resume: still lifting - press Resume again in a moment"));
                return;
            }
            const QStringList down = m_grbl->resumeLinesAfterHold();
            log(QStringLiteral("resume: spindle on, back down, then the program"));
            if (m_jobView)
                m_jobView->linesInserted(int(down.size()));
            m_liftHold = false;
            m_pauseBtn->setText(QStringLiteral("⏸ Hold"));
            m_grbl->continueAfterHold(down);
        } else if (!m_held && m_homed && !riseToTop().isEmpty() && m_grbl->canLiftHold()) {
            // Z to the top, then the spindle off; XY stays where it is.
            QStringList up = riseToTop();
            up << QStringLiteral("M5");
            if (m_grbl->queueHoldPoint(up)) {
                if (m_jobView)
                    m_jobView->linesInserted(int(up.size()) + 1);
                m_liftHold = true;
                m_pauseBtn->setText(QStringLiteral("⏵ Resume"));
                log(QStringLiteral("hold: finishing the lines already sent, then Z up and spindle off"));
            }
        } else if (!m_held) {
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
        endWarmup(QStringLiteral("warm-up ended by Stop (the reset stops the spindle)"));
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
    // Command line: one line to the controller, under the log. On the shop
    // PC the on-screen keyboard comes up by itself when it gets focus (see
    // main.cpp, accessibility on).
    auto *mdiRow = new QHBoxLayout;
    mdiRow->setSpacing(6);
    m_mdi = new QLineEdit(this);
    m_mdi->setObjectName(QStringLiteral("mdi"));
    m_mdi->setPlaceholderText(QStringLiteral("Type G-code or a $ command, then Send"));
    m_mdi->setMinimumHeight(40);
    m_mdi->setFont(QFont(QStringLiteral("monospace")));
    m_mdi->setToolTip(QStringLiteral(
        "One line to the controller (G0 X10, $$, $#...). Up and Down bring back "
        "earlier lines. Not while a job runs. $H homes like Initialize, $X unlocks "
        "like Unlock; ?, ! and ~ alone are status, hold and resume."));
    m_mdi->installEventFilter(this);
    auto *mdiSend = new QPushButton(QStringLiteral("Send"), this);
    mdiSend->setObjectName(QStringLiteral("big"));
    mdiSend->setMinimumSize(90, 40);
    mdiSend->setAutoDefault(false);
    mdiRow->addWidget(m_mdi, 1);
    mdiRow->addWidget(mdiSend);
    connLay->addLayout(mdiRow);
    m_mdiHistory = QSettings().value(QStringLiteral("ui/mdiHistory")).toStringList();
    auto sendMdi = [this] {
        if (sendTyped(m_mdi->text()))
            m_mdi->clear();
    };
    connect(m_mdi, &QLineEdit::returnPressed, this, sendMdi);
    connect(mdiSend, &QPushButton::clicked, this, sendMdi);
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
        m_connectedPort = p;
        loadZZero();                      // this controller's record
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
    connect(m_grbl, &GrblStreamer::settingsListed, this, [this] { autoBackupSettings(); });
    connect(m_grbl, &GrblStreamer::disconnected, this, [this] {
        ++m_stopSeq;
        flushCounters();
        m_counterClock.invalidate();
        // Nothing more can be sent; the spindle stays as the controller has it.
        endWarmup(QStringLiteral("warm-up ended: not connected any more (the spindle was not "
                                 "told to stop)"));
        m_rpmRangeSet = false;
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
        if (m_jobView)
            m_jobView->setProgress(a, t);
        saveResumePoint(a);
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
            const int n = l.mid(6, 1).toInt();
            log(QStringLiteral("!! %1. %2 Unlocking ($X) now; no need to Initialize.")
                    .arg(grblcodes::label(true, n), grblcodes::alarm(n).fix));
            QTimer::singleShot(300, this, [this] { m_grbl->sendCommand(QStringLiteral("$X")); });
        }
        // Every other alarm (hard/soft limit, reset while moving, homing
        // failures) loses the position: no machine-coordinate move until the
        // next homing cycle.
        if (l.startsWith(QLatin1String("ALARM:"))
            && !l.startsWith(QLatin1String("ALARM:4")) && !l.startsWith(QLatin1String("ALARM:5"))) {
            m_homed = false;
            bool isAlarm = true;
            int n = 0;
            grblcodes::find(l, &isAlarm, &n);
            m_unhomedWhy = QStringLiteral("%1 (ALARM:%2)").arg(grblcodes::alarm(n).brief).arg(n);
            if (n != 8 && n != 9)       // those two get their own line below
                log(QStringLiteral("!! %1. %2").arg(grblcodes::label(true, n), grblcodes::alarm(n).fix));
            // The end-of-job rise and spindle stop come after the program:
            // an alarm there does not undo the job, but it does lock the
            // controller and lose the position.
            if (!m_grbl->isStreaming() && m_jobView && m_jobView->isFinished())
                log(QStringLiteral("!! %1 came after the job finished (on the way up / spindle off): the job "
                                   "is done, but the controller is locked and lost its position. Press "
                                   "Initialize to home it again.").arg(l.trimmed()));
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
            const int n = l.mid(6, 1).toInt();
            log(QStringLiteral("!! %1. %2").arg(grblcodes::label(true, n), grblcodes::alarm(n).fix));
        }
    });
    connect(m_grbl, &GrblStreamer::errorOccurred, this, [this](const QString &e) {
        // A bare "error:22" from the controller gets its meaning beside it.
        bool isAlarm = false;
        int n = 0;
        const bool coded = grblcodes::find(e, &isAlarm, &n);
        log(coded && e.trimmed() == QStringLiteral("error:%1").arg(n)
                ? QStringLiteral("!! %1").arg(grblcodes::label(false, n))
                : QStringLiteral("!! %1").arg(e));
        // The program stopped on a code: streamFinished(false) comes next and
        // explains it in a popup.
        if (coded && e.startsWith(QLatin1String("program stopped:")))
            m_stopCode = (isAlarm ? n : -n);
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
        // Finished: nothing to continue. Stopped: the place stays saved.
        if (ok && !m_runPath.isEmpty())
            clearResumePoint();
        m_runPath.clear();
        m_runHash.clear();
        if (m_jobView) {
            m_jobView->setProgress(ok ? m_progress->maximum() : m_progress->value(), m_progress->maximum());
            m_jobView->setParked(false);
            // Stopped, cancelled, reset or refused: the step it was in says
            // so, here and on the pad, instead of still running.
            if (!ok)
                m_jobView->setStopped();
        }
        setJobRunning(false);
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
        // Stopped on a GRBL error or alarm: say in plain words what it was,
        // why it likely happened here and what to do. Not over a rehearsal's
        // own summary.
        const int stopCode = m_stopCode;
        m_stopCode = 0;
        if (!rehearsal) {
            recordJobEnd(ok ? QStringLiteral("finished")
                         : m_hist.cancelled ? QStringLiteral("cancelled")
                         : stopCode > 0 ? QStringLiteral("alarm")
                         : stopCode < 0 ? QStringLiteral("error") : QStringLiteral("stopped"),
                         stopCode > 0 ? stopCode : -stopCode);
            jobhistory::chime(this);
        }
        // It does not wait for OK: the machine still needs Initialize or
        // Unlock, and the serial line keeps being read.
        if (!ok && stopCode != 0 && !rehearsal) {
            const bool isAlarm = stopCode > 0;
            const int n = isAlarm ? stopCode : -stopCode;
            if (m_stopPopup)
                m_stopPopup->close();
            m_stopPopup = shopShow(this, QStringLiteral("Job stopped"), grblcodes::label(isAlarm, n),
                                   grblcodes::details(isAlarm, n));
        }
        // The job card says how it ended, not a bare percentage that reads
        // like a job still going.
        m_progress->setFormat(ok ? QStringLiteral("✓ Finished")
                                 : QStringLiteral("Stopped at %p%"));
        // A finished job ends parked: all the way up, across to the park
        // spot (front right, beside the BitSetter), and only then the
        // spindle off. A stopped one was reset, which already killed the
        // spindle, and stays where it is.
        if (endJob) {
            // Once the machine stands still: the last lines are acked when
            // planned, so the position read now is from before the program's
            // own final retract, and a rise from it could go down.
            m_endRisePending = true;
            whenIdle([this] {
                m_endRisePending = false;
                m_grbl->startMacro(endOfJobMoves());
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
    loadCounters();
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
        std::fill(std::begin(m_padAxisDir), std::end(m_padAxisDir), 0);
        log(QStringLiteral("gamepad disconnected"));
    });
    connect(m_pad, &Gamepad::buttonChanged, this, &MachinePanel::gamepadButton);
    connect(m_pad, &Gamepad::axisChanged, this, &MachinePanel::gamepadAxis);
    m_pad->start();
}

MachinePanel::~MachinePanel()
{
    flushCounters();
    disconnect(m_grbl, nullptr, this, nullptr);
    // The streamer closes the port as it is deleted, after this panel's own
    // members are gone. A surface map still probing hears that and reports
    // "failed" to the panel: quitting while it mapped wrote to the log and the
    // BitZero panel of a half-destroyed window. The gamepad likewise.
    disconnect(m_mapper, nullptr, this, nullptr);
    disconnect(m_pad, nullptr, this, nullptr);
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
        // Tried again on every line (the disk may come back), but said only
        // once: a log that is quietly not kept is missed when it is needed.
        static bool told = false;
        if (!file.open(QIODevice::Append | QIODevice::Text) && !told) {
            told = true;
            m_console->appendPlainText(QStringLiteral("!! cannot write %1: %2 — this log is on screen only")
                                           .arg(file.fileName(), file.errorString()));
        }
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
    // A direction pressed while another is still held (a pad flipped from +
    // to -, a second arrow key): the held jog stops the way a release stops
    // it, with the second cancel for a $J= still unread in GRBL's buffer.
    // One cancel alone let that jog run on in the old direction. A press
    // not yet held is dropped, not taken as a tap.
    if (m_holdJogging) endHoldJog(); else stopHoldJog();
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
    if ((m_bzPanel->mode() == BitZeroPanel::Corner || m_bzPanel->mode() == BitZeroPanel::ZOnly
         || m_bzPanel->mode() == BitZeroPanel::Plate)
        && bitSetter().enabled && !bitSetterReachable(bitSetter(), QStringLiteral("BitZero")))
        return;
    // The plate finds the stock's lower-left corner and its top. A job whose
    // zero is elsewhere (centre, another corner, the spoilboard) would run
    // from the wrong point unless the zero is moved there afterwards.
    if (m_doc && !jobZeroIsDefault(*m_doc)) {
        const auto mode = m_bzPanel->mode();
        const bool xyOff = jobZeroXY(*m_doc) != ZeroXY::LowerLeft
                           && (mode == BitZeroPanel::Corner || mode == BitZeroPanel::XOnly
                               || mode == BitZeroPanel::YOnly);
        const bool zOff = jobZeroBottom(*m_doc)
                          && (mode == BitZeroPanel::Corner || mode == BitZeroPanel::ZOnly
                              || mode == BitZeroPanel::Plate);
        if (xyOff || zOff) {
            ShopAsk ask;
            ask.title = QStringLiteral("BitZero");
            ask.headline = QStringLiteral("This job's zero is not where BitZero sets it.");
            ask.body = QStringLiteral("BitZero sets zero at the stock's lower-left corner, on its top. "
                                      "The open design's zero is the %1. Probe anyway, then set the "
                                      "zero at the point the job uses before you run it.")
                           .arg(jobZeroText(*m_doc));
            ask.okText = QStringLiteral("Probe anyway");
            ask.defaultOk = false;
            if (!shopAsk(this, ask))
                return;
            log(QStringLiteral("BitZero: job zero is the %1, not the probed corner").arg(jobZeroText(*m_doc)));
        }
    }
    stopHoldJog();
    m_bz = m_bzPanel->config();
    m_bzMode = m_bzPanel->mode();
    const bool round = m_bzMode == BitZeroPanel::Hole || m_bzMode == BitZeroPanel::Boss;
    m_bzWantX = m_bzMode == BitZeroPanel::Corner || m_bzMode == BitZeroPanel::XOnly || round;
    m_bzWantY = m_bzMode == BitZeroPanel::Corner || m_bzMode == BitZeroPanel::YOnly || round;
    m_bzWantZ = m_bzMode == BitZeroPanel::Corner || m_bzMode == BitZeroPanel::ZOnly
                || m_bzMode == BitZeroPanel::Plate;
    m_bzPass = 0;
    if (round)
        m_bzPanel->setCentreResult(QString());
    m_bzCx = st.mx;
    m_bzCy = st.my;
    m_bzPanel->setBusy(true);
    log(QStringLiteral("BitZero: probing %1 from machine X%2 Y%3 Z%4")
            .arg(m_bzMode == BitZeroPanel::Corner ? QStringLiteral("the corner (X, Y, Z)")
                 : m_bzMode == BitZeroPanel::ZOnly ? QStringLiteral("Z only")
                 : m_bzMode == BitZeroPanel::XOnly ? QStringLiteral("X only")
                 : m_bzMode == BitZeroPanel::Hole ? QStringLiteral("the centre of a hole")
                 : m_bzMode == BitZeroPanel::Boss ? QStringLiteral("the centre of a boss")
                 : m_bzMode == BitZeroPanel::Plate ? QStringLiteral("Z on a touch plate")
                                                    : QStringLiteral("Y only"))
            .arg(st.mx, 0, 'f', 3).arg(st.my, 0, 'f', 3).arg(st.mz, 0, 'f', 3));
    m_bzStartX = st.mx;
    m_bzStartY = st.my;
    if (round) {
        // A failed boss touch leaves the bit beside the boss, below its
        // top: back up to here before anything goes sideways.
        m_bzOverZ = st.mz;
        bitZeroStage(Phase::BitZeroX);
        return;
    }
    if (m_bzMode == BitZeroPanel::Plate) {
        bitZeroStage(Phase::BitZeroZ);
        return;
    }
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
    if (m_bzMode == BitZeroPanel::Hole || m_bzMode == BitZeroPanel::Boss
        || m_bzMode == BitZeroPanel::Plate) {
        ProbeFeeds f;
        f.fastFeed = m_bz.fastFeed;
        f.slowFeed = m_bz.slowFeed;
        f.retract = m_bz.retract;
        const char axis = stage == Phase::BitZeroY ? 'Y' : 'X';
        const double r = m_bz.holeDiameter / 2.0;
        if (stage == Phase::BitZeroZ)
            lines += plateProbeLines(m_bz.zTravel, f);
        else if (m_bzMode == BitZeroPanel::Hole)
            // Started off-centre the far wall is up to a diameter away.
            lines += holeProbeLines(axis, std::max(r + 5.0, m_bz.holeDiameter), f);
        else
            lines += bossProbeLines(axis, m_bz.holeDiameter, m_bz.bossClearance, m_bz.bossHeight, f);
        m_grbl->startMacro(lines);
        return;
    }
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
    const bool boss = m_bzMode == BitZeroPanel::Boss;
    if (!ok && (m_bz.v1 || boss) && m_bzMode != BitZeroPanel::Hole
        && m_bzMode != BitZeroPanel::Plate
        && (stage == Phase::BitZeroX || stage == Phase::BitZeroY)
        && m_resets == m_bzStageResets) {
        // A V1 side probe stops beside the block, below its top: Begin
        // again from there would search Z downward past the block, into
        // the stock or the spoilboard. Once unlocked, back up over the
        // block and over to where Begin was pressed.
        if (boss)
            log(QStringLiteral("!! Boss centre failed: no contact on its %1 sides. Check the "
                               "ground clip and the diameter, boss height and clearance; the bit "
                               "goes back up to where it started, then Begin again")
                    .arg(stage == Phase::BitZeroX ? QStringLiteral("X") : QStringLiteral("Y")));
        else
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
    if (!ok && m_bzMode == BitZeroPanel::Hole) {
        bitZeroFail(stage == Phase::BitZeroHome ? QStringLiteral("could not move to the hole's centre")
                                                : QStringLiteral("no contact on a hole wall"));
        return;
    }
    if (!ok) {
        bitZeroFail(stage == Phase::BitZeroZ ? QStringLiteral("no contact on the plate")
                    : stage == Phase::BitZeroHome ? QStringLiteral("could not move back over the corner")
                    : m_bz.v1 ? QStringLiteral("the side probe beside the block stopped")
                                                  : QStringLiteral("no contact on a bore wall"));
        return;
    }
    if (m_bzMode == BitZeroPanel::Hole || boss || m_bzMode == BitZeroPanel::Plate) {
        bitZeroAfterCentre(stage);
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

// Hole / boss: X walls, over to their middle, Y walls, over to theirs, X
// once more (the first X chord was off-centre in Y), then X0 Y0 there.
// Touch plate: one Z touch, Z0 the plate thickness below it.
void MachinePanel::bitZeroAfterCentre(Phase stage)
{
    const bool boss = m_bzMode == BitZeroPanel::Boss;
    const QString what = boss ? QStringLiteral("Boss") : QStringLiteral("Hole");
    switch (stage) {
    case Phase::BitZeroX: {
        if (m_hits.size() < 4) {
            bitZeroFail(QStringLiteral("expected four X contacts, got %1").arg(m_hits.size()));
            return;
        }
        m_bzXp = m_hits.at(1).x;
        m_bzXn = m_hits.at(3).x;
        m_bzCx = (m_bzXp + m_bzXn) / 2.0;
        log(QStringLiteral("%1 centre: X sides at %2 and %3 → centre X %4")
                .arg(what).arg(m_bzXp, 0, 'f', 3).arg(m_bzXn, 0, 'f', 3).arg(m_bzCx, 0, 'f', 3));
        const QStringList toCentre = {QStringLiteral("G53 G0 X%1").arg(m_bzCx, 0, 'f', 3)};
        if (m_bzPass == 0) {
            m_bzPass = 1;
            bitZeroStage(Phase::BitZeroY, toCentre);
            return;
        }
        const ProbeCentre c = probeCentre(m_bzXp, m_bzXn, m_bzYp, m_bzYn, m_bz.toolDiameter, boss);
        const QString res = QStringLiteral("%1 diameter %2 mm (X %3, Y %4; tip %5 mm %6)")
                                .arg(what).arg(c.diameter(), 0, 'f', 3)
                                .arg(c.diameterX, 0, 'f', 3).arg(c.diameterY, 0, 'f', 3)
                                .arg(m_bz.toolDiameter, 0, 'f', 3)
                                .arg(boss ? QStringLiteral("taken off") : QStringLiteral("added"));
        log(QStringLiteral("%1 centre at machine X %2 Y %3. %4")
                .arg(what).arg(c.x, 0, 'f', 3).arg(c.y, 0, 'f', 3).arg(res));
        m_bzPanel->setCentreResult(QStringLiteral("Centre set as X0 Y0. ") + res);
        m_phase = Phase::BitZeroHome;
        m_grbl->startMacro({QStringLiteral("G90"),
                            QStringLiteral("G53 G0 X%1 Y%2").arg(m_bzCx, 0, 'f', 3).arg(m_bzCy, 0, 'f', 3)});
        return;
    }
    case Phase::BitZeroY: {
        if (m_hits.size() < 4) {
            bitZeroFail(QStringLiteral("expected four Y contacts, got %1").arg(m_hits.size()));
            return;
        }
        m_bzYp = m_hits.at(1).y;
        m_bzYn = m_hits.at(3).y;
        m_bzCy = (m_bzYp + m_bzYn) / 2.0;
        log(QStringLiteral("%1 centre: Y sides at %2 and %3 → centre Y %4")
                .arg(what).arg(m_bzYp, 0, 'f', 3).arg(m_bzYn, 0, 'f', 3).arg(m_bzCy, 0, 'f', 3));
        m_bzPass = 2;
        bitZeroStage(Phase::BitZeroX, {QStringLiteral("G53 G0 Y%1").arg(m_bzCy, 0, 'f', 3)});
        return;
    }
    case Phase::BitZeroZ: {
        if (m_hits.size() < 2) {
            bitZeroFail(QStringLiteral("expected two Z contacts, got %1").arg(m_hits.size()));
            return;
        }
        const double plate = m_hits.at(1).z;
        m_bzStockZ = plate - m_bz.plateThickness;
        log(QStringLiteral("Touch plate: top at machine Z %1, %2 mm thick → stock top Z %3")
                .arg(plate, 0, 'f', 3).arg(m_bz.plateThickness, 0, 'f', 2).arg(m_bzStockZ, 0, 'f', 3));
        bitZeroApply();
        return;
    }
    default:
        bitZeroApply();
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
    m_toolStretch = m_pendingStretch;
    m_toolStretchSeq = m_docSeq;
    m_toolStretchNc = m_ncPath;
    QStringList first = m_homed ? riseToTop() : QStringList{};
    // After the measurement the spindle stands over the BitSetter, and the
    // program's first line is a retract to ITS safe height in work Z: with
    // the stock top below the button that is a rapid down into the button
    // (2026-10-24, 16.7 mm into it). So, still at the top of Z, go back to
    // where the program parked - or, without a position, to the tool-change
    // spot, which is beside the button and clear by definition.
    if (m_homed) {
        const double x = m_preParkValid ? m_preParkX : m_tcX->value();
        const double y = m_preParkValid ? m_preParkY : m_tcY->value();
        first << QStringLiteral("G53 G0 X%1 Y%2").arg(x, 0, 'f', 3).arg(y, 0, 'f', 3);
        log(QStringLiteral("continuing: up to the top, back over %1 (machine X%2 Y%3), then the program")
                .arg(m_preParkValid ? QStringLiteral("where the program parked")
                                    : QStringLiteral("the tool-change spot (no position report)"))
                .arg(x, 0, 'f', 3).arg(y, 0, 'f', 3));
    }
    if (m_jobView) {
        m_jobView->linesInserted(first.size());
        m_jobView->setParked(false);
    }
    setJobRunning(m_jobView && m_jobView->hasPlan());
    m_grbl->continueAfterToolChange(first);
}

// While a job streams the middle column shows the job view and the
// buttons that are not in use while cutting (zeroing, go to zero, measure,
// probe, Initialize, Park, Unlock, BitSetter) hide: nothing to press by mistake, and the room
// goes to what is happening. A tool-change pause and the end bring them back.
void MachinePanel::setJobRunning(bool on)
{
    if (on == m_jobShown)
        return;
    m_jobShown = on;
    if (m_midStack)
        m_midStack->setCurrentIndex(on ? 1 : 0);
    for (QWidget *w : std::as_const(m_idleOnly))
        w->setVisible(!on);
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
    s.beginGroup(zzeroGroup());
    s.setValue(QStringLiteral("withMap"), m_zZeroWithMap);
    s.setValue(QStringLiteral("sent"), sent);
    s.setValue(QStringLiteral("mx"), mx);
    s.setValue(QStringLiteral("my"), my);
    s.setValue(QStringLiteral("wco"), wcoZ);
}

// The Z-zero record belongs to the controller (its work offset is what it
// describes), not to the machine profile: two profiles on one board share
// it, and a profile used with another board starts without one. Kept under
// the port the controller is on, outside the profile's own settings.
QString MachinePanel::zzeroGroup() const
{
    QString port = m_grbl && m_grbl->isConnected() && !m_connectedPort.isEmpty() ? m_connectedPort
                                                                                  : currentPortName();
    port.replace(QLatin1Char('/'), QLatin1Char('_')).replace(QLatin1Char('\\'), QLatin1Char('_'));
    return QStringLiteral("controllers/%1/zzero").arg(port.isEmpty() ? QStringLiteral("default") : port);
}

void MachinePanel::loadZZero()
{
    QSettings s;
    s.beginGroup(zzeroGroup());
    m_zZeroKnown = s.contains(QStringLiteral("wco"));
    m_zZeroWithMap = s.value(QStringLiteral("withMap")).toBool();
    m_zZeroSent = s.value(QStringLiteral("sent")).toDouble();
    m_zZeroMx = s.value(QStringLiteral("mx")).toDouble();
    m_zZeroMy = s.value(QStringLiteral("my")).toDouble();
    m_zZeroWco = s.value(QStringLiteral("wco")).toDouble();
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
        // Done, it waits 10 cm left of the BitSetter, not back over the corner.
        QSettings sset;
        const auto machine = appsettings::machine(sset);
        const double tx = qMin(machine.width, m_grbl->setting(130, machine.width));
        const double waitX = qMax(bs.x - 100.0, -tx + 5.0), waitY = bs.y;
        // What happens after the zero is set: up, then measure the pin on the
        // BitSetter or wait at the spot. Run at once, or after OK when asked.
        auto moveOn = [this, bs, what, waitX, waitY](QStringList cmds) {
            if (m_bzWantZ && bs.enabled) {
                m_phase = Phase::MeasureForRef;
                m_grbl->startMacro(cmds + riseToTop()
                                   + GrblStreamer::measureToolLines(bs, true, waitX, waitY));
                log(QStringLiteral("BitZero: %1set — measuring the tool now in the spindle on the "
                                   "BitSetter as the reference").arg(what));
                log(QStringLiteral("!! If that was the probing pin: the cutting bit must be measured "
                                   "before it cuts. The job's first tool-change prompt does it — "
                                   "answer OK there, never Ignore — or press Measure bit after "
                                   "changing."));
            } else {
                // Then straight up, out of the way of the plate and the magnet.
                QStringList after = cmds + riseToTop();
                if (m_homed && bs.enabled)
                    after << QStringLiteral("G90")
                          << QStringLiteral("G53 G0 X%1 Y%2").arg(waitX, 0, 'f', 3).arg(waitY, 0, 'f', 3);
                for (const QString &c : after)
                    m_grbl->sendCommand(c);
                if (m_bzWantZ)
                    forgetReference();   // see zero()
                if (m_bzWantZ && m_manualZero)
                    m_awaitToolZero = true;      // m_zeroedForTool on the G10's ok
                log(QStringLiteral("BitZero: %1set. Remove the plate and the magnet before cutting")
                        .arg(what));
            }
        };
        if (QSettings().value(QStringLiteral("machine/bitzero/askBeforeMoving"), false).toBool()) {
            // The zero is set and nothing moves until the plate and magnet are off.
            for (const QString &c : cmds)
                m_grbl->sendCommand(c);
            if (m_bzWantZ && m_manualZero)
                m_awaitToolZero = true;
            log(QStringLiteral("BitZero: %1set — waiting for OK before moving").arg(what));
            const quint64 resets = m_resets;
            whenIdle([this, moveOn, resets] {
                QMessageBox box(QMessageBox::Information, QStringLiteral("BitZero"),
                                QStringLiteral("Remove the magnet and BitZero, then press OK."),
                                QMessageBox::Ok, this);
                box.exec();
                if (resets != m_resets || !m_homed)
                    return;                  // a reset while waiting: the position is gone
                moveOn({});
            });
        } else {
            moveOn(cmds);
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

static QString fileHash(const QString &path)
{
    QFile f(path);
    if (path.isEmpty() || !f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromLatin1(QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha1).toHex());
}

void MachinePanel::setDocument(Document *doc)
{
    m_doc = doc;
    ++m_docSeq;
    m_docHash = doc ? fileHash(doc->filePath()) : QString();
    // A design opened (here or from a pad) is what is run next.
    if (!m_ncPath.isEmpty()) {
        log(QStringLiteral("G-code file %1 put away: a design was opened").arg(QFileInfo(m_ncPath).fileName()));
        m_ncPath.clear();
        m_ncLines.clear();
        m_ncOps.clear();
    }
    refreshJobInfo();
    updateContinueOffer();
}

void MachinePanel::openProgramFile()
{
    if (isBusy() || m_grbl->isParkedForTool()) {
        shopTell(this, QStringLiteral("Machine"), QStringLiteral("The machine is busy."),
                 QStringLiteral("Open a G-code file when the job has ended."));
        return;
    }
    QSettings set;
    const QString dir = set.value(QStringLiteral("machine/ncDir")).toString();
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open G-code"), dir,
        QStringLiteral("G-code (*.nc *.gcode *.tap *.ngc *.NC *.GCODE *.TAP *.NGC);;All files (*)"));
    if (path.isEmpty())
        return;
    set.setValue(QStringLiteral("machine/ncDir"), QFileInfo(path).absolutePath());
    QString why;
    if (!loadProgramFile(path, &why))
        shopTell(this, QStringLiteral("Machine"),
                 QStringLiteral("%1 cannot run here.").arg(QFileInfo(path).fileName()), why);
}

bool MachinePanel::loadProgramFile(const QString &path, QString *why)
{
    auto no = [&](const QString &w) {
        if (why)
            *why = w;
        log(QStringLiteral("!! G-code file %1 refused: %2").arg(QFileInfo(path).fileName(), w));
        return false;
    };
    if (isBusy() || m_grbl->isParkedForTool())
        return no(QStringLiteral("The machine is busy."));
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return no(QStringLiteral("It could not be read (%1).").arg(f.errorString()));
    if (f.size() > 64 * 1024 * 1024)
        return no(QStringLiteral("It is larger than 64 MB."));
    const NcProgram p = readNcProgram(QString::fromUtf8(f.readAll()));
    if (!p.ok())
        return no(p.errorLine > 0 ? QStringLiteral("Line %1: %2").arg(p.errorLine).arg(p.error) : p.error);
    m_ncPath = path;
    m_ncLines = p.lines;
    m_ncOps = p.ops;
    log(QStringLiteral("G-code file loaded: %1 (%2 lines)").arg(QFileInfo(path).fileName()).arg(p.lines.size()));
    refreshJobInfo();
    return true;
}

// Surfacing: a program made here from a few numbers (area, bit, depth),
// then loaded like an opened G-code file, so Start runs it through the same
// checks, job list, Hold/Stop and end park.
void MachinePanel::openSurfacing()
{
    if (isBusy() || m_grbl->isParkedForTool()) {
        shopTell(this, QStringLiteral("Surfacing"), QStringLiteral("The machine is busy."),
                 QStringLiteral("Set up surfacing when the job has ended."));
        return;
    }
    if (!requireHomed(QStringLiteral("Surfacing")))
        return;
    // The travel the bit's centre may use, machine coordinates (X0 right,
    // Y0 back), as for Quick move.
    QSettings st;
    const auto machine = appsettings::machine(st);
    const double tx = qMin(machine.width, m_grbl->setting(130, machine.width));
    const double ty = qMin(machine.height, m_grbl->setting(131, machine.height));
    const double clear = switchClearance();
    const double right = -clear, left = -(tx - 1.0);
    const double back = -clear, front = -(ty - 1.0);
    constexpr double kMargin = 5.0;
    const MachineStatus &ms = m_grbl->status();
    const double wcoX = ms.mx - ms.wx, wcoY = ms.my - ms.wy;

    ToolLibrary lib;
    lib.loadDefault();
    QVector<const LibraryTool *> bits;
    for (const LibraryTool &t : lib.tools())
        if (t.ccType() == 0 && t.diameter > 0)
            bits << &t;
    std::sort(bits.begin(), bits.end(), [](const LibraryTool *a, const LibraryTool *b) {
        return a->diameter > b->diameter;
    });
    int surfBit = -1;
    for (int i = 0; i < bits.size(); ++i) {
        const QString n = (bits.at(i)->name + QLatin1Char(' ') + bits.at(i)->kind).toLower();
        if (n.contains(QLatin1String("surfac")) || n.contains(QLatin1String("fly")) || n.contains(QLatin1String("spoilboard"))) {
            surfBit = i;
            break;
        }
    }

    QDialog dlg(this);
    dlg.setObjectName(QStringLiteral("surfacing"));
    dlg.setWindowTitle(QStringLiteral("Surfacing"));
    auto *lay = new QVBoxLayout(&dlg);
    auto *hint = new QLabel(QStringLiteral("Flattens a rectangle in zigzag lines. Set Z zero on the top "
                                           "of the board first: the cut goes down from there. The bit "
                                           "runs one radius past every edge."), &dlg);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: #c5cad3; font-size: 15px;"));
    lay->addWidget(hint);
    auto *form = new QFormLayout;
    form->setVerticalSpacing(8);
    auto big = [](QWidget *w) { w->setMinimumHeight(44); return w; };

    auto *bitBox = new QComboBox(&dlg);
    bitBox->addItem(QStringLiteral("My own bit (diameter below)"), -1);
    for (int i = 0; i < bits.size(); ++i)
        bitBox->addItem(QStringLiteral("%1 (Ø %2 mm)").arg(bits.at(i)->name).arg(bits.at(i)->diameter), i);
    auto *matBox = new QComboBox(&dlg);
    for (const LibraryMaterial &m : lib.materials())
        matBox->addItem(m.name, m.id);
    const int mdf = matBox->findData(QStringLiteral("mdf"));
    matBox->setCurrentIndex(qMax(0, st.value(QStringLiteral("machine/surf/material")).isValid()
                                        ? matBox->findData(st.value(QStringLiteral("machine/surf/material")))
                                        : mdf));
    auto *dia = mmSpin(&dlg, 1, 100, st.value(QStringLiteral("machine/surf/dia"), 22.0).toDouble(), 2);
    const double defDia = surfBit >= 0 ? bits.at(surfBit)->diameter : dia->value();
    auto *w = mmSpin(&dlg, 1, 5000, std::floor(tx - 1.0 - clear - 2 * kMargin - defDia), 1);
    auto *h = mmSpin(&dlg, 1, 5000, std::floor(ty - 1.0 - clear - 2 * kMargin - defDia), 1);
    w->setToolTip(QStringLiteral("Width along X. The default is the machine's travel, less 5 mm "
                                 "each side and the bit"));
    h->setToolTip(QStringLiteral("Length along Y"));
    auto *corner = new QComboBox(&dlg);
    corner->addItem(QStringLiteral("At the X/Y zero (front-left corner of the area)"));
    corner->addItem(QStringLiteral("Front-left of the machine"));
    corner->setCurrentIndex(st.value(QStringLiteral("machine/surf/corner"), 1).toInt() == 0 ? 0 : 1);
    auto *stepover = mmSpin(&dlg, 5, 95, st.value(QStringLiteral("machine/surf/stepover"), 40.0).toDouble(), 0,
                            QStringLiteral(" %"));
    auto *stepdown = mmSpin(&dlg, 0.05, 20, st.value(QStringLiteral("machine/surf/stepdown"), 0.5).toDouble(), 2);
    auto *depth = mmSpin(&dlg, 0.05, 50, st.value(QStringLiteral("machine/surf/depth"), 0.5).toDouble(), 2);
    auto *feed = mmSpin(&dlg, 10, 20000, 2000, 0, QStringLiteral(" mm/min"));
    auto *plunge = mmSpin(&dlg, 10, 10000, 500, 0, QStringLiteral(" mm/min"));
    auto *rpm = new QSpinBox(&dlg);
    rpm->setRange(1000, 60000);
    rpm->setSingleStep(500);
    rpm->setValue(18000);
    rpm->setSuffix(QStringLiteral(" rpm"));
    auto *dir = new QComboBox(&dlg);
    dir->addItem(QStringLiteral("Lines along X (left - right)"));
    dir->addItem(QStringLiteral("Lines along Y (front - back)"));
    dir->setCurrentIndex(st.value(QStringLiteral("machine/surf/alongY"), false).toBool() ? 1 : 0);
    auto *finish = new QCheckBox(QStringLiteral("Finish pass: climb all round"), &dlg);
    finish->setToolTip(QStringLiteral("One more pass at the full depth round the edge of the area, "
                                      "climb cut, for a clean wall"));
    finish->setChecked(st.value(QStringLiteral("machine/surf/finish"), false).toBool());

    form->addRow(QStringLiteral("Bit"), big(bitBox));
    form->addRow(QStringLiteral("Material (feeds)"), big(matBox));
    form->addRow(QStringLiteral("Bit diameter"), big(dia));
    form->addRow(QStringLiteral("Area X"), big(w));
    form->addRow(QStringLiteral("Area Y"), big(h));
    form->addRow(QStringLiteral("Start corner"), big(corner));
    form->addRow(QStringLiteral("Stepover"), big(stepover));
    form->addRow(QStringLiteral("Depth per pass"), big(stepdown));
    form->addRow(QStringLiteral("Total depth"), big(depth));
    form->addRow(QStringLiteral("Feed"), big(feed));
    form->addRow(QStringLiteral("Plunge"), big(plunge));
    form->addRow(QStringLiteral("Spindle"), big(rpm));
    form->addRow(QStringLiteral("Direction"), big(dir));
    form->addRow(QString(), big(finish));
    lay->addLayout(form);
    auto *preview = new QLabel(&dlg);
    preview->setWordWrap(true);
    preview->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 600;"));
    lay->addWidget(preview);

    // A library bit fills its diameter and, for the material, its feeds.
    auto fromLibrary = [&] {
        const int i = bitBox->currentData().toInt();
        dia->setEnabled(i < 0);
        if (i < 0 || i >= bits.size())
            return;
        const LibraryTool *t = bits.at(i);
        dia->setValue(t->diameter);
        if (const ToolFeeds *f = t->feedsFor(matBox->currentData().toString()); f && f->valid()) {
            feed->setValue(f->feed);
            if (f->plunge > 0)
                plunge->setValue(f->plunge);
            rpm->setValue(int(f->rpm));
        }
    };
    auto params = [&] {
        SurfacingParams p;
        p.toolDia = dia->value();
        p.width = w->value();
        p.height = h->value();
        p.stepoverPct = stepover->value();
        p.stepdown = stepdown->value();
        p.depth = depth->value();
        p.feed = feed->value();
        p.plunge = plunge->value();
        p.rpm = rpm->value();
        p.alongX = dir->currentIndex() == 0;
        p.finishClimb = finish->isChecked();
        p.safeZ = 5;
        if (corner->currentIndex() == 1) {
            // The area's corner one radius (plus a margin) in from the
            // front-left end of the travel, as work coordinates.
            p.x0 = left + kMargin + p.toolDia / 2 - wcoX;
            p.y0 = front + kMargin + p.toolDia / 2 - wcoY;
        }
        return p;
    };
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    bb->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Load as the job"));
    bb->button(QDialogButtonBox::Ok)->setMinimumHeight(52);
    bb->button(QDialogButtonBox::Cancel)->setMinimumHeight(52);
    auto update = [&] {
        const SurfacingPlan plan = planSurfacing(params());
        QString why = plan.error;
        if (why.isEmpty())
            why = surfacingFitProblem(plan, wcoX, wcoY, left, right, front, back);
        bb->button(QDialogButtonBox::Ok)->setEnabled(why.isEmpty());
        if (!why.isEmpty()) {
            preview->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 600; color: #ff6b6b;"));
            preview->setText(why);
            return;
        }
        preview->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 600; color: #e6e9ef;"));
        preview->setText(QStringLiteral("%1, %2 lines × %3 pass%4")
                             .arg(roughDuration(computeStats(plan.ops).timeSec))
                             .arg(plan.lines).arg(plan.passes)
                             .arg(plan.passes == 1 ? QString() : QStringLiteral("es")));
    };
    if (surfBit >= 0)
        bitBox->setCurrentIndex(surfBit + 1);
    fromLibrary();
    update();
    connect(bitBox, &QComboBox::currentIndexChanged, &dlg, [&] { fromLibrary(); update(); });
    connect(matBox, &QComboBox::currentIndexChanged, &dlg, [&] { fromLibrary(); update(); });
    for (QDoubleSpinBox *s : {dia, w, h, stepover, stepdown, depth, feed, plunge})
        connect(s, &QDoubleSpinBox::valueChanged, &dlg, [&] { update(); });
    connect(rpm, &QSpinBox::valueChanged, &dlg, [&] { update(); });
    connect(corner, &QComboBox::currentIndexChanged, &dlg, [&] { update(); });
    connect(dir, &QComboBox::currentIndexChanged, &dlg, [&] { update(); });
    connect(finish, &QCheckBox::toggled, &dlg, [&] { update(); });
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);
    if (dlg.exec() != QDialog::Accepted)
        return;

    st.setValue(QStringLiteral("machine/surf/material"), matBox->currentData());
    st.setValue(QStringLiteral("machine/surf/dia"), dia->value());
    st.setValue(QStringLiteral("machine/surf/corner"), corner->currentIndex());
    st.setValue(QStringLiteral("machine/surf/stepover"), stepover->value());
    st.setValue(QStringLiteral("machine/surf/stepdown"), stepdown->value());
    st.setValue(QStringLiteral("machine/surf/depth"), depth->value());
    st.setValue(QStringLiteral("machine/surf/alongY"), dir->currentIndex() == 1);
    st.setValue(QStringLiteral("machine/surf/finish"), finish->isChecked());

    const SurfacingParams p = params();
    const SurfacingPlan plan = planSurfacing(p);
    // Checked again: the machine may have moved its zero while the dialog was open.
    const MachineStatus &now = m_grbl->status();
    QString why = plan.error;
    if (why.isEmpty())
        why = surfacingFitProblem(plan, now.mx - now.wx, now.my - now.wy, left, right, front, back);
    if (isBusy() || m_grbl->isParkedForTool())
        why = QStringLiteral("The machine is busy.");
    if (!why.isEmpty()) {
        shopTell(this, QStringLiteral("Surfacing"), QStringLiteral("Surfacing cannot run here."), why);
        return;
    }
    const NcProgram nc = readNcProgram(GrblPost().generate(plan.ops));
    if (!nc.ok()) {
        shopTell(this, QStringLiteral("Surfacing"), QStringLiteral("Surfacing cannot run here."), nc.error);
        return;
    }
    m_ncPath = QStringLiteral("Surfacing %1 × %2 mm").arg(p.width).arg(p.height);
    m_ncLines = nc.lines;
    m_ncOps = nc.ops;
    log(QStringLiteral("surfacing loaded: %1 × %2 mm, Ø %3 bit, %4 lines × %5 passes to Z-%6")
            .arg(p.width).arg(p.height).arg(p.toolDia).arg(plan.lines).arg(plan.passes).arg(p.depth));
    if (m_rightStack)
        m_rightStack->setCurrentWidget(m_jobPage);
    refreshJobInfo();
}

void MachinePanel::backToDesign()
{
    if (m_ncPath.isEmpty())
        return;
    log(QStringLiteral("back to the design: %1 put away").arg(QFileInfo(m_ncPath).fileName()));
    m_ncPath.clear();
    m_ncLines.clear();
    m_ncOps.clear();
    refreshJobInfo();
}

// The job card names the design and counts its toolpaths; re-read whenever
// the page comes up, since both change behind it in Design/Toolpaths.
void MachinePanel::refreshJobInfo()
{
    if (!m_jobName)
        return;
    if (m_backToDesignBtn)
        m_backToDesignBtn->setVisible(!m_ncPath.isEmpty());
    if (!m_ncPath.isEmpty()) {
        m_jobName->setText(QStringLiteral("File: %1").arg(QFileInfo(m_ncPath).fileName()));
        const int tools = toolChanges(m_ncOps).size();
        m_jobInfo->setText(QStringLiteral("G-code file, %1 lines%2").arg(m_ncLines.size())
                               .arg(tools > 0 ? QStringLiteral(", %1 tool change%2").arg(tools).arg(tools == 1 ? "" : "s")
                                              : QString()));
        return;
    }
    const QString path = m_doc ? m_doc->filePath() : QString();
    QString name = QFileInfo(path).fileName();
    if (name.startsWith(QLatin1String("untitled-")))
        name = QStringLiteral("Untitled design");
    m_jobName->setText(name.isEmpty() ? QStringLiteral("No design open") : name);
    // The ones a job cuts: switched-off toolpaths are left out of it.
    int n = 0, all = 0;
    if (m_doc)
        for (const Toolpath &t : m_doc->toolpaths()) {
            ++all;
            n += t.json.value(QStringLiteral("enabled")).toBool(true);
        }
    m_jobInfo->setText(all == 0 ? QStringLiteral("No toolpaths yet")
                       : n == 0 ? QStringLiteral("All %1 toolpaths are off").arg(all)
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
    // Locked by an alarm (an end stop) with no program left to stop: GRBL
    // has stopped everything itself and listens only after a reset, which
    // is Initialize's job. A hold, a reset and a rise here only produced
    // refused lines and a log that said Z went up when nothing moved.
    const bool locked = m_grbl->isConnected() && !m_grbl->isStreaming()
        && (m_grbl->needsReset() || m_machineState.startsWith(QLatin1String("Alarm")));
    if (locked) {
        clearJob();
        log(QStringLiteral("job cleared: ready to run %1. The controller is in alarm: press Initialize "
                           "to home it before the next run.").arg(m_jobInfo->text()));
        return;
    }
    if (!active) {
        // Nothing moves (a Stop, a reset or the end already ended it), but
        // the last job is still on screen: Cancel job clears it.
        clearJob();
        log(QStringLiteral("job cleared: ready to run %1").arg(m_jobInfo->text()));
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
    m_hist.cancelled = true;
    clearResumePoint();
    m_runPath.clear();
    holdResetThen([this] {
        clearJob();
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

void MachinePanel::clearJob()
{
    // A job that ends without its stream ending (cancelled from a hold
    // after the last line went out) still gets its history line.
    if (m_hist.active)
        recordJobEnd(m_hist.cancelled ? QStringLiteral("cancelled") : QStringLiteral("stopped"), 0);
    m_endOfJobPending = false;
    m_jobTools.clear();
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    m_progress->setFormat(QStringLiteral("%p%"));
    if (m_jobView)
        m_jobView->clear();
    setJobRunning(false);
    m_runBtn->setText(QStringLiteral("▶ Run"));
    if (m_rightStack)
        m_rightStack->setCurrentWidget(m_jobPage);
    refreshJobInfo();
}

void MachinePanel::holdResetThen(std::function<void()> then, bool evenAfterStop)
{
    m_grbl->pauseStream();               // again: harmless when already held
    // One wait and one reset at a time. A second one started meanwhile
    // (Disconnect confirmed while Cancel job still waits for the hold)
    // cleared m_resetPending under the first when that finished, and then
    // reset the controller again, into whatever the first had gone on to do.
    // Its step joins the wait under way instead.
    m_resetThen.append(ResetStep{std::move(then), evenAfterStop});
    if (m_resetPending)
        return;
    m_resetPending = true;
    // Wait for the hold to finish decelerating (Hold:0), at most 5 s. The
    // first 300 ms are not trusted: the last report may predate the hold.
    auto *t = new QTimer(this);
    t->setInterval(100);
    auto tries = std::make_shared<int>(0);
    // A Stop while waiting has already reset the controller: nothing more
    // goes out after it. (The job finishing meanwhile is no Stop: its last
    // lines went into the held planner, and the reset is still needed.)
    connect(t, &QTimer::timeout, this, [this, t, tries, seq = m_stopSeq] {
        if (seq != m_stopSeq) {
            m_resetPending = false;
            t->stop();
            t->deleteLater();
            const QVector<ResetStep> steps = std::exchange(m_resetThen, {});
            for (const ResetStep &s : steps)
                if (s.evenAfterStop && s.then)
                    s.then();   // no motion in it (a disconnect): still wanted
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
        const QVector<ResetStep> steps = std::exchange(m_resetThen, {});
        for (const ResetStep &s : steps)
            if (s.then)
                s.then();
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
    m_liftHold = false;
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
    m_padXbox = s.value(QStringLiteral("ui/padStyle")).toString() == QLatin1String("xbox");
}

void MachinePanel::editGamepadMapping()
{
    // Pressing a button to find it in the table must not also jog, zero or
    // stop the machine, so the pad is ignored here while the dialog is open.
    stopHoldJog();
    std::fill(std::begin(m_padAxisDir), std::end(m_padAxisDir), 0);
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
    // An Xbox 360 pad has its D-pad on axes 6 and 7; 0 and 1 are its left
    // stick, which jogs as well. The triggers and the right stick (2 to 5)
    // do nothing.
    int slot = number;
    if (m_padXbox && (number == 6 || number == 7)) {
        slot = number - 4;
        number -= 6;
    }
    if (number < 0 || number > 1)
        return;
    // A D-pad reports its rest position as 0 and a direction as full scale;
    // an analogue stick needs the same treatment with a dead zone, so both are
    // reduced to -1, 0 or +1 here. A stick held about half way wobbles round
    // the threshold: it lets go only well below it, or each wobble was a tap
    // and a full jog step.
    const int was = m_padAxisDir[slot];
    const int dir = value > 16000 ? 1 : (value < -16000 ? -1
                  : (was != 0 && value * was > 8000 ? was : 0));
    if (dir == was)
        return;
    // Kept while not connected too: a direction held across a dropped link
    // and let go there, then pressed again, was taken for no change.
    m_padAxisDir[slot] = dir;
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
    } else if ((action == QLatin1String("zeroxy") || action == QLatin1String("zeroz")
                || action == QLatin1String("home") || action.startsWith(QLatin1String("macro")))
               && questionOpen(this)) {
        // What a question on this page says was worked out before it opened
        // (Run and Hover check the travel from the work zero as it was): a
        // pad button bumped meanwhile must not move the zero or home the
        // machine under it. Hold and Stop stay, and so do jogs: the answers
        // wait for the machine to stand still.
        log(QStringLiteral("gamepad: %1 ignored while a question is open on screen — answer it first")
                .arg(action));
    } else if (action == QLatin1String("zeroxy")) {
        zero(QStringLiteral("XY"));
    } else if (action == QLatin1String("zeroz")) {
        zero(QStringLiteral("Z"));
    } else if (action == QLatin1String("home")) {
        initializeMachine();
    } else if (action == QLatin1String("unlock")) {
        unlock();
    } else if (action == QLatin1String("spindle+") || action == QLatin1String("spindle-")) {
        m_grbl->sendRealtime(char(action == QLatin1String("spindle+") ? 0x9A : 0x9B));
    } else if (action == QLatin1String("coolant")) {
        toggleCoolant();
    } else if (action.startsWith(QLatin1String("macro"))) {
        runMacro(action.mid(5).toInt() - 1);
    } else if (action == QLatin1String("rapid")) {
        // 100 → 50 → 25 → 100, from what the controller last reported.
        const int now = m_grbl->status().ovRapid;
        m_grbl->sendRealtime(char(now > 50 ? 0x96 : now > 25 ? 0x97 : 0x95));
        log(QStringLiteral("gamepad: rapid %1 %").arg(now > 50 ? 50 : now > 25 ? 25 : 100));
    } else {
        log(QStringLiteral("gamepad: button %1 is mapped to \"%2\", which is not "
                           "an action").arg(number).arg(action));
    }
}

// ---- command line and macros --------------------------------------------------

bool MachinePanel::eventFilter(QObject *watched, QEvent *event)
{
    // Up and Down in the command line walk its history; they must not reach
    // the page, where they jog Y.
    if (watched == m_mdi && event->type() == QEvent::KeyPress) {
        const int k = static_cast<QKeyEvent *>(event)->key();
        if (k == Qt::Key_Up || k == Qt::Key_Down) {
            if (m_mdiHistory.isEmpty())
                return true;
            if (m_mdiBrowse < 0) {
                if (k == Qt::Key_Down)
                    return true;
                m_mdiDraft = m_mdi->text();
                m_mdiBrowse = m_mdiHistory.size();
            }
            m_mdiBrowse += k == Qt::Key_Up ? -1 : 1;
            if (m_mdiBrowse < 0)
                m_mdiBrowse = 0;
            if (m_mdiBrowse >= m_mdiHistory.size()) {
                m_mdiBrowse = -1;
                m_mdi->setText(m_mdiDraft);
            } else {
                m_mdi->setText(m_mdiHistory.at(m_mdiBrowse));
            }
            return true;
        }
        if (k == Qt::Key_PageUp || k == Qt::Key_PageDown)
            return true;   // nor jog Z
    }
    return QWidget::eventFilter(watched, event);
}

bool MachinePanel::sendTyped(const QString &typed)
{
    using machinemacros::Typed;
    const QString line = typed.trimmed();
    const Typed kind = machinemacros::classify(line);
    if (kind == Typed::Empty)
        return false;
    if (!m_grbl->isConnected()) {
        log(QStringLiteral("!! not connected — %1 not sent").arg(line));
        return false;
    }
    m_mdiBrowse = -1;
    m_mdiHistory = machinemacros::pushHistory(m_mdiHistory, line);
    QSettings().setValue(QStringLiteral("ui/mdiHistory"), m_mdiHistory);
    switch (kind) {
    case Typed::Realtime:
        // Through the page's own Hold / Resume, so its button stays right.
        if (line == QLatin1String("?")) {
            m_grbl->requestStatus();
        } else if (line == QLatin1String("!")) {
            if (!m_held && !m_liftHold)
                m_pauseBtn->click();
        } else if (m_held || m_liftHold) {
            m_pauseBtn->click();
        } else {
            m_grbl->sendRealtime('~');
        }
        return true;
    case Typed::Home:
        initializeMachine();
        return true;
    case Typed::Unlock:
        if (m_grbl->isStreaming()) {
            log(QStringLiteral("!! $X not sent: a job is running"));
            return false;
        }
        unlock();
        return true;
    default:
        break;
    }
    if (m_grbl->isStreaming()) {
        log(QStringLiteral("!! %1 not sent: a job is running — the command line waits "
                           "until it has ended").arg(line));
        return false;
    }
    if (!m_grbl->canSendCommand()) {
        log(QStringLiteral("!! %1 not sent: busy (a macro or probe is running) — try again "
                           "when it is done").arg(line));
        return false;
    }
    if (!line.startsWith(QChar('$')) && jobWindingDown(line))
        return false;
    // A typed move: the next absolute jog starts from a fresh report.
    forgetJogPlan();
    return m_grbl->sendCommand(line);
}

void MachinePanel::refreshMacroButtons()
{
    if (!m_macroRow)
        return;
    QSettings s;
    const QVector<MachineMacro> macros = machinemacros::load(s);
    for (int i = 0; i < m_macroBtns.size(); ++i) {
        QPushButton *b = m_macroBtns.at(i);
        const bool have = i < macros.size();
        b->setVisible(have);
        if (!have)
            continue;
        b->setText(macros.at(i).name);
        const QStringList lines = machinemacros::cleanLines(macros.at(i).lines.join(QChar('\n')));
        b->setToolTip(QStringLiteral("Macro %1 — sends:\n%2").arg(i + 1).arg(lines.join(QChar('\n'))));
    }
}

void MachinePanel::editMacros()
{
    QSettings s;
    MacroEditDialog dlg(machinemacros::load(s), this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    QSettings w;
    machinemacros::save(w, dlg.macros());
    refreshMacroButtons();
    log(QStringLiteral("macros saved (%1)").arg(dlg.macros().size()));
}

bool MachinePanel::runMacro(int index)
{
    QSettings s;
    const QVector<MachineMacro> macros = machinemacros::load(s);
    if (index < 0 || index >= macros.size()) {
        log(QStringLiteral("!! there is no macro %1 — add it with Macros…").arg(index + 1));
        return false;
    }
    const MachineMacro &m = macros.at(index);
    const QStringList lines = machinemacros::cleanLines(m.lines.join(QChar('\n')));
    const QString what = QStringLiteral("Macro \"%1\"").arg(m.name);
    if (lines.isEmpty())
        return false;
    if (!m_grbl->isConnected()) {
        log(QStringLiteral("!! not connected"));
        return false;
    }
    if (m_grbl->isStreaming()) {
        log(QStringLiteral("!! %1 refused: a job is running").arg(what));
        return false;
    }
    if (jobWindingDown(what))
        return false;
    if (!m_grbl->canSendCommand()) {
        log(QStringLiteral("!! busy — cannot run %1 now").arg(what));
        return false;
    }
    if (needsInit())
        return false;
    if (movingStill(what))
        return false;
    log(QStringLiteral("%1: %2 line(s)").arg(what).arg(lines.size()));
    m_grbl->startMacro(lines);
    return true;
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
    refreshMacroButtons();                // this machine's own
    m_loading = true;                     // widget setters must not save back
    QSettings s;
    s.beginGroup(QStringLiteral("machine"));
    // The time estimates plan with this machine's rates and accelerations,
    // as its last $$ said (kept in the profile, so they hold offline).
    {
        MotionLimits m;
        for (int i = 0; i < 3; ++i) {
            m.rate[i] = s.value(QStringLiteral("motion/%1").arg(110 + i), m.rate[i]).toDouble();
            m.accel[i] = s.value(QStringLiteral("motion/%1").arg(120 + i), m.accel[i]).toDouble();
        }
        m.junctionDev = s.value(QStringLiteral("motion/11"), m.junctionDev).toDouble();
        setMotionLimits(m);
    }
    m_bsEnabled->setChecked(s.value(QStringLiteral("bitsetter/enabled"), false).toBool());
    m_bsX->setValue(s.value(QStringLiteral("bitsetter/x"), -20.0).toDouble());
    m_bsY->setValue(s.value(QStringLiteral("bitsetter/y"), -20.0).toDouble());
    m_bsSafeZ->setValue(s.value(QStringLiteral("bitsetter/safeZ"), -5.0).toDouble());
    m_bsFast->setValue(s.value(QStringLiteral("bitsetter/fast"), 600.0).toDouble());
    m_bsSlow->setValue(s.value(QStringLiteral("bitsetter/slow"), 50.0).toDouble());
    m_tcX->setValue(s.value(QStringLiteral("toolchange/x"), -120.0).toDouble());
    m_tcY->setValue(s.value(QStringLiteral("toolchange/y"), -20.0).toDouble());
    m_coolantAuto->setChecked(s.value(QStringLiteral("coolant/withJob"), false).toBool());
    m_coolantCode->setCurrentIndex(s.value(QStringLiteral("coolant/mist"), false).toBool() ? 1 : 0);
    m_haveRef = s.value(QStringLiteral("ref/valid"), false).toBool();
    m_refZ = s.value(QStringLiteral("ref/z"), 0.0).toDouble();
    const QString port = s.value(QStringLiteral("port")).toString();
    if (!port.isEmpty())
        m_ports->setEditText(port);
    loadZZero();
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
    s.setValue(QStringLiteral("coolant/withJob"), m_coolantAuto->isChecked());
    s.setValue(QStringLiteral("coolant/mist"), m_coolantCode->currentIndex() == 1);
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
    countStatus(st);
    m_machineState = st.state;
    ++m_statusSeq;
    // A $$ listing came in: the estimates use the machine's own rates and
    // accelerations from now on, and the profile keeps them for offline.
    if (m_grbl->haveSettings()) {
        const MotionLimits cur = motionLimits();
        MotionLimits m = cur;
        for (int i = 0; i < 3; ++i) {
            m.rate[i] = m_grbl->setting(110 + i, cur.rate[i]);
            m.accel[i] = m_grbl->setting(120 + i, cur.accel[i]);
        }
        m.junctionDev = m_grbl->setting(11, cur.junctionDev);
        if (m != cur) {
            setMotionLimits(m);
            QSettings s;
            for (int i = 0; i < 3; ++i) {
                s.setValue(QStringLiteral("machine/motion/%1").arg(110 + i), m.rate[i]);
                s.setValue(QStringLiteral("machine/motion/%1").arg(120 + i), m.accel[i]);
            }
            s.setValue(QStringLiteral("machine/motion/11"), m.junctionDev);
        }
    }
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
        if (m_zeroedUnhomed) {
            m_zeroedUnhomed = false;
            log(QStringLiteral("!! the X/Y zero was set BEFORE this homing: it points somewhere "
                               "else now - jog to the stock corner and zero X/Y again"));
            shopTell(this, QStringLiteral("Machine"), QStringLiteral("Zero X/Y again"),
                     QStringLiteral("The X/Y zero was set before this homing. Homing redefined the "
                                    "machine coordinates, so that zero now points somewhere else.\n\n"
                                    "Jog to the stock corner and zero X/Y again before running a job."));
        }
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
    // An override off 100 % is worth seeing without unfolding "More".
    if (st.ovFeed != 100)
        extra += QStringLiteral("   FEED %1 %").arg(st.ovFeed);
    m_extra->setText(extra);
    if (m_jobView) {
        m_jobView->setOverride(st.ovFeed);
        m_jobView->setStartHereAllowed(m_homed && m_grbl->isConnected() && !m_grbl->isStreaming()
                                       && !isBusy() && !m_held && m_machineState == QLatin1String("Idle"));
    }
    updateContinueOffer();
    if ((st.flood || st.mist) != m_coolantOn)
        showCoolant(st.flood || st.mist);
    if (m_ovLabel) {
        QString ov = QStringLiteral("feed %1 %").arg(st.ovFeed);
        if (st.ovRapid != 100)
            ov += QStringLiteral("   rapid %1 %").arg(st.ovRapid);
        if (st.ovSpindle != 100)
            ov += QStringLiteral("   spindle %1 %").arg(st.ovSpindle);
        m_ovLabel->setText(ov);
        m_ovLabel->setStyleSheet(st.ovFeed == 100 && st.ovRapid == 100 && st.ovSpindle == 100
            ? QStringLiteral("font-size: 16px; font-weight: 600; padding-left: 8px;")
            : QStringLiteral("font-size: 16px; font-weight: 600; padding-left: 8px; color: #f0a830;"));
    }
    emit livePosition(st.wx, st.wy, st.wz, true);
    updateSpindleButtons();

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

// ---- Spindle by hand ------------------------------------------------------

double MachinePanel::spindleMaxRpm() const
{
    const double fromController = m_grbl->haveSettings() ? m_grbl->setting(30, 0) : 0;
    if (fromController > 0)
        return fromController;
    const double own = QSettings().value(QStringLiteral("machine/spindleMaxRpm"), 10000).toDouble();
    return own > 0 ? own : 10000;
}

void MachinePanel::updateSpindleButtons()
{
    if (!m_rpmSpin)
        return;
    if (!m_rpmRangeSet && m_grbl->haveSettings()) {
        m_rpmRangeSet = true;
        const int hi = int(spindleMaxRpm());
        const int lo = qBound(0, int(m_grbl->setting(31, 0)), hi);
        const bool atDefault = m_rpmSpin->value() == 10000
                               || m_rpmSpin->value() > hi || m_rpmSpin->value() < lo;
        m_rpmSpin->setRange(lo, hi);
        if (atDefault)
            m_rpmSpin->setValue(hi);
    }
    const bool connected = m_grbl->isConnected();
    const bool idle = connected && m_homed && m_machineState == QLatin1String("Idle")
                      && m_grbl->canSendCommand() && !m_grbl->isStreaming() && !isBusy();
    const bool warming = m_warmStep >= 0;
    m_spinOnBtn->setEnabled(idle && !warming);
    m_spinOffBtn->setEnabled(connected && m_grbl->canSendCommand() && !m_grbl->isStreaming());
    m_warmBtn->setEnabled(warming || idle);
    if (warming) {
        static const int share[] = {50, 75, 100};
        m_warmBtn->setText(QStringLiteral("Stop %1:%2 (%3 %)")
                               .arg(m_warmLeft / 60)
                               .arg(m_warmLeft % 60, 2, 10, QLatin1Char('0'))
                               .arg(share[qBound(0, m_warmStep, 2)]));
    } else {
        m_warmBtn->setText(QStringLiteral("Warm-up"));
    }
}

// Z near the top (homed), or the operator says the bit is clear of the work.
bool MachinePanel::spindleClear()
{
    const MachineStatus &st = m_grbl->status();
    if (m_homed && st.valid && st.mz >= -switchClearance() - 5.0)
        return true;
    ShopAsk a;
    a.title = QStringLiteral("Spindle");
    a.headline = QStringLiteral("Spindle on: is the bit clear of the work?");
    a.body = QStringLiteral("Z is not up at the top. Check that the bit touches nothing, "
                            "the collet is tight and nothing loose lies under it.");
    a.okText = QStringLiteral("Spin up");
    a.cancelText = QStringLiteral("Not yet");
    a.defaultOk = false;
    return shopAsk(this, a);
}

void MachinePanel::spindleOn()
{
    if (!m_spinOnBtn->isEnabled() || m_grbl->isStreaming() || isBusy()) {
        log(QStringLiteral("spindle on refused: only when the machine stands still and no job runs"));
        return;
    }
    if (!spindleClear() || !m_spinOnBtn->isEnabled())
        return;
    const int rpm = m_rpmSpin->value();
    if (m_grbl->sendCommand(QStringLiteral("M3 S%1").arg(rpm)))
        log(QStringLiteral("spindle on at %1 rpm").arg(rpm));
}

void MachinePanel::spindleOff()
{
    endWarmup(QString());
    if (m_grbl->sendCommand(QStringLiteral("M5")))
        log(QStringLiteral("spindle off"));
    updateSpindleButtons();
}

void MachinePanel::endWarmup(const QString &why)
{
    if (m_warmStep < 0)
        return;
    m_warmTimer->stop();
    m_warmStep = -1;
    if (!why.isEmpty())
        log(why);
    updateSpindleButtons();
}

void MachinePanel::toggleWarmup()
{
    if (m_warmStep >= 0) {
        spindleOff();
        log(QStringLiteral("warm-up stopped"));
        return;
    }
    if (!m_warmBtn->isEnabled() || m_grbl->isStreaming() || isBusy()) {
        log(QStringLiteral("warm-up refused: only when the machine stands still and no job runs"));
        return;
    }
    if (!spindleClear() || !m_warmBtn->isEnabled())
        return;
    m_warmSecs.clear();
    const QStringList secs = QSettings().value(QStringLiteral("machine/warmupSeconds"),
                                               QStringList{QStringLiteral("120"), QStringLiteral("120"),
                                                           QStringLiteral("60")}).toStringList();
    for (int i = 0; i < 3; ++i)
        m_warmSecs << qMax(1, i < secs.size() ? secs.at(i).trimmed().toInt() : 60);
    log(QStringLiteral("warm-up: %1 s at 50 %, %2 s at 75 %, %3 s at full speed, then off")
            .arg(m_warmSecs.at(0)).arg(m_warmSecs.at(1)).arg(m_warmSecs.at(2)));
    m_warmStep = -1;                     // warmupStep() moves on to step 0
    warmupStep();
}

void MachinePanel::warmupStep()
{
    static const double share[] = {0.5, 0.75, 1.0};
    // A job took over (it streams its own M3 and may be parked at a tool
    // change, where commands still go through): leave the spindle to it.
    if (m_grbl->isStreaming()) {
        endWarmup(QStringLiteral("warm-up ended: a job is running"));
        return;
    }
    const int next = m_warmStep + 1;
    if (next >= 3) {
        m_warmTimer->stop();
        m_warmStep = -1;
        if (m_grbl->sendCommand(QStringLiteral("M5")))
            log(QStringLiteral("warm-up done: spindle off"));
        updateSpindleButtons();
        return;
    }
    const int rpm = int(std::lround(spindleMaxRpm() * share[next]));
    if (!m_grbl->sendCommand(QStringLiteral("M3 S%1").arg(rpm))) {
        endWarmup(QStringLiteral("warm-up ended: the controller would not take the next speed"));
        return;
    }
    log(QStringLiteral("warm-up: %1 rpm for %2 s").arg(rpm).arg(m_warmSecs.at(next)));
    m_warmStep = next;
    m_warmLeft = m_warmSecs.at(next);
    m_warmTimer->start();
    updateSpindleButtons();
}

QString MachinePanel::settingsRefusal() const
{
    if (!m_grbl->isConnected())
        return QStringLiteral("Connect to the controller first.");
    if (isBusy() || m_grbl->isStreaming())
        return QStringLiteral("The machine is busy. Wait until the job or move is over.");
    // GRBL takes $ settings in Idle and Alarm (a wrong homing setting is
    // fixed while it is locked), never while moving or held.
    if (m_machineState != QLatin1String("Idle") && !m_machineState.startsWith(QLatin1String("Alarm")))
        return QStringLiteral("The controller is not idle (%1).").arg(m_machineState);
    return QString();
}

static QMap<int, double> toMap(const QHash<int, double> &h)
{
    QMap<int, double> m;
    for (auto it = h.cbegin(); it != h.cend(); ++it)
        m.insert(it.key(), it.value());
    return m;
}

void MachinePanel::autoBackupSettings()
{
    QSettings ps;
    const QString profile = machineprofiles::nameOf(ps, machineprofiles::activeId(ps));
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                        + QStringLiteral("/grbl-backups");
    const QString path = grblsettings::autoBackup(dir, profile, QDate::currentDate(),
                                                  m_grbl->banner(), toMap(m_grbl->settings()));
    if (!path.isEmpty())
        log(QStringLiteral("controller settings backed up to %1").arg(QDir::toNativeSeparators(path)));
}

void MachinePanel::openControllerSettings()
{
    const QString why = settingsRefusal();
    if (!why.isEmpty()) {
        shopTell(this, QStringLiteral("Controller settings"),
                 QStringLiteral("Not now"), why);
        return;
    }
    if (!m_grbl->haveSettings()) {
        m_settingsAsked.start();
        m_grbl->sendCommand(QStringLiteral("$$"));
        shopTell(this, QStringLiteral("Controller settings"),
                 QStringLiteral("Reading the settings"),
                 QStringLiteral("The controller has not listed its settings yet. Try again in a moment."));
        return;
    }
    GrblSettingsDialog dlg(toMap(m_grbl->settings()), m_grbl->banner(), this);
    connect(&dlg, &GrblSettingsDialog::sendRequested, this, [this, &dlg](const QStringList &lines) {
        const QString no = settingsRefusal();
        if (!no.isEmpty()) {
            shopTell(&dlg, QStringLiteral("Controller settings"), QStringLiteral("Not sent"), no);
            return;
        }
        for (const QString &l : lines)
            log(QStringLiteral("setting %1").arg(l));
        dlg.setBusy(true, QStringLiteral("Sending %1 setting%2 and reading them back…")
                              .arg(lines.size()).arg(lines.size() == 1 ? "" : "s"));
        auto conn = std::make_shared<QMetaObject::Connection>();
        *conn = connect(m_grbl, &GrblStreamer::macroFinished, &dlg, [this, &dlg, conn](bool ok) {
            disconnect(*conn);
            dlg.setBusy(false);
            dlg.setSettings(toMap(m_grbl->settings()));
            if (!ok)
                shopTell(&dlg, QStringLiteral("Controller settings"),
                         QStringLiteral("The controller refused a setting"),
                         QStringLiteral("The ones before it were stored; the list shows what the "
                                        "controller has now. See the log for its error."));
        });
        m_grbl->startMacro(lines + QStringList{QStringLiteral("$$")});
    });
    dlg.exec();
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

// Up to safe Z, then at rapid speed to the front-right corner (parkSpot),
// where the collet is in reach for changing bits.
// The XY box of everything the job cuts (feeds and arcs, arcs by their own
// extremes), in work coordinates. False when it cuts nothing.
static bool cutBounds(const QVector<Op> &ops, double &x0, double &y0, double &x1, double &y1)
{
    x0 = y0 = 1e300;
    x1 = y1 = -1e300;
    auto grow = [&](double x, double y) {
        x0 = qMin(x0, x); x1 = qMax(x1, x);
        y0 = qMin(y0, y); y1 = qMax(y1, y);
    };
    double px = 0, py = 0;
    for (const Op &o : ops) {
        if (o.kind == Op::Rapid) {
            px = o.x; py = o.y;
            continue;
        }
        if (o.kind != Op::Feed && o.kind != Op::Arc)
            continue;
        grow(px, py);
        grow(o.x, o.y);
        if (o.kind == Op::Arc) {
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
                    grow(cx + r * std::cos(a), cy + r * std::sin(a));
            }
        }
        px = o.x; py = o.y;
    }
    return x0 <= x1;
}

// Hover dry run: before cutting, trace the outer rectangle of the job 50 mm
// above the work zero (spindle off, nothing touched) to check that the stock
// and the zero are where they should be. Ends hovering over the first corner.
void MachinePanel::hoverDryRun()
{
    constexpr double kHover = 50.0, kFeed = 3000.0;
    if (jobWindingDown(QStringLiteral("Hover dry run")))
        return;
    if (!m_grbl->isConnected()) {
        shopTell(this, QStringLiteral("Machine"), QStringLiteral("Connect to the controller first."));
        return;
    }
    if (!m_grbl->canSendCommand()) {
        log(QStringLiteral("!! busy — cannot hover now"));
        return;
    }
    if (needsInit() || !requireHomed(QStringLiteral("Hover dry run")) || movingStill(QStringLiteral("Hover dry run")))
        return;
    if (!m_doc) {
        shopTell(this, QStringLiteral("Machine"), QStringLiteral("Open a .c2d file first."));
        return;
    }
    // The outline traced is that of the design as it is now. Preparing it
    // and asking both run the event loop: a pad can send a new version of
    // the open design (or another one) meanwhile, and the rectangle asked
    // about would no longer be the job's.
    const quint64 docAtStart = m_docSeq;
    auto designSwapped = [&]() -> bool {
        if (m_docSeq == docAtStart)
            return false;
        shopTell(this, QStringLiteral("Machine"), QStringLiteral("The design changed meanwhile."),
                 QStringLiteral("Press Hover 5 cm again to trace the one that is open now."));
        return true;
    };
    const GcodeResult r = exportWithProgress(this, *m_doc, QStringLiteral("Preparing program"));
    if (r.cancelled || designSwapped())
        return;
    double x0, y0, x1, y1;
    QVector<Op> program = r.ops;   // as posted: from the job zero
    shiftOps(program, jobZeroShift(*m_doc));
    // 5 cm above the stock top, wherever Z zero is (top or spoilboard).
    const double hoverZ = kHover + double(jobZeroShift(*m_doc).z());
    if (r.done.isEmpty() || !cutBounds(program, x0, y0, x1, y1)) {
        shopTell(this, QStringLiteral("Machine"), QStringLiteral("The job has nothing to trace."));
        return;
    }
    if (!m_grbl->status().valid) {
        shopTell(this, QStringLiteral("Machine"), QStringLiteral("No position from the controller yet."));
        return;
    }
    // Inside the travel from the current work zero, like the program check.
    // Run again after the dialog: the pad or a zero button may have moved things.
    auto travelOk = [&]() -> bool {
        const auto &st = m_grbl->status();
        const double off[3] = {st.mx - st.wx, st.my - st.wy, st.mz - st.wz};
        QSettings sset;
        const auto machine = appsettings::machine(sset);
        const double travel[2] = {m_grbl->setting(130, machine.width), m_grbl->setting(131, machine.height)};
        const double top = -switchClearance();
        const double hi[2] = {x1, y1}, lo[2] = {x0, y0};
        QStringList over;
        for (int k = 0; k < 2; ++k) {
            const QChar ax = QChar::fromLatin1("XY"[k]);
            if (hi[k] + off[k] > top + 1e-6)
                over << QStringLiteral("%1 %2 mm past its home switch").arg(ax).arg(hi[k] + off[k] - top, 0, 'f', 1);
            if (lo[k] + off[k] < -travel[k] - 1e-6)
                over << QStringLiteral("%1 %2 mm past the far end of its travel").arg(ax).arg(-travel[k] - (lo[k] + off[k]), 0, 'f', 1);
        }
        if (hoverZ + off[2] > top + 1e-6)
            over << QStringLiteral("Z %1 mm above the top of its travel").arg(hoverZ + off[2] - top, 0, 'f', 1);
        if (!over.isEmpty()) {
            shopTell(this, QStringLiteral("Machine"), QStringLiteral("The outline goes off the machine."),
                     QStringLiteral("From the current work zero it reaches %1. Set the zero again or move the design.")
                         .arg(over.join(QStringLiteral(", "))));
            return false;
        }
        return true;
    };
    if (!travelOk())
        return;
    ShopAsk ask;
    ask.title = QStringLiteral("Hover dry run");
    ask.headline = QStringLiteral("Trace the job outline 50 mm above the work zero?");
    ask.body = QStringLiteral("X %1 to %2, Y %3 to %4 (%5 × %6 mm), at Z +50 mm, feed %7 mm/min. "
                              "Spindle stays off. It ends hovering over the first corner.")
                   .arg(x0, 0, 'f', 1).arg(x1, 0, 'f', 1).arg(y0, 0, 'f', 1).arg(y1, 0, 'f', 1)
                   .arg(x1 - x0, 0, 'f', 1).arg(y1 - y0, 0, 'f', 1).arg(kFeed, 0, 'f', 0);
    ask.note = QStringLiteral("Nothing is cut: the bit stays 5 cm up.");
    ask.okText = QStringLiteral("▶ Trace");
    ask.defaultOk = false;
    if (!shopAsk(this, ask) || designSwapped())
        return;
    // The dialog was modal-ish: state and zero can have changed meanwhile.
    if (!m_grbl->isConnected() || !m_grbl->canSendCommand() || !m_grbl->status().valid
        || needsInit() || !requireHomed(QStringLiteral("Hover dry run")) || movingStill(QStringLiteral("Hover dry run"))
        || !travelOk())
        return;
    QStringList lines = riseToTop();
    lines << QStringLiteral("G21") << QStringLiteral("G90")
          << QStringLiteral("G0 X%1 Y%2").arg(x0, 0, 'f', 3).arg(y0, 0, 'f', 3)
          << QStringLiteral("G0 Z%1").arg(hoverZ, 0, 'f', 3)
          << QStringLiteral("G1 F%1 X%2 Y%3").arg(kFeed, 0, 'f', 0).arg(x1, 0, 'f', 3).arg(y0, 0, 'f', 3)
          << QStringLiteral("G1 X%1 Y%2").arg(x1, 0, 'f', 3).arg(y1, 0, 'f', 3)
          << QStringLiteral("G1 X%1 Y%2").arg(x0, 0, 'f', 3).arg(y1, 0, 'f', 3)
          << QStringLiteral("G1 X%1 Y%2").arg(x0, 0, 'f', 3).arg(y0, 0, 'f', 3);
    log(QStringLiteral("hover dry run: X %1..%2 Y %3..%4 at Z +%5")
            .arg(x0, 0, 'f', 1).arg(x1, 0, 'f', 1).arg(y0, 0, 'f', 1).arg(y1, 0, 'f', 1).arg(kHover, 0, 'f', 0));
    m_grbl->startMacro(lines);
}


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
    double x = 0, y = 0;
    parkSpot(x, y);
    const double z = riseZ();
    log(QStringLiteral("parking front right (X%1 Y%2, machine)").arg(x, 0, 'f', 1).arg(y, 0, 'f', 1));
    m_grbl->startMacro({QStringLiteral("G90"),
                        QStringLiteral("G53 G0 Z%1").arg(z, 0, 'f', 3),
                        QStringLiteral("G53 G0 X%1 Y%2").arg(x, 0, 'f', 3).arg(y, 0, 'f', 3)});
}

// Where Park goes, in machine coordinates. Machine X0 is the RIGHT end and
// Y0 the BACK on this Shapeoko, so front right is X just off its switch and
// Y the full cutting height of the machine picked in Settings forward, less a
// margin so the front end stop is never touched.
void MachinePanel::parkSpot(double &x, double &y) const
{
    QSettings st;
    const auto machine = appsettings::machine(st);
    constexpr double kMargin = 5.0;
    // Inside the travel the controller reports ($130-$132) when it is
    // smaller than the Settings size (which defaults to a 4x4): soft limits
    // are off, so a target past the end is a rapid into the hard stop.
    const double tx = qMin(machine.width, m_grbl->setting(130, machine.width));
    const double ty = qMin(machine.height, m_grbl->setting(131, machine.height));
    const double clear = switchClearance();
    x = -qMin(qMax(kMargin, clear), tx / 2);
    y = -qMax(clear, ty - kMargin);
    // The front right corner is where the BitSetter sits (2026-10-25: parked
    // 2.5 mm from its button, and the next move down went into it): park
    // beside it instead.
    if (m_bsEnabled->isChecked() && std::hypot(x - m_bsX->value(), y - m_bsY->value()) < 40.0)
        x = qMax(m_bsX->value() - 60.0, -tx + kMargin);
}

// The end of a finished job: up to the top of Z, across to the park spot at
// that height, then the spindle off. M5 goes last: on phobic-cnc the limit
// inputs have answered a spindle stop with ALARM:1 (2026-10-26), and an alarm
// then costs nothing, the machine already stands parked. Not homed, machine
// coordinates say nothing about where the park spot is: only up (never down,
// riseZ) and the spindle off, as before.
bool MachinePanel::coolantWithJob() const
{
    return m_coolantAuto && m_coolantAuto->isChecked();
}

bool MachinePanel::coolantMist() const
{
    return m_coolantCode && m_coolantCode->currentIndex() == 1;
}

// The button shows what the controller reports, not what was last pressed.
void MachinePanel::showCoolant(bool on)
{
    m_coolantOn = on;
    if (!m_coolantBtn)
        return;
    m_coolantBtn->setChecked(on);
    m_coolantBtn->setText(on ? QStringLiteral("Dust/Coolant ON") : QStringLiteral("Dust/Coolant off"));
}

// By hand (button or gamepad), when the machine takes a command: M8 (or M7)
// on, M9 off. During a job the program's own lines switch it.
void MachinePanel::toggleCoolant()
{
    if (!m_grbl->isConnected() || !m_grbl->canSendCommand()) {
        log(QStringLiteral("!! busy — dust/coolant not switched"));
        showCoolant(m_coolantOn);   // undo the button's own check
        return;
    }
    const QString code = m_coolantOn ? QStringLiteral("M9")
                         : coolantMist() ? QStringLiteral("M7") : QStringLiteral("M8");
    if (m_grbl->sendCommand(code)) {
        log(QStringLiteral("dust/coolant %1 (%2)").arg(m_coolantOn ? QStringLiteral("off")
                                                                   : QStringLiteral("on"), code));
        showCoolant(!m_coolantOn);  // until the next report says otherwise
    } else {
        showCoolant(m_coolantOn);
    }
}

QStringList MachinePanel::endOfJobMoves()
{
    if (!m_homed) {
        log(QStringLiteral("job done: not homed, so up to safe Z and spindle off, no park"));
        QStringList up{QStringLiteral("G90"), QStringLiteral("G53 G0 Z%1").arg(riseZ(), 0, 'f', 3)};
        if (coolantWithJob())
            up << QStringLiteral("M9");
        up << QStringLiteral("M5");
        return up;
    }
    double x = 0, y = 0;
    parkSpot(x, y);
    QStringList moves{QStringLiteral("G90"),
                      QStringLiteral("G53 G0 Z%1").arg(-switchClearance(), 0, 'f', 3)};
    if (insideTravel(x, y)) {
        log(QStringLiteral("job done: up to the top, over to the park spot (X%1 Y%2, machine), "
                           "then spindle off").arg(x, 0, 'f', 1).arg(y, 0, 'f', 1));
        moves << QStringLiteral("G53 G0 X%1 Y%2").arg(x, 0, 'f', 3).arg(y, 0, 'f', 3);
    } else {
        log(QStringLiteral("job done: up to the top, then spindle off (the park spot is off the machine)"));
    }
    if (coolantWithJob())
        moves << QStringLiteral("M9");
    moves << QStringLiteral("M5");
    return moves;
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
    // Homing redefines the machine coordinates, and G54 is stored in them:
    // an X/Y zero set now points somewhere else once the machine is homed
    // (2026-10-24: zeroed, then Initialize, then "go to zero" went elsewhere).
    if ((axes.contains(QChar('X')) || axes.contains(QChar('Y'))) && !m_homed) {
        ShopAsk a;
        a.title = QStringLiteral("Machine");
        a.headline = QStringLiteral("X/Y zero before Initialize");
        a.body = QStringLiteral("The machine is not homed (%1).\n\nInitialize (homing) redefines "
                                "the machine coordinates, so a zero set now points somewhere else "
                                "after homing. Press Initialize first, then jog to the stock corner "
                                "and zero X/Y.").arg(m_unhomedWhy);
        a.note = QStringLiteral("Zero anyway only if you will not home before this job.");
        a.noteDanger = true;
        a.okText = QStringLiteral("Zero anyway");
        a.okDanger = true;
        a.defaultOk = false;
        if (!shopAsk(this, a))
            return;
        m_zeroedUnhomed = true;
        log(QStringLiteral("!! X/Y zeroed while not homed: zero X/Y again after any Initialize"));
    }
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

// ---- Quick move -------------------------------------------------------------

// Machine X0 is the RIGHT end and Y0 the BACK (homed towards + on all axes).
// The outer ring sits where the jog limits stop: just off the switches, and
// 1 mm short of the far end stops. The travel is the smaller of the
// controller's ($130/$131) and the machine picked in Settings, as for Park.
void MachinePanel::quickMoveSpot(int col, int row, double &x, double &y) const
{
    QSettings st;
    const auto machine = appsettings::machine(st);
    const double tx = qMin(machine.width, m_grbl->setting(130, machine.width));
    const double ty = qMin(machine.height, m_grbl->setting(131, machine.height));
    constexpr double kEdge = 1.0;
    const double clear = switchClearance();
    const double right = -clear, left = -(tx - kEdge);
    const double back = -clear, front = -(ty - kEdge);
    col = qBound(0, col, 4);
    row = qBound(0, row, 4);
    x = left + (right - left) * col / 4.0;
    y = back + (front - back) * row / 4.0;
}

void MachinePanel::openQuickMove()
{
    if (jobWindingDown(QStringLiteral("Quick move")))
        return;
    if (needsInit())
        return;
    if (!requireHomed(QStringLiteral("Quick move")))
        return;

    // A plain dialog: the app-wide popup look gives it the amber card, the
    // caption and the big X like every other popup.
    QDialog dlg(this);
    dlg.setObjectName(QStringLiteral("quickMove"));
    dlg.setWindowTitle(QStringLiteral("Quick move"));
    auto *lay = new QVBoxLayout(&dlg);
    auto *hint = new QLabel(QStringLiteral("Back of the machine at the top. The head goes "
                                           "all the way up first, then straight to the spot."),
                            &dlg);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: #c5cad3; font-size: 15px;"));
    lay->addWidget(hint);
    QPoint pick(-1, -1);
    auto *grid = new QGridLayout;
    grid->setSpacing(8);
    static const char *const rows[] = {"Back", "", "Middle", "", "Front"};
    static const char *const cols[] = {"left", "", "centre", "", "right"};
    for (int r = 0; r < 5; ++r)
        for (int c = 0; c < 5; ++c) {
            QString name;
            if (r == 2 && c == 2)
                name = QStringLiteral("Centre");
            else if (*rows[r] && *cols[c])
                name = QStringLiteral("%1 %2").arg(QLatin1String(rows[r]), QLatin1String(cols[c]));
            double x = 0, y = 0;
            quickMoveSpot(c, r, x, y);
            auto *b = new QPushButton(name.isEmpty() ? QStringLiteral("·") : name, &dlg);
            b->setObjectName(QStringLiteral("quickSpot"));
            b->setProperty("col", c);
            b->setProperty("row", r);
            b->setToolTip(QStringLiteral("X%1 Y%2 (machine)").arg(x, 0, 'f', 1).arg(y, 0, 'f', 1));
            b->setMinimumSize(150, 80);
            b->setFocusPolicy(Qt::NoFocus);
            const bool outer = r == 0 || r == 4 || c == 0 || c == 4;
            b->setStyleSheet(QStringLiteral(
                "QPushButton { font-size: 16px; border-radius: 12px; color: #e6e9ef; "
                "  background: %1; border: 1px solid #3a404b; }"
                "QPushButton:pressed { background: #f2a536; color: #1a1a1a; }")
                                 .arg(outer ? QStringLiteral("#2a2e36") : QStringLiteral("#232730")));
            connect(b, &QPushButton::clicked, &dlg, [&dlg, &pick, c, r] {
                pick = QPoint(c, r);
                dlg.accept();
            });
            grid->addWidget(b, r, c);
        }
    lay->addLayout(grid);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);
    if (dlg.exec() == QDialog::Accepted && pick.x() >= 0)
        quickMoveTo(pick.x(), pick.y());
}

void MachinePanel::quickMoveTo(int col, int row)
{
    if (jobWindingDown(QStringLiteral("Quick move")))
        return;
    if (!m_grbl->canSendCommand()) {
        log(QStringLiteral("!! busy — cannot move now"));
        return;
    }
    if (needsInit())
        return;
    if (!requireHomed(QStringLiteral("Quick move")))
        return;
    if (movingStill(QStringLiteral("Quick move")))
        return;
    double x = 0, y = 0;
    quickMoveSpot(col, row, x, y);
    // Up means the top of Z: just under the switch, out of its trigger zone.
    const double top = -switchClearance();
    QStringList macro{QStringLiteral("G90")};
    if (m_grbl->status().mz < top - 0.05)
        macro << QStringLiteral("G53 G0 Z%1").arg(top, 0, 'f', 3);
    macro << QStringLiteral("G53 G0 X%1 Y%2").arg(x, 0, 'f', 3).arg(y, 0, 'f', 3);
    log(QStringLiteral("quick move to X%1 Y%2 (machine)").arg(x, 0, 'f', 1).arg(y, 0, 'f', 1));
    m_grbl->startMacro(macro);
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
    // The bit comes out now: which one is in is known again on Continue.
    m_pendingStretch = m_rehearsal ? -1 : m_toolOrdBase + ordinal + 1;
    m_toolStretch = -1;
    m_toolMeasured = false;
    m_manualZero = false;
    m_zeroedForTool = false;
    m_awaitToolZero = false;
    m_toolPromptAnswered = false;
    m_runBtn->setText(QStringLiteral("▶ Continue"));
    // Parked: the operator may need to jog and zero, so the controls come
    // back; the job view's clock waits.
    if (m_jobView)
        m_jobView->setParked(true);
    setJobRunning(false);
    if (!m_rehearsal && m_hist.active)
        ++m_hist.toolChanges;
    jobhistory::chime(this);
    // Remember where the program stands: the resume returns here before
    // the program moves again (see continueAfterToolChange()).
    const MachineStatus st = m_grbl->status();
    m_preParkValid = st.valid && m_homed;
    m_preParkX = st.mx;
    m_preParkY = st.my;
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
    if (!m_rehearsal && m_doc && !m_jobFromFile) {
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
    m_toolOrdBase = 0;
    // Not the last job's: its list followed the rehearsal's progress and
    // came back as a running job at the rehearsal's tool change.
    if (m_jobView)
        m_jobView->clear();
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

bool MachinePanel::runProgram(QString *remoteWhy, const StartFrom *from)
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
    // A loaded G-code file runs instead of the design, through all the
    // same checks from here on.
    const bool fromFile = !m_ncPath.isEmpty();
    const QString ncAtStart = m_ncPath;
    if (!fromFile && !m_doc)
        return refuse(QStringLiteral("Open a .c2d file first."));
    const QString docAtStart = m_doc ? m_doc->filePath() : QString();
    const quint64 loadAtStart = m_doc ? m_doc->loadCount() : 0;
    // The window opens a file into a fresh document and moves it in, so the
    // load count reads 1 after every open: the same path reloaded (a pad's
    // edit rebuilt it) shows only here.
    const quint64 seqAtStart = m_docSeq;
    // Re-exported at the moment Run is pressed, so it is the current document
    // that gets streamed - and a 3D finish is minutes of work, which used to
    // freeze the window with the machine sitting idle.
    GcodeResult r;
    if (fromFile) {
        r.ops = m_ncOps;
        r.gcode = m_ncLines.join(QLatin1Char('\n'));
        // A surface map bends a design's depths as they are made; a file's
        // are fixed, and Z zeroed with the map on is not its zero.
        if (m_useMap->isChecked() && m_map.isValid())
            return refuse(QStringLiteral("The surface map is switched on."),
                          QStringLiteral("It applies to designs only. Turn it off to run a G-code file."));
    } else {
        r = exportWithProgress(this, *m_doc, QStringLiteral("Preparing program"));
    }
    if (!fromFile && r.cancelled) {
        log(QStringLiteral("program preparation cancelled"));
        if (remote)
            *remoteWhy = QStringLiteral("Preparing the program was cancelled.");
        return false;
    }
    if (!fromFile && r.done.isEmpty())
        return refuse(QStringLiteral("No exportable toolpaths."));
    // A toolpath left out (a V-bit in a pocket, a depth it cannot reach)
    // and the rest cut anyway can free the part before it: from a pad,
    // where nobody sees the list, that start is refused.
    if (remote && !r.skipped.isEmpty())
        return refuse(QStringLiteral("Some toolpaths cannot be cut: %1.").arg(r.skipped.join(QStringLiteral(", "))),
                      QStringLiteral("Fix or turn them off, then send again."));
    // A document set up for tiling whose job needs more than one tile is
    // taller than the machine: streamed whole it drove Y past the far end.
    if (!fromFile) {
        const double tileH = m_doc->params().value("tile_height", "0").toDouble();
        if (m_doc->params().value("tiling_enabled") == QLatin1String("1") && tileH > 0
            && tileCount(r.ops, tileH) > 1)
            return refuse(QStringLiteral("This design is tiled: the job needs %1 tiles of %2 mm.")
                              .arg(QString::number(tileCount(r.ops, tileH)), QString::number(tileH)),
                          QStringLiteral("Export G-code makes one program per tile; run those one at a time."));
        // The program is posted from the job zero; its operations are made
        // to match, so the travel check and the stats see what will run.
        if (!jobZeroIsDefault(*m_doc)) {
            if (m_useMap->isChecked() && m_map.isValid())
                return refuse(jobZeroBottom(*m_doc)
                                  ? QStringLiteral("The surface map cannot be used with Z zero on the spoilboard.")
                                  : QStringLiteral("The surface map is measured from the lower-left corner, "
                                                   "and this job's zero is the %1.").arg(jobZeroText(*m_doc)),
                              QStringLiteral("Turn the surface map off, or set the job zero back to lower left, top."));
            shiftOps(r.ops, jobZeroShift(*m_doc));
        }
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
    // on its way: a report GRBL sent just before it applied a new offset
    // still carries the one before.
    const double wcoNow = m_grbl->hasWorkOffset() ? m_grbl->workOffsetZ() : qQNaN();
    if (m_zZeroKnown && (std::abs(wcoNow - m_zZeroWco) < 0.005
                         || std::abs(wcoNow - m_grbl->toolLengthOffset() - m_zZeroWco) < 0.005
                         || std::abs(wcoNow - m_grbl->toolLengthOffsetAtWco() - m_zZeroWco) < 0.005
                         || std::abs(wcoNow - m_grbl->toolLengthOffsetBefore() - m_zZeroWco) < 0.005)) {
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
    lines = safeRapidsTransform(lines);
    {
        QStringList head{QStringLiteral("G21")};
        head += riseToTop();
        // A Z-only first rapid would come down wherever the spindle stands.
        const QString across = GrblStreamer::crossToFirstCut(lines, 0);
        if (m_homed && !across.isEmpty())
            head << across;
        lines = head + lines;
    }

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
                       .arg(fromFile ? QFileInfo(m_ncPath).fileName()
                            : m_doc->filePath().isEmpty()
                                ? QStringLiteral("the project")
                                : QFileInfo(m_doc->filePath()).fileName());
    ask.body = fromFile ? QStringLiteral("G-code file, %1 lines\n%2")
                              .arg(lines.size()).arg(statsSummary(computeStats(ops)))
                        : QStringLiteral("%1 toolpath(s), %2 lines\n%3")
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
    // Not a refusal: the flute length in the tool library may be a guess.
    if (!fromFile)
        for (const Toolpath &t : m_doc->toolpaths())
            if (r.doneIds.contains(t.uuid))
                if (const QString w = fluteLengthWarning(t.type, t.json); !w.isEmpty())
                    ask.body += QStringLiteral("\n\n\u26a0 %1 %2")
                                    .arg(t.json.value(QStringLiteral("name")).toString(), w);
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
    if (from) {
        // Asked below, once the step is found in the program.
    } else if (remote) {
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
            || last == QLatin1String("M30")
            || (coolantWithJob() && (last == QLatin1String("M9") || last == QLatin1String("M09"))))
            lines.removeLast();
        else
            break;
    }
    // Dust collector / coolant with the spindle: on after each start, off
    // before each tool change; the last off is in the end-of-job moves,
    // just before the spindle stops at the park spot.
    if (coolantWithJob()) {
        lines = coolantTransform(lines, coolantMist());
        log(QStringLiteral("dust/coolant: %1 after the spindle starts, M9 before it stops")
                .arg(coolantMist() ? QStringLiteral("M7") : QStringLiteral("M8")));
    }
    // Start here: the program from the chosen step, behind the way in to it.
    QVector<Op> jobTools = toolChanges(ops);
    int toolOrdBase = 0;
    if (from) {
        const JobPlan full = planJob(lines, ops, fromFile ? nullptr : m_doc,
                                     qMin(m_grbl->setting(110, 5000), m_grbl->setting(111, 5000)));
        int found = -1;
        for (int i = full.sections.size() - 1, rank = 0; i >= 0 && found < 0; --i) {
            const JobSection &sec = full.sections.at(i);
            if (sec.kind != JobSection::Toolpath || sec.title() != from->title
                || sec.passes.size() != from->passes)
                continue;
            if (rank++ == from->rank)
                found = i;
        }
        if (found < 0 || from->pass >= full.sections.at(found).passes.size())
            return refuse(QStringLiteral("%1 is not in the program any more.").arg(from->row),
                          QStringLiteral("The design changed since that job. Run it from the start."));
        const JobSection &sec = full.sections.at(found);
        const int fromQ = from->pass >= 0 ? sec.passes.at(from->pass).qStart : sec.qStart;
        int lastTool = -1;
        const int k = GrblStreamer::toolMarkersBefore(lines, fromQ, &lastTool);
        // The bit for that step is in when the last tool change of this
        // design ended on it; anything else (another bit, unknown after a
        // restart) goes through the tool change as a job does.
        const bool bitIn = m_toolStretch == k && m_toolStretchSeq == m_docSeq && m_toolStretchNc == m_ncPath;
        const bool change = k > 0 && !bitIn;
        if (change && m_bsEnabled->isChecked() && !m_haveRef)
            return refuse(QStringLiteral("There is no BitSetter reference yet."),
                          QStringLiteral("Zero Z with the BitSetter on (or press Measure tool "
                                         "with the bit Z was zeroed with), then start again."));
        const QString coolant = coolantWithJob() ? (coolantMist() ? QStringLiteral("M7") : QStringLiteral("M8"))
                                                 : QString();
        QString why;
        const QStringList cut = GrblStreamer::startFromLines(lines, fromQ, riseToTop(), change ? lastTool : -1,
                                                             coolant, &why);
        if (cut.isEmpty())
            return refuse(QStringLiteral("Cannot start from %1: %2.").arg(from->row, why));
        lines = cut;
        toolOrdBase = change ? k - 1 : k;
        jobTools = jobTools.mid(qMin(toolOrdBase, int(jobTools.size())));
        if (!remote) {
            ask.headline = QStringLiteral("Start from %1?").arg(from->row);
            QString body = QStringLiteral("The machine will go up, move over the start, spin up and go down.");
            if (change)
                body += QStringLiteral("\n\nFirst a tool change: it parks and asks for T%1%2.")
                            .arg(lastTool)
                            .arg(m_bsEnabled->isChecked() ? QStringLiteral(", then measures it on the BitSetter")
                                                          : QString());
            body += QStringLiteral("\n\nEverything before this step is left out.");
            ask.body = body;
            ask.okText = QStringLiteral("\u25b6 Start here");
            if (!shopAsk(this, ask))
                return false;
        }
        log(QStringLiteral("== start from %1%2 ==").arg(from->row,
                change ? QStringLiteral(" (tool change to T%1 first)").arg(lastTool) : QString()));
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
    if (m_docSeq != seqAtStart || m_ncPath != ncAtStart
        || (!fromFile && (m_doc->filePath() != docAtStart || m_doc->loadCount() != loadAtStart)))
        return refuse(QStringLiteral("Another design was opened meanwhile; run it again."));
    m_endOfJobPending = true;
    m_progress->setFormat(QStringLiteral("%p%"));
    m_jobTools = jobTools;
    m_toolOrdBase = toolOrdBase;
    // A design (not a G-code file, not an air cut) leaves its place behind
    // while it runs, to continue from after a crash.
    m_runPath = (!fromFile && !air) ? m_doc->filePath() : QString();
    m_runHash = m_runPath.isEmpty() ? QString() : fileHash(m_runPath);
    if (m_runHash.isEmpty())
        m_runPath.clear();     // never saved: nothing to open again
    else
        m_docHash = m_runHash; // saved since it was opened
    m_resumeSaveClock.invalidate();
    m_resumeSavedSec = m_resumeSavedPass = -1;
    m_jobFromFile = fromFile;
    {
        QSettings ps;
        m_hist = HistoryJob();
        m_hist.active = true;
        m_hist.job = fromFile ? QFileInfo(m_ncPath).fileName()
                     : m_doc->filePath().isEmpty() ? QStringLiteral("Untitled")
                                                   : QFileInfo(m_doc->filePath()).fileName();
        if (air)
            m_hist.job += QStringLiteral(" (air cut)");
        if (from)
            m_hist.job += QStringLiteral(" (from %1)").arg(from->row);
        m_hist.machine = machineprofiles::nameOf(ps, machineprofiles::activeId(ps));
        m_hist.start = QDateTime::currentDateTime();
        m_hist.estSec = computeStats(ops).timeSec;
        m_hist.toolpaths = fromFile ? 0 : int(r.done.size());
    }
    if (m_jobView) {
        const QString jobName = fromFile ? QStringLiteral("File: %1").arg(QFileInfo(m_ncPath).fileName())
                                : m_doc->filePath().isEmpty()
                                    ? QStringLiteral("Untitled job")
                                    : QFileInfo(m_doc->filePath()).completeBaseName();
        const QString shown = air ? jobName + QStringLiteral("  (air cut)") : jobName;
        m_jobView->setPlan(planJob(lines, ops, fromFile ? nullptr : m_doc, qMin(m_grbl->setting(110, 5000), m_grbl->setting(111, 5000))),
                           from ? shown + QStringLiteral("  (from %1)").arg(from->row) : shown);
    }
    setJobRunning(true);
    // GRBL keeps overrides until M2 or a reset, and the job's M2 is stripped:
    // three +10 % taps ran every later job at 130 %. Each job starts at 100 %.
    m_grbl->sendRealtime(char(0x90));   // feed
    m_grbl->sendRealtime(char(0x95));   // rapids
    m_grbl->sendRealtime(char(0x99));   // spindle
    m_grbl->startStream(lines);
    return true;
}

// One line in the job history for the job that just ended; once per job.
void MachinePanel::recordJobEnd(const QString &how, int code)
{
    if (!m_hist.active)
        return;
    m_hist.active = false;
    const QDateTime end = QDateTime::currentDateTime();
    const int max = m_progress->maximum();
    const int pct = how == QLatin1String("finished") ? 100
                    : max > 0 ? int(100.0 * m_progress->value() / max) : 0;
    QJsonObject o;
    o.insert(QStringLiteral("job"), m_hist.job);
    o.insert(QStringLiteral("machine"), m_hist.machine);
    o.insert(QStringLiteral("start"), m_hist.start.toString(Qt::ISODate));
    o.insert(QStringLiteral("end"), how);
    o.insert(QStringLiteral("finish"), end.toString(Qt::ISODate));
    o.insert(QStringLiteral("estSec"), qRound(m_hist.estSec));
    o.insert(QStringLiteral("realSec"), qRound(m_hist.start.msecsTo(end) / 1000.0));
    o.insert(QStringLiteral("percent"), pct);
    if (code != 0)
        o.insert(QStringLiteral("code"), code);
    o.insert(QStringLiteral("toolpaths"), m_hist.toolpaths);
    o.insert(QStringLiteral("toolChanges"), m_hist.toolChanges);
    if (!jobhistory::append(o))
        log(QStringLiteral("!! could not write the job history (%1)").arg(jobhistory::filePath()));
}

MachinePanel::StartFrom MachinePanel::rowOf(int section, int pass) const
{
    const JobPlan &plan = m_jobView->plan();
    const JobSection &sec = plan.sections.at(section);
    StartFrom f;
    f.title = sec.title();
    f.passes = int(sec.passes.size());
    f.pass = f.passes > 1 ? pass : -1;
    for (int i = section + 1; i < plan.sections.size(); ++i) {
        const JobSection &o = plan.sections.at(i);
        if (o.kind == JobSection::Toolpath && o.title() == f.title && o.passes.size() == f.passes)
            ++f.rank;
    }
    f.row = m_jobView->rowName(section, f.pass);
    return f;
}

void MachinePanel::saveResumePoint(int acked)
{
    if (m_runPath.isEmpty() || !m_jobView || !m_grbl->isStreaming())
        return;
    const int sec = m_jobView->running();
    if (sec < 0 || sec >= m_jobView->plan().sections.size()
        || m_jobView->plan().sections.at(sec).kind != JobSection::Toolpath)
        return;
    // On each new step, else at most every 2 s: every answered line would
    // be a disk write.
    const bool moved = sec != m_resumeSavedSec || m_jobView->runningPass() != m_resumeSavedPass;
    if (!moved && m_resumeSaveClock.isValid() && m_resumeSaveClock.elapsed() < 2000)
        return;
    m_resumeSaveClock.start();
    m_resumeSavedSec = sec;
    m_resumeSavedPass = m_jobView->runningPass();
    const StartFrom f = rowOf(sec, m_jobView->runningPass());
    QSettings set;
    set.setValue(QStringLiteral("resume/path"), m_runPath);
    set.setValue(QStringLiteral("resume/hash"), m_runHash);
    set.setValue(QStringLiteral("resume/acked"), acked);
    set.setValue(QStringLiteral("resume/title"), f.title);
    set.setValue(QStringLiteral("resume/passes"), f.passes);
    set.setValue(QStringLiteral("resume/rank"), f.rank);
    set.setValue(QStringLiteral("resume/pass"), f.pass);
    set.setValue(QStringLiteral("resume/row"), f.row);
    set.sync();
}

void MachinePanel::clearResumePoint()
{
    QSettings set;
    set.remove(QStringLiteral("resume"));
    set.sync();
}

void MachinePanel::updateContinueOffer()
{
    if (!m_continueBtn)
        return;
    QSettings set;
    const QString path = set.value(QStringLiteral("resume/path")).toString();
    const bool show = !path.isEmpty() && m_doc && m_ncPath.isEmpty() && m_doc->filePath() == path
                      && !m_docHash.isEmpty() && set.value(QStringLiteral("resume/hash")).toString() == m_docHash
                      && m_homed && m_grbl->isConnected() && !m_grbl->isStreaming() && !isBusy() && !m_held
                      && m_machineState == QLatin1String("Idle");
    if (show)
        m_continueBtn->setText(QStringLiteral("\u25b6 Continue from %1 (where it stopped)")
                                   .arg(set.value(QStringLiteral("resume/row")).toString()));
    m_continueBtn->setVisible(show);
}

void MachinePanel::continueWhereStopped()
{
    QSettings set;
    StartFrom f;
    f.title = set.value(QStringLiteral("resume/title")).toString();
    f.passes = set.value(QStringLiteral("resume/passes")).toInt();
    f.rank = set.value(QStringLiteral("resume/rank")).toInt();
    f.pass = set.value(QStringLiteral("resume/pass"), -1).toInt();
    f.row = set.value(QStringLiteral("resume/row")).toString();
    if (f.title.isEmpty() || isBusy() || m_grbl->isStreaming() || m_held)
        return;
    runProgram(nullptr, &f);
}

// A row of the last job's list was chosen: start the job again from there.
void MachinePanel::startHere(int section, int pass)
{
    if (!m_jobView || section < 0 || section >= m_jobView->plan().sections.size())
        return;
    const StartFrom f = rowOf(section, pass);
    if (isBusy() || m_grbl->isStreaming() || m_held) {
        log(QStringLiteral("!! busy — cannot start from a step now"));
        return;
    }
    runProgram(nullptr, &f);
}

bool MachinePanel::startFromRemote(QString *why)
{
    QString dummy;
    return runProgram(why ? why : &dummy);
}

bool MachinePanel::holdFromRemote(const QString &who, QString *why)
{
    if (!m_grbl->isConnected()) {
        if (why) *why = QStringLiteral("the machine is not connected");
        return false;
    }
    if (m_held || m_liftHold) {
        log(QStringLiteral("Hold from pad %1: already held").arg(who));
        return true;
    }
    log(QStringLiteral("Hold from pad %1").arg(who));
    m_pauseBtn->click();                  // same path as the on-screen button
    return true;
}

bool MachinePanel::resumeFromRemote(const QString &who, QString *why)
{
    if (!m_grbl->isConnected()) {
        if (why) *why = QStringLiteral("the machine is not connected");
        return false;
    }
    if (!m_held && !m_liftHold) {
        if (why) *why = QStringLiteral("the machine is not on hold");
        return false;
    }
    log(QStringLiteral("Resume from pad %1").arg(who));
    m_pauseBtn->click();
    return true;
}

bool MachinePanel::stopFromRemote(const QString &who, QString *why)
{
    if (!m_grbl->isConnected()) {
        if (why) *why = QStringLiteral("the machine is not connected");
        return false;
    }
    log(QStringLiteral("Stop from pad %1").arg(who));
    m_stopBtn->click();
    return true;
}

// ---- run-time counters ---------------------------------------------------------

void MachinePanel::loadCounters()
{
    QSettings s;
    m_countersId = machineprofiles::activeId(s);
    m_counters = machinecounters::load(s, m_countersId);
    m_counterClock.invalidate();
    m_counterFlush.start();
    showDue();
}

void MachinePanel::flushCounters()
{
    if (m_countersId.isEmpty())
        return;
    QSettings s;
    machinecounters::save(s, m_countersId, m_counters);
    m_counterFlush.start();
}

// The bit as the tool library knows it: its id when a library bit has the
// same model (or name and diameter), else its name.
QString MachinePanel::bitKey(const LibraryTool &bit)
{
    const QString cacheKey = QStringLiteral("%1|%2|%3").arg(bit.model, bit.name).arg(bit.diameter);
    auto hit = m_bitKeys.constFind(cacheKey);
    if (hit != m_bitKeys.cend())
        return hit.value();
    QString key = bit.name.isEmpty() ? bit.model : bit.name;
    ToolLibrary lib;
    if (lib.loadDefault()) {
        for (const LibraryTool &lt : lib.tools()) {
            const bool sameModel = !bit.model.isEmpty() && lt.model == bit.model;
            const bool sameName = !bit.name.isEmpty() && lt.name == bit.name
                                  && std::abs(lt.diameter - bit.diameter) < 1e-3;
            if (sameModel || sameName) {
                key = lt.id;
                break;
            }
        }
    }
    m_bitKeys.insert(cacheKey, key);
    return key;
}

void MachinePanel::countStatus(const MachineStatus &st)
{
    if (!st.valid)
        return;
    // The time since the last report goes to what this one says is running.
    const double dt = m_counterClock.isValid() ? m_counterClock.restart() / 1000.0 : 0.0;
    if (!m_counterClock.isValid())
        m_counterClock.start();
    machinecounters::Sample smp;
    smp.state = st.state;
    smp.spindleRpm = st.spindle;
    smp.spindleReported = st.spindleReported;
    const JobSection *sec = m_jobView ? m_jobView->runningSection() : nullptr;
    const bool cutting = sec && sec->kind == JobSection::Toolpath && m_grbl->isStreaming();
    smp.jobSpindleOn = cutting && st.state == QLatin1String("Run");
    if (cutting && (!sec->bit.name.isEmpty() || !sec->bit.model.isEmpty())) {
        smp.bitId = bitKey(sec->bit);
        smp.bitName = sec->bit.name.isEmpty() ? sec->bit.model : sec->bit.name;
    }
    const QString dueBefore = machinecounters::dueLine(m_counters);
    machinecounters::accumulate(m_counters, dt, smp);
    if (machinecounters::dueLine(m_counters) != dueBefore)
        showDue();
    if (!m_counterFlush.isValid() || m_counterFlush.elapsed() > 60000)
        flushCounters();
}

void MachinePanel::showDue()
{
    if (!m_dueLabel)
        return;
    const QString line = machinecounters::dueLine(m_counters);
    m_dueLabel->setText(line);
    m_dueLabel->setVisible(!line.isEmpty());
}

void MachinePanel::openCounters()
{
    QSettings s;
    if (machinecounters::showDialog(this, m_counters, machineprofiles::nameOf(s, m_countersId)))
        flushCounters();
    showDue();
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
    flushCounters();                     // to the machine they were counted on
    loadCounters();
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
    // On hold here (Hold pressed, here or on a pad): the pad offers Resume.
    o.insert(QStringLiteral("held"), m_grbl->isConnected() && (m_held || m_liftHold));
    o.insert(QStringLiteral("coolant"), m_grbl->isConnected() && m_coolantOn);
    o.insert(QStringLiteral("job"), !m_ncPath.isEmpty() ? QFileInfo(m_ncPath).fileName()
                                    : m_doc && !m_doc->filePath().isEmpty()
                                        ? QFileInfo(m_doc->filePath()).fileName() : QString());
    // The running job's steps, for the pad's job list.
    if (m_jobView && m_jobView->hasPlan()) {
        o.insert(QStringLiteral("steps"), m_jobView->stepsJson());
        // How the last job ended, so the pad says Finished, not "last job".
        if (!m_grbl->isStreaming())
            o.insert(QStringLiteral("jobEnd"), m_jobView->isFinished() ? QStringLiteral("finished")
                                               : m_jobView->isStopped() ? QStringLiteral("stopped")
                                                                        : QString());
    }
    return o;
}

} // namespace c2d
