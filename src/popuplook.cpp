#include "popuplook.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDialog>
#include <QEvent>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFontMetrics>
#include <QLabel>
#include <QLayout>
#include <QMessageBox>
#include <QInputDevice>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QToolButton>
#include <QWindow>

namespace c2d {

namespace {

const char *kDone = "phobicPopupLook";
// The height of the strip along a popup's top edge (its caption) that
// moves the popup when dragged: with no title bar it is the only handle.
const char *kDragTop = "phobicPopupDragTop";

// The default button is bold, but Qt sizes the buttons from the plain font,
// so a long label ("Close without Saving") was cut off at both ends. Make
// every button wide enough for its label in bold.
void fitButtons(QDialog *dlg)
{
    if (!dlg->property(kDone).toBool())
        return;
    for (auto *box : dlg->findChildren<QDialogButtonBox *>()) {
        for (QAbstractButton *b : box->buttons()) {
            auto *btn = qobject_cast<QPushButton *>(b);
            if (!btn)
                continue;
            btn->ensurePolished();
            QFont f = btn->font();
            f.setBold(true);
            QString text = btn->text();
            text.remove(QLatin1Char('&'));
            const int need = QFontMetrics(f).horizontalAdvance(text) + 2 * 20 + 16;
            if (btn->minimumWidth() < need)
                btn->setMinimumWidth(need);
        }
    }
}

class PopupLook : public QObject
{
public:
    using QObject::QObject;
    bool eventFilter(QObject *o, QEvent *e) override
    {
        // Polish comes once, before the first show: the window flags can
        // still be changed without the window flashing.
        if (e->type() == QEvent::Polish) {
            if (auto *dlg = qobject_cast<QDialog *>(o))
                applyPopupLook(dlg);
        } else if (e->type() == QEvent::Show) {
            if (auto *dlg = qobject_cast<QDialog *>(o))
                fitButtons(dlg);
        } else if (e->type() == QEvent::MouseButtonPress) {
            auto *dlg = qobject_cast<QDialog *>(o);
            auto *me = static_cast<QMouseEvent *>(e);
            const int top = dlg ? dlg->property(kDragTop).toInt() : 0;
            if (top > 0 && me->button() == Qt::LeftButton && me->position().y() < top) {
                // A mouse asks the window system to move it (Wayland allows
                // nothing else); a finger, or a system that says no, moves
                // it here.
                const QInputDevice *dev = me->device();
                const bool touch = dev && dev->type() == QInputDevice::DeviceType::TouchScreen;
                QWindow *w = dlg->windowHandle();
                if (!touch && w && w->startSystemMove())
                    return true;
                m_drag = dlg;
                m_offset = me->globalPosition().toPoint() - dlg->pos();
                return true;
            }
        } else if (e->type() == QEvent::MouseMove) {
            if (m_drag && o == m_drag) {
                auto *me = static_cast<QMouseEvent *>(e);
                if (me->buttons() & Qt::LeftButton)
                    m_drag->move(me->globalPosition().toPoint() - m_offset);
                else
                    m_drag.clear();
                return true;
            }
        } else if (e->type() == QEvent::MouseButtonRelease) {
            if (m_drag && o == m_drag) {
                m_drag.clear();
                return true;
            }
        } else if (e->type() == QEvent::Resize) {
            // The X stays in the corner.
            if (auto *dlg = qobject_cast<QDialog *>(o))
                if (auto *x = dlg->findChild<QToolButton *>(QStringLiteral("popupClose"),
                                                            Qt::FindDirectChildrenOnly))
                    x->move(dlg->width() - x->width() - 10, 10);
        }
        return false;
    }

private:
    QPointer<QDialog> m_drag;   // the popup being dragged by its caption
    QPoint m_offset;            // from the pointer to the popup's corner
};

} // namespace

void applyPopupLook(QDialog *dlg)
{
    if (!dlg || dlg->property(kDone).toBool())
        return;
    dlg->setProperty(kDone, true);
    if (qobject_cast<QFileDialog *>(dlg))
        return;
    // Already without a title bar: it has its own look (the shop questions).
    if (dlg->windowFlags() & Qt::FramelessWindowHint)
        return;
    // Nothing to press: it gets a big X of its own in the top right corner.
    const bool needsX = dlg->findChildren<QAbstractButton *>().isEmpty();

    // Not setWindowFlag(): that re-parents the dialog in the middle of show(),
    // and on Qt 6.4 (the CNC PC) the dialog then never hides again, so its
    // buttons and the X did nothing. Set the flags directly, and on the
    // native window too when Qt has already made it.
    dlg->overrideWindowFlags(dlg->windowFlags() | Qt::FramelessWindowHint);
    if (QWindow *w = dlg->windowHandle())
        w->setFlags(dlg->windowFlags());
    dlg->setAttribute(Qt::WA_StyledBackground);
    if (auto *box = qobject_cast<QMessageBox *>(dlg))
        box->setIcon(QMessageBox::NoIcon);

    // Ours first, so a dialog's own rules still win where they say something.
    dlg->setStyleSheet(QStringLiteral(
        "QDialog { background: #1e2127; border: 2px solid #f2a536; border-radius: 10px; }"
        "QToolButton#popupClose { background: #2a2e36; color: #e6e9ef; border: 1px solid #3a404b; "
        "  border-radius: 12px; font-size: 22px; font-weight: bold; }"
        "QToolButton#popupClose:pressed { background: #3a4150; }"
        "QLabel#popupCaption { color: #8b93a1; font-size: 11px; letter-spacing: 1px; "
        "  background: transparent; border: 0; padding: 0; }"
        "QMessageBox QLabel#qt_msgbox_label { color: #e6e9ef; font-size: 16px; }"
        "QMessageBox QLabel#qt_msgbox_informativelabel { color: #c5cad3; font-size: 15px; }"
        "QInputDialog QLabel { color: #e6e9ef; font-size: 16px; }"
        "QInputDialog QLineEdit, QInputDialog QAbstractSpinBox, QInputDialog QComboBox "
        "  { min-height: 40px; font-size: 16px; }"
        "QDialogButtonBox QPushButton { min-height: 48px; min-width: 120px; padding: 0 20px; "
        "  font-size: 16px; border-radius: 12px; background: #2a2e36; color: #e6e9ef; "
        "  border: 1px solid #3a404b; }"
        "QDialogButtonBox QPushButton:pressed { background: #3a4150; }"
        "QDialogButtonBox QPushButton:disabled { color: #6b7380; }"
        "QDialogButtonBox QPushButton:default { font-weight: bold; background: #f2a536; "
        "  color: #1a1a1a; border: 0; }"
        "QDialogButtonBox QPushButton:default:pressed { background: #d48f25; }")
        + dlg->styleSheet());

    // Room for the edge, and for the caption that stands in for the title.
    const QString title = dlg->windowTitle().trimmed();
    constexpr int kEdge = 10, kCaption = 26, kX = 48;
    if (needsX) {
        auto *x = new QToolButton(dlg);
        x->setObjectName(QStringLiteral("popupClose"));
        x->setText(QStringLiteral("\u2715"));
        x->setToolTip(QStringLiteral("Close"));
        x->setFixedSize(kX, kX);
        x->setFocusPolicy(Qt::NoFocus);
        QObject::connect(x, &QToolButton::clicked, dlg, &QDialog::close);
        x->move(dlg->width() - kX - kEdge, kEdge);
        x->show();
        x->raise();
        dlg->setMinimumSize(dlg->minimumSize().expandedTo(QSize(kX + 4 * kEdge, kX + 2 * kEdge)));
    }
    // The caption strip (or the bare edge) moves the popup: one that shows
    // its work on the canvas behind it (Adjust background, Trace) can be
    // dragged out of the way. Only where the dialog itself is under the
    // pointer: the X and anything else in the strip still take their press.
    dlg->setProperty(kDragTop, kEdge);
    if (QLayout *lay = dlg->layout()) {
        int l, t, r, b;
        lay->getContentsMargins(&l, &t, &r, &b);
        const int strip = kEdge + (needsX ? kX + 4 : title.isEmpty() ? 0 : kCaption);
        lay->setContentsMargins(l + kEdge, t + strip, r + kEdge, b + kEdge);
        dlg->setProperty(kDragTop, t + strip);
        if (!title.isEmpty()) {
            auto *cap = new QLabel(title.toUpper(), dlg);
            cap->setObjectName(QStringLiteral("popupCaption"));
            cap->setAttribute(Qt::WA_TransparentForMouseEvents);
            cap->adjustSize();
            cap->move(l + kEdge + 4, kEdge + 6);
            cap->show();
        }
    }
    // Polish comes before Qt sizes the dialog, so the wider buttons count.
    fitButtons(dlg);
}

void installPopupLook(QApplication &app)
{
    app.installEventFilter(new PopupLook(&app));
}

} // namespace c2d
