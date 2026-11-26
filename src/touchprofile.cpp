#include "touchprofile.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFileDialog>
#include <QLayout>
#include <QMessageBox>
#include <QPainter>
#include <QProxyStyle>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QStyleOption>
#include <QVBoxLayout>

namespace c2d {
namespace touch {

namespace {

bool g_enabled = false;

// The spin box arrows a finger can hit: "−" and "+" side by side at the
// right end of the field, each as wide as the field is high, in place of
// Fusion's two 16 px half-height arrows.
int spinButtonWidth(const QRect &r)
{
    return qBound(32, r.height() - 4, kTarget);
}

class TouchStyle : public QProxyStyle
{
public:
    using QProxyStyle::QProxyStyle;

    int pixelMetric(PixelMetric m, const QStyleOption *opt, const QWidget *w) const override
    {
        switch (m) {
        case PM_ScrollBarExtent:          return 32;
        case PM_ScrollBarSliderMin:       return 40;
        case PM_IndicatorWidth:
        case PM_IndicatorHeight:
        case PM_ExclusiveIndicatorWidth:
        case PM_ExclusiveIndicatorHeight: return 28;
        case PM_SmallIconSize:            return 24;     // menu and list icons
        case PM_ToolBarExtensionExtent:   return kTarget;
        case PM_TabBarScrollButtonWidth:  return 40;
        case PM_MenuScrollerHeight:       return 36;
        case PM_SliderLength:             return 36;
        case PM_SliderThickness:          return 40;
        case PM_SplitterWidth:            return 16;
        default:                          return QProxyStyle::pixelMetric(m, opt, w);
        }
    }

    QSize sizeFromContents(ContentsType ct, const QStyleOption *opt, const QSize &contents,
                           const QWidget *w) const override
    {
        QSize sz = QProxyStyle::sizeFromContents(ct, opt, contents, w);
        switch (ct) {
        case CT_ItemViewItem:
        case CT_MenuBarItem:
            sz.setHeight(qMax(sz.height(), kRow));
            break;
        case CT_MenuItem:
            if (const auto *mi = qstyleoption_cast<const QStyleOptionMenuItem *>(opt))
                if (mi->menuItemType != QStyleOptionMenuItem::Separator)
                    sz.setHeight(qMax(sz.height(), kRow));
            break;
        case CT_TabBarTab:
            if (const auto *tab = qstyleoption_cast<const QStyleOptionTab *>(opt)) {
                const bool vertical = tab->shape == QTabBar::RoundedWest || tab->shape == QTabBar::RoundedEast
                                      || tab->shape == QTabBar::TriangularWest
                                      || tab->shape == QTabBar::TriangularEast;
                if (vertical)
                    sz.setWidth(qMax(sz.width(), kRow));
                else
                    sz.setHeight(qMax(sz.height(), kRow));
            }
            break;
        case CT_SpinBox:
            // Room for the two finger-wide buttons instead of Fusion's one.
            sz.setHeight(qMax(sz.height(), kRow));
            sz.rwidth() += 2 * spinButtonWidth(QRect(0, 0, 0, sz.height())) - 16;
            break;
        case CT_ComboBox:
        case CT_LineEdit:
            sz.setHeight(qMax(sz.height(), kRow));
            break;
        case CT_PushButton:
        case CT_CheckBox:
        case CT_RadioButton:
            sz.setHeight(qMax(sz.height(), kTarget));
            break;
        default:
            break;
        }
        return sz;
    }

    QRect subControlRect(ComplexControl cc, const QStyleOptionComplex *opt, SubControl sc,
                         const QWidget *w) const override
    {
        if (cc == CC_SpinBox)
            if (const auto *sb = qstyleoption_cast<const QStyleOptionSpinBox *>(opt))
                if (sb->buttonSymbols != QAbstractSpinBox::NoButtons) {
                    const QRect r = sb->rect;
                    const int fw = sb->frame ? 2 : 0;
                    const int bw = spinButtonWidth(r);
                    const QRect up(r.right() - fw - bw + 1, r.top() + fw, bw, r.height() - 2 * fw);
                    const QRect down = up.translated(-bw, 0);
                    QRect res;
                    switch (sc) {
                    case SC_SpinBoxUp:        res = up; break;
                    case SC_SpinBoxDown:      res = down; break;
                    case SC_SpinBoxEditField: res = QRect(r.left() + fw + 2, r.top() + fw,
                                                          down.left() - r.left() - fw - 4, r.height() - 2 * fw);
                                              break;
                    case SC_SpinBoxFrame:     res = r; break;
                    default:                  break;
                    }
                    if (res.isValid())
                        return visualRect(sb->direction, r, res);
                }
        return QProxyStyle::subControlRect(cc, opt, sc, w);
    }

    void drawComplexControl(ComplexControl cc, const QStyleOptionComplex *opt, QPainter *p,
                            const QWidget *w) const override
    {
        const auto *sb = qstyleoption_cast<const QStyleOptionSpinBox *>(opt);
        if (cc != CC_SpinBox || !sb || sb->buttonSymbols == QAbstractSpinBox::NoButtons) {
            QProxyStyle::drawComplexControl(cc, opt, p, w);
            return;
        }
        // The field and its frame as the base style draws them, then our
        // two buttons on top.
        QStyleOptionSpinBox field(*sb);
        field.buttonSymbols = QAbstractSpinBox::NoButtons;
        field.subControls &= ~(SC_SpinBoxUp | SC_SpinBoxDown);
        QProxyStyle::drawComplexControl(cc, &field, p, w);

        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QPalette &pal = sb->palette;
        for (SubControl part : {SC_SpinBoxDown, SC_SpinBoxUp}) {
            const QRect r = subControlRect(cc, sb, part, w).adjusted(2, 2, -2, -2);
            const bool on = part == SC_SpinBoxUp ? (sb->stepEnabled & QAbstractSpinBox::StepUpEnabled)
                                                 : (sb->stepEnabled & QAbstractSpinBox::StepDownEnabled);
            const bool pressed = (sb->activeSubControls & part) && (sb->state & State_Sunken);
            QColor fill = pal.color(QPalette::Button);
            if (pressed)
                fill = fill.lighter(140);
            p->setPen(QPen(pal.color(QPalette::Mid), 1));
            p->setBrush(fill);
            p->drawRoundedRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
            QColor ink = pal.color(on && (sb->state & State_Enabled) ? QPalette::Active : QPalette::Disabled,
                                   QPalette::ButtonText);
            p->setPen(QPen(ink, 2.0, Qt::SolidLine, Qt::RoundCap));
            const QPointF c = QRectF(r).center();
            const qreal a = qMin(r.width(), r.height()) * 0.2;
            p->drawLine(QPointF(c.x() - a, c.y()), QPointF(c.x() + a, c.y()));
            if (part == SC_SpinBoxUp)
                p->drawLine(QPointF(c.x(), c.y() - a), QPointF(c.x(), c.y() + a));
        }
        p->restore();
    }

    int styleHint(StyleHint hint, const QStyleOption *opt, const QWidget *w,
                  QStyleHintReturn *ret) const override
    {
        // A tap opens a combo box's list where it can be read, not over the
        // field the finger is still on.
        if (hint == SH_ComboBox_Popup)
            return 0;
        return QProxyStyle::styleHint(hint, opt, w, ret);
    }
};

// What the proxy style cannot reach: the application's style sheet already
// sizes menus and tool buttons, so their touch sizes are style sheet rules on
// top of it. min-height counts without the padding (4 px a side on tool
// buttons), so 40 makes 48.
const char *kSheet = R"(
    QMenuBar { font-size: 16px; }
    QMenuBar::item { padding: 12px 14px; }
    QMenu { font-size: 16px; }
    QMenu::item { padding: 12px 36px 12px 16px; }
    QMenu::separator { height: 9px; }
    QToolBar { padding: 2px; }
    QToolButton { min-width: 32px; min-height: 40px; }
    QPushButton { min-height: 40px; }
    QComboBox, QLineEdit, QAbstractSpinBox { min-height: 40px; }   /* kField */
    QCheckBox, QRadioButton { min-height: 44px; spacing: 10px; }
    QTabBar::tab { min-height: 40px; }
)";

// The dialog's screen, without the system bars.
QRect screenRect(QDialog *dlg)
{
    QScreen *s = nullptr;
    if (QWidget *p = dlg->parentWidget())
        s = p->screen();
    if (!s)
        s = dlg->screen();
    if (!s)
        s = QGuiApplication::primaryScreen();
    return s ? s->availableGeometry() : QRect();
}

// The style sheet sizes a spin box for one of Fusion's narrow arrows (with a
// style sheet set, Qt does not ask the style); ours are two finger-wide
// buttons, so the box asks for that much more room the first time it shows.
void widenSpinBox(QAbstractSpinBox *sb)
{
    if (sb->property("touchWidened").toBool() || sb->buttonSymbols() == QAbstractSpinBox::NoButtons)
        return;
    sb->setProperty("touchWidened", true);
    const int extra = 2 * spinButtonWidth(QRect(0, 0, 0, qMax(sb->height(), kRow))) - 16;
    sb->setMinimumWidth(qMax(sb->minimumWidth(), sb->sizeHint().width() + extra));
}

class Watcher : public QObject
{
public:
    using QObject::QObject;
    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (e->type() == QEvent::Show) {
            if (auto *dlg = qobject_cast<QDialog *>(o))
                fitToScreen(dlg);
            else if (g_enabled)
                if (auto *sb = qobject_cast<QAbstractSpinBox *>(o))
                    widenSpinBox(sb);
        }
        return false;
    }
};

} // namespace

Mode mode(QSettings &s)
{
    const QString v = s.value(QStringLiteral("ui/touch"), QStringLiteral("auto")).toString();
    if (v == QLatin1String("on"))
        return Mode::On;
    if (v == QLatin1String("off"))
        return Mode::Off;
    return Mode::Automatic;
}

void setMode(QSettings &s, Mode m)
{
    s.setValue(QStringLiteral("ui/touch"),
               m == Mode::On ? QStringLiteral("on") : m == Mode::Off ? QStringLiteral("off")
                                                                     : QStringLiteral("auto"));
}

QStringList modeLabels()
{
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
    return {QStringLiteral("Automatic (on: this is a pad)"), QStringLiteral("On"), QStringLiteral("Off")};
#else
    return {QStringLiteral("Automatic (off: this is a computer)"), QStringLiteral("On (touch screen)"),
            QStringLiteral("Off")};
#endif
}

bool wantedBy(Mode m)
{
    if (m == Mode::On)
        return true;
    if (m == Mode::Off)
        return false;
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
    return true;
#else
    return false;
#endif
}

void setEnabled(bool on) { g_enabled = on; }
bool enabled() { return g_enabled; }

QSize iconSize(int desktopPx)
{
    const int px = g_enabled ? (desktopPx * 4 + 2) / 3 : desktopPx;
    return QSize(px, px);
}

int minSide(int desktopPx)
{
    return g_enabled ? qMax(desktopPx, kTarget) : desktopPx;
}

int minFieldHeight(int desktopPx)
{
    return g_enabled ? qMax(desktopPx, kField) : desktopPx;
}

QString styleSheet()
{
    return g_enabled ? QString::fromLatin1(kSheet) : QString();
}

QStyle *wrap(QStyle *base)
{
    // The proxy owns its base from here.
    return g_enabled ? new TouchStyle(base) : base;
}

void fitToScreen(QDialog *dlg)
{
    if (!dlg || !dlg->isWindow())
        return;
    const QRect avail = screenRect(dlg);
    if (avail.isEmpty())
        return;
    // Room for a window frame around a dialog that has one.
    const int frame = (dlg->windowFlags() & Qt::FramelessWindowHint) ? 0 : 40;
    const QSize room = QSize(avail.width(), avail.height() - frame).expandedTo(QSize(200, 150));

    // Contents that cannot shrink to the screen go into a scroll area, once.
    // File dialogs and message boxes look after their own layout.
    QLayout *lay = dlg->layout();
    if (lay && !dlg->property("phobicScrollFit").toBool() && !qobject_cast<QFileDialog *>(dlg)
        && !qobject_cast<QMessageBox *>(dlg)) {
        const QSize need = lay->totalMinimumSize();
        if (need.height() > room.height() || need.width() > room.width()) {
            dlg->setProperty("phobicScrollFit", true);
            // The OK / Cancel row stays outside, always in reach.
            QDialogButtonBox *buttons = nullptr;
            if (lay->count() > 0)
                if (QLayoutItem *last = lay->itemAt(lay->count() - 1))
                    buttons = qobject_cast<QDialogButtonBox *>(last->widget());
            if (buttons)
                lay->removeWidget(buttons);
            const QMargins margins = lay->contentsMargins();
            if (lay->sizeConstraint() == QLayout::SetFixedSize
                || lay->sizeConstraint() == QLayout::SetMinimumSize)
                lay->setSizeConstraint(QLayout::SetDefaultConstraint);
            auto *body = new QWidget;
            body->setLayout(lay);                   // takes the layout off the dialog
            lay->setContentsMargins(0, 0, 0, 0);
            auto *area = new QScrollArea;
            area->setObjectName(QStringLiteral("phobicFitScroll"));
            area->setWidgetResizable(true);
            area->setFrameShape(QFrame::NoFrame);
            area->setWidget(body);
            auto *outer = new QVBoxLayout(dlg);
            outer->setContentsMargins(margins);
            outer->addWidget(area, 1);
            if (buttons)
                outer->addWidget(buttons);
        }
    }
    // Whatever the code asked for, never bigger than the screen.
    if (dlg->minimumWidth() > room.width() || dlg->minimumHeight() > room.height())
        dlg->setMinimumSize(dlg->minimumSize().boundedTo(room));
    if (dlg->width() > room.width() || dlg->height() > room.height()) {
        dlg->resize(dlg->size().boundedTo(room));
        // Placed for its old size: bring it back onto the screen.
        QRect g = dlg->frameGeometry();
        g.moveLeft(qBound(avail.left(), g.left(), qMax(avail.left(), avail.right() - g.width() + 1)));
        g.moveTop(qBound(avail.top(), g.top(), qMax(avail.top(), avail.bottom() - g.height() + 1)));
        dlg->move(g.topLeft());
    }
}

void install(QApplication &app)
{
    app.installEventFilter(new Watcher(&app));
}

} // namespace touch
} // namespace c2d
