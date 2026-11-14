#include "popuplook.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDialog>
#include <QEvent>
#include <QFileDialog>
#include <QLabel>
#include <QLayout>
#include <QMessageBox>
#include <QToolButton>

namespace c2d {

namespace {

const char *kDone = "phobicPopupLook";

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
        } else if (e->type() == QEvent::Resize) {
            // The X stays in the corner.
            if (auto *dlg = qobject_cast<QDialog *>(o))
                if (auto *x = dlg->findChild<QToolButton *>(QStringLiteral("popupClose"),
                                                            Qt::FindDirectChildrenOnly))
                    x->move(dlg->width() - x->width() - 10, 10);
        }
        return false;
    }
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

    dlg->setWindowFlag(Qt::FramelessWindowHint, true);
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
    if (QLayout *lay = dlg->layout()) {
        int l, t, r, b;
        lay->getContentsMargins(&l, &t, &r, &b);
        lay->setContentsMargins(l + kEdge,
                                t + kEdge + (needsX ? kX + 4 : title.isEmpty() ? 0 : kCaption),
                                r + kEdge, b + kEdge);
        if (!title.isEmpty()) {
            auto *cap = new QLabel(title.toUpper(), dlg);
            cap->setObjectName(QStringLiteral("popupCaption"));
            cap->setAttribute(Qt::WA_TransparentForMouseEvents);
            cap->adjustSize();
            cap->move(l + kEdge + 4, kEdge + 6);
            cap->show();
        }
    }
}

void installPopupLook(QApplication &app)
{
    app.installEventFilter(new PopupLook(&app));
}

} // namespace c2d
