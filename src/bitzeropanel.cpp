#include "bitzeropanel.h"

#include <QButtonGroup>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>

namespace c2d {

static QDoubleSpinBox *spin(QWidget *parent, double lo, double hi, int decimals,
                            const QString &suffix)
{
    auto *s = new QDoubleSpinBox(parent);
    s->setRange(lo, hi);
    s->setDecimals(decimals);
    s->setSuffix(suffix);
    s->setMinimumHeight(32);
    return s;
}

static QToolButton *toggle(QWidget *parent, const QString &text)
{
    auto *b = new QToolButton(parent);
    b->setObjectName(QStringLiteral("step"));
    b->setText(text);
    b->setCheckable(true);
    b->setMinimumHeight(40);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
}

static QWidget *toggleBox(QWidget *parent)
{
    auto *box = new QWidget(parent);
    box->setObjectName(QStringLiteral("stepBox"));
    box->setAttribute(Qt::WA_StyledBackground);
    box->setStyleSheet(QStringLiteral("#stepBox { background: #16181d; border-radius: 12px; }"));
    return box;
}

BitZeroPanel::BitZeroPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(18, 12, 18, 16);
    lay->setSpacing(10);

    // Header: back to the job, and which plate.
    auto *head = new QHBoxLayout;
    head->setSpacing(10);
    auto *back = new QPushButton(QStringLiteral("← Back to job"), this);
    back->setObjectName(QStringLiteral("big"));
    back->setMinimumHeight(40);
    back->setFocusPolicy(Qt::NoFocus);
    connect(back, &QPushButton::clicked, this, &BitZeroPanel::backRequested);
    head->addWidget(back);
    lay->addLayout(head);

    QWidget *plateBox = toggleBox(this);
    auto *plateLay = new QHBoxLayout(plateBox);
    plateLay->setContentsMargins(5, 5, 5, 5);
    plateLay->setSpacing(4);
    m_plates = new QButtonGroup(this);
    m_plates->setExclusive(true);
    QToolButton *v2 = toggle(plateBox, QStringLiteral("V2"));
    QToolButton *v1 = toggle(plateBox, QStringLiteral("V1 block"));
    v2->setToolTip(QStringLiteral("BitZero V2: the thin plate with the bore over the corner"));
    v1->setToolTip(QStringLiteral("BitZero V1: the original 60 × 60 block, probed from outside"));
    v2->setChecked(true);
    m_plates->addButton(v2, 0);
    m_plates->addButton(v1, 1);
    plateLay->addWidget(v2);
    plateLay->addWidget(v1);
    head->addWidget(plateBox, 1);

    // What to find: one row of the same toggles the jog step uses.
    QWidget *modeBox = toggleBox(this);
    auto *modeGrid = new QGridLayout(modeBox);
    modeGrid->setContentsMargins(5, 5, 5, 5);
    modeGrid->setSpacing(4);
    m_modes = new QButtonGroup(this);
    m_modes->setExclusive(true);
    const char *names[] = {"Corner", "Z only", "X only", "Y only"};
    for (int i = 0; i < 4; ++i) {
        QToolButton *b = toggle(modeBox, QString::fromUtf8(names[i]));
        b->setChecked(i == 0);
        m_modes->addButton(b, i);
        modeGrid->addWidget(b, 0, i);
    }
    lay->addWidget(modeBox);

    m_help = new QLabel(this);
    m_help->setWordWrap(true);
    m_help->setStyleSheet(QStringLiteral("color: #a3abb8;"));
    lay->addWidget(m_help);

    // The probe circuit, live. Nothing should move until this reacts.
    m_contact = new QLabel(this);
    m_contact->setMinimumHeight(36);
    m_contact->setAlignment(Qt::AlignCenter);
    lay->addWidget(m_contact);
    auto *contactHint = new QLabel(QStringLiteral(
        "Clip the ground on, then tap the tool on the plate: this must say "
        "CLOSED (and the V2's light come on)."), this);
    contactHint->setWordWrap(true);
    contactHint->setStyleSheet(QStringLiteral("color: #8b93a1; font-size: 12px;"));
    lay->addWidget(contactHint);

    // Plate and feeds, folded like "Feed override, air cut" on the job page.
    auto *dimsToggle = new QToolButton(this);
    dimsToggle->setCheckable(true);
    dimsToggle->setAutoRaise(true);
    dimsToggle->setArrowType(Qt::RightArrow);
    dimsToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    dimsToggle->setText(QStringLiteral("Plate and feeds"));
    dimsToggle->setFocusPolicy(Qt::NoFocus);
    lay->addWidget(dimsToggle);
    auto *dims = new QWidget(this);
    auto *form = new QFormLayout(dims);
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(6);
    auto row = [&](const QString &label, QWidget *field, QWidget **rowOut = nullptr,
                   QWidget **labelOut = nullptr) {
        auto *l = new QLabel(label, dims);
        form->addRow(l, field);
        if (rowOut) *rowOut = field;
        if (labelOut) *labelOut = l;
    };
    m_thick = spin(dims, 1, 50, 2, QStringLiteral(" mm"));
    m_thick->setToolTip(QStringLiteral(
        "Plate top above the stock top: 13 mm for the V2, 22 mm for the V1 block "
        "by owners' measurements. Check yours with calipers if Z comes out wrong "
        "by a constant."));
    row(QStringLiteral("Top over stock"), m_thick);
    m_lipDrop = spin(dims, 0, 10, 2, QStringLiteral(" mm"));
    m_lipDrop->setToolTip(QStringLiteral(
        "How far the locating lips hang below the base. In a Z-only cycle the whole "
        "plate stands on the surface, on its lips, so its top is this much higher "
        "than in a corner cycle. Measured 2.8 mm on a V2."));
    row(QStringLiteral("Lip drop (Z only)"), m_lipDrop);
    // V2
    m_search = spin(dims, 5, 100, 1, QStringLiteral(" mm"));
    m_search->setToolTip(QStringLiteral("Farthest a bore-wall probe may travel before it gives up"));
    row(QStringLiteral("Bore search"), m_search, &m_v2Rows[0], &m_v2Labels[0]);
    auto *spotW = new QWidget(dims);
    auto *spot = new QHBoxLayout(spotW);
    spot->setContentsMargins(0, 0, 0, 0);
    m_spotX = spin(spotW, -100, 100, 1, QStringLiteral(" mm"));
    m_spotY = spin(spotW, -100, 100, 1, QStringLiteral(" mm"));
    spot->addWidget(new QLabel(QStringLiteral("X"), spotW));
    spot->addWidget(m_spotX, 1);
    spot->addWidget(new QLabel(QStringLiteral("Y"), spotW));
    spot->addWidget(m_spotY, 1);
    row(QStringLiteral("Z touch spot"), spotW, &m_v2Rows[1], &m_v2Labels[1]);
    // V1
    m_toolDia = spin(dims, 0.5, 25.4, 3, QStringLiteral(" mm"));
    m_toolDia->setToolTip(QStringLiteral("Diameter of the pin or end mill doing the probing"));
    row(QStringLiteral("Tool diameter"), m_toolDia, &m_v1Rows[0], &m_v1Labels[0]);
    m_lip = spin(dims, 0, 30, 2, QStringLiteral(" mm"));
    m_lip->setToolTip(QStringLiteral("Lip width: from the block's outer side to the stock edge"));
    row(QStringLiteral("Lip width"), m_lip, &m_v1Rows[1], &m_v1Labels[1]);
    m_clearance = spin(dims, 10, 120, 1, QStringLiteral(" mm"));
    m_clearance->setToolTip(QStringLiteral(
        "How far left / forward of the start to go before descending beside the "
        "block. Must clear the block's side; the block is 60 mm wide."));
    row(QStringLiteral("Side clearance"), m_clearance, &m_v1Rows[2], &m_v1Labels[2]);
    m_sideDepth = spin(dims, 1, 20, 1, QStringLiteral(" mm"));
    m_sideDepth->setToolTip(QStringLiteral("How far below the block top the side is probed"));
    row(QStringLiteral("Side probe depth"), m_sideDepth, &m_v1Rows[3], &m_v1Labels[3]);
    // common
    m_zTravel = spin(dims, 2, 100, 1, QStringLiteral(" mm"));
    row(QStringLiteral("Z search"), m_zTravel);
    auto *feedsW = new QWidget(dims);
    auto *feeds = new QHBoxLayout(feedsW);
    feeds->setContentsMargins(0, 0, 0, 0);
    m_fast = spin(feedsW, 10, 2000, 0, QStringLiteral(" fast"));
    m_slow = spin(feedsW, 5, 500, 0, QStringLiteral(" slow"));
    feeds->addWidget(m_fast, 1);
    feeds->addWidget(m_slow, 1);
    row(QStringLiteral("Feeds mm/min"), feedsW);
    m_retract = spin(dims, 0.5, 10, 1, QStringLiteral(" mm"));
    row(QStringLiteral("Back-off"), m_retract);
    dims->hide();
    lay->addWidget(dims);
    connect(dimsToggle, &QToolButton::toggled, this, [dims, dimsToggle](bool on) {
        dims->setVisible(on);
        dimsToggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
    });

    lay->addStretch(1);
    m_begin = new QPushButton(QStringLiteral("▶  Begin probe"), this);
    m_begin->setObjectName(QStringLiteral("start"));
    m_begin->setMinimumHeight(64);
    lay->addWidget(m_begin);
    connect(m_begin, &QPushButton::clicked, this, &BitZeroPanel::beginRequested);
    connect(m_modes, &QButtonGroup::idClicked, this, [this] { updateHelp(); save(); });
    // Each plate keeps its own numbers: switching loads the other set.
    connect(m_plates, &QButtonGroup::idClicked, this, [this] {
        QSettings().setValue(QStringLiteral("machine/bitzero/plate"), m_plates->checkedId());
        load();
        updateRows();
        updateHelp();
    });
    for (QDoubleSpinBox *s : {m_thick, m_search, m_spotX, m_spotY, m_zTravel, m_fast, m_slow,
                              m_retract, m_lip, m_toolDia, m_clearance, m_sideDepth, m_lipDrop})
        connect(s, &QDoubleSpinBox::valueChanged, this, [this] { save(); });

    if (QAbstractButton *b = m_plates->button(
            QSettings().value(QStringLiteral("machine/bitzero/plate"), 0).toInt()))
        b->setChecked(true);
    load();
    updateRows();
    setContact(false, false);
    setConnected(false);
    updateHelp();
}

BitZeroPanel::Mode BitZeroPanel::mode() const
{
    return Mode(qBound(0, m_modes->checkedId(), 3));
}

QString BitZeroPanel::plateKey() const
{
    return m_plates->checkedId() == 1 ? QStringLiteral("machine/bitzero/v1")
                                      : QStringLiteral("machine/bitzero/v2");
}

BitZeroConfig BitZeroPanel::config() const
{
    BitZeroConfig c;
    c.v1 = m_plates->checkedId() == 1;
    c.thickness = m_thick->value();
    c.search = m_search->value();
    c.zSpotX = m_spotX->value();
    c.zSpotY = m_spotY->value();
    c.zTravel = m_zTravel->value();
    c.fastFeed = m_fast->value();
    c.slowFeed = m_slow->value();
    c.retract = m_retract->value();
    c.lipWidth = m_lip->value();
    c.toolDiameter = m_toolDia->value();
    c.sideClearance = m_clearance->value();
    c.sideDepth = m_sideDepth->value();
    c.lipDrop = m_lipDrop->value();
    return c;
}

void BitZeroPanel::updateRows()
{
    const bool v1 = m_plates->checkedId() == 1;
    for (QWidget *w : m_v2Rows) if (w) w->setVisible(!v1);
    for (QWidget *w : m_v2Labels) if (w) w->setVisible(!v1);
    for (QWidget *w : m_v1Rows) if (w) w->setVisible(v1);
    for (QWidget *w : m_v1Labels) if (w) w->setVisible(v1);
}

void BitZeroPanel::setContact(bool closed, bool known)
{
    if (!known) {
        m_contact->setText(QStringLiteral("Probe: not connected"));
        m_contact->setStyleSheet(QStringLiteral(
            "border: 1px solid #2a2e36; border-radius: 10px; color: #8b93a1; padding: 4px 10px;"));
    } else if (closed) {
        m_contact->setText(QStringLiteral("Probe: ● CLOSED"));
        m_contact->setStyleSheet(QStringLiteral(
            "border: 1px solid #f2a536; border-radius: 10px; color: #f2a536; "
            "font-weight: bold; padding: 4px 10px;"));
    } else {
        m_contact->setText(QStringLiteral("Probe: ○ open"));
        m_contact->setStyleSheet(QStringLiteral(
            "border: 1px solid #6fcf97; border-radius: 10px; color: #6fcf97; padding: 4px 10px;"));
    }
}

void BitZeroPanel::setBusy(bool on)
{
    m_busy = on;
    m_begin->setText(on ? QStringLiteral("Probing…") : QStringLiteral("▶  Begin probe"));
    m_begin->setEnabled(!on && m_connected);
}

void BitZeroPanel::setConnected(bool on)
{
    m_connected = on;
    m_begin->setEnabled(!m_busy && on);
}

void BitZeroPanel::updateHelp()
{
    const bool v1 = m_plates->checkedId() == 1;
    QString t;
    switch (mode()) {
    case Corner:
        t = v1 ? QStringLiteral(
                "Block on the <b>lower-left corner</b> of the stock, lips hooked over both "
                "edges. Jog the tool a few millimetres <b>above the block top, over the "
                "circle</b>. Finds Z on the top, then goes left and down to touch the block's "
                "left side, right and forward to touch its front side. Stock edge = side + "
                "lip width, so set the tool diameter under Plate and feeds.")
               : QStringLiteral(
                "Probing pin in the collet. BitZero on the <b>lower-left corner</b> of the "
                "stock, lips over the edge and pressed against both sides. Jog the pin "
                "<b>into the bore</b>, tip just below the plate top. Finds both X walls, "
                "both Y walls, touches the plate for Z, sets X0 Y0 Z0 at the corner.");
        break;
    case ZOnly:
        t = QStringLiteral(
            "%1 flat on the surface to measure, standing on its lips. Jog the tool a "
            "few millimetres <b>above it</b>. Probes straight down, sets Z0 one plate "
            "height plus the lip drop below the contact.")
                .arg(v1 ? QStringLiteral("Block") : QStringLiteral("BitZero"));
        break;
    case XOnly:
        t = v1 ? QStringLiteral(
                "Block on a corner, lips over the edges. Tool a few millimetres <b>above "
                "the top, over the circle</b>. Touches the top for height, then the block's "
                "left side from outside; X0 = side + lip width.")
               : QStringLiteral(
                "BitZero on a corner, lips against the stock. Jog the pin <b>into the "
                "bore</b>. Finds both X walls, sets X0 at their middle: the left edge.");
        break;
    case YOnly:
        t = v1 ? QStringLiteral(
                "Block on a corner, lips over the edges. Tool a few millimetres <b>above "
                "the top, over the circle</b>. Touches the top for height, then the block's "
                "front side from outside; Y0 = side + lip width.")
               : QStringLiteral(
                "BitZero on a corner, lips against the stock. Jog the pin <b>into the "
                "bore</b>. Finds both Y walls, sets Y0 at their middle: the front edge.");
        break;
    }
    m_help->setText(t);
}

void BitZeroPanel::load()
{
    m_loading = true;
    QSettings s;
    BitZeroConfig d;
    d.v1 = m_plates->checkedId() == 1;
    if (d.v1) {
        d.thickness = 22.0;
        d.lipDrop = 3.0;
    }
    s.beginGroup(plateKey());
    m_thick->setValue(s.value(QStringLiteral("thickness"), d.thickness).toDouble());
    m_search->setValue(s.value(QStringLiteral("search"), d.search).toDouble());
    m_spotX->setValue(s.value(QStringLiteral("zSpotX"), d.zSpotX).toDouble());
    m_spotY->setValue(s.value(QStringLiteral("zSpotY"), d.zSpotY).toDouble());
    m_zTravel->setValue(s.value(QStringLiteral("zTravel"), d.zTravel).toDouble());
    m_fast->setValue(s.value(QStringLiteral("fast"), d.fastFeed).toDouble());
    m_slow->setValue(s.value(QStringLiteral("slow"), d.slowFeed).toDouble());
    m_retract->setValue(s.value(QStringLiteral("retract"), d.retract).toDouble());
    m_lip->setValue(s.value(QStringLiteral("lip"), d.lipWidth).toDouble());
    m_toolDia->setValue(s.value(QStringLiteral("toolDiameter"), d.toolDiameter).toDouble());
    m_clearance->setValue(s.value(QStringLiteral("sideClearance"), d.sideClearance).toDouble());
    m_sideDepth->setValue(s.value(QStringLiteral("sideDepth"), d.sideDepth).toDouble());
    m_lipDrop->setValue(s.value(QStringLiteral("lipDrop"), d.lipDrop).toDouble());
    s.endGroup();
    if (QAbstractButton *b = m_modes->button(s.value(QStringLiteral("machine/bitzero/mode"), 0).toInt()))
        b->setChecked(true);
    m_loading = false;
}

void BitZeroPanel::save()
{
    if (m_loading)
        return;
    QSettings s;
    s.beginGroup(plateKey());
    s.setValue(QStringLiteral("thickness"), m_thick->value());
    s.setValue(QStringLiteral("search"), m_search->value());
    s.setValue(QStringLiteral("zSpotX"), m_spotX->value());
    s.setValue(QStringLiteral("zSpotY"), m_spotY->value());
    s.setValue(QStringLiteral("zTravel"), m_zTravel->value());
    s.setValue(QStringLiteral("fast"), m_fast->value());
    s.setValue(QStringLiteral("slow"), m_slow->value());
    s.setValue(QStringLiteral("retract"), m_retract->value());
    s.setValue(QStringLiteral("lip"), m_lip->value());
    s.setValue(QStringLiteral("toolDiameter"), m_toolDia->value());
    s.setValue(QStringLiteral("sideClearance"), m_clearance->value());
    s.setValue(QStringLiteral("sideDepth"), m_sideDepth->value());
    s.setValue(QStringLiteral("lipDrop"), m_lipDrop->value());
    s.endGroup();
    s.setValue(QStringLiteral("machine/bitzero/mode"), m_modes->checkedId());
}

} // namespace c2d
