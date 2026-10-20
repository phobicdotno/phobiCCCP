#include "shopdialog.h"

#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace c2d {

ShopAnswer shopChoose(QWidget *parent, const ShopAsk &a)
{
    // No window-manager title bar: the caption inside is the title, and the
    // card gets an amber edge instead so it still reads as a popup.
    QDialog dlg(parent, Qt::Dialog | Qt::FramelessWindowHint);
    dlg.setWindowTitle(a.title);
    dlg.setObjectName(QStringLiteral("shopDialog"));
    dlg.setAttribute(Qt::WA_StyledBackground);
    dlg.setStyleSheet(QStringLiteral(
        "#shopDialog { background: #1e2127; border: 2px solid #f2a536; border-radius: 10px; }"
        "QLabel { color: #e6e9ef; }"
        "QLabel#caption { color: #8b93a1; font-size: 11px; letter-spacing: 1px; }"
        "QLabel#headline { font-size: 20px; font-weight: bold; }"
        "QLabel#body { color: #c5cad3; font-size: 15px; }"
        "QLabel#note { font-size: 15px; border-radius: 10px; padding: 10px 14px; "
        "  background: #4a3413; border: 1px solid #f2a536; color: #f6d7a0; }"
        "QLabel#noteDanger { font-size: 15px; font-weight: bold; border-radius: 10px; "
        "  padding: 10px 14px; background: #3a1f1f; border: 1px solid #c94040; color: #ffb3b3; }"
        "QPushButton { min-height: 56px; min-width: 150px; padding: 0 22px; font-size: 16px; "
        "  border-radius: 12px; }"
        "QPushButton#ok { font-weight: bold; background: #f2a536; color: #1a1a1a; border: 0; }"
        "QPushButton#ok:pressed { background: #d48f25; }"
        "QPushButton#okDanger { font-weight: bold; background: #7a2626; color: #ffffff; border: 0; }"
        "QPushButton#okDanger:pressed { background: #5e1d1d; }"
        "QPushButton#cancel { background: #2a2e36; color: #e6e9ef; border: 1px solid #3a404b; }"
        "QPushButton#cancel:pressed { background: #3a4150; }"));

    // Word-wrapped labels only get the right height when their width is
    // known up front; left to the layout they were cut off at the bottom.
    constexpr int kWidth = 640, kMargin = 28;
    const int textW = kWidth - 2 * kMargin;
    auto *v = new QVBoxLayout(&dlg);
    v->setContentsMargins(kMargin, 22, kMargin, 22);
    v->setSizeConstraint(QLayout::SetFixedSize);
    v->setSpacing(14);
    auto *cap = new QLabel(a.title.toUpper(), &dlg);
    cap->setObjectName(QStringLiteral("caption"));
    v->addWidget(cap);
    // Headline and body; with a picture they sit in a column beside it.
    const int picW = a.picture.isNull() ? 0 : int(a.picture.width() / a.picture.devicePixelRatio());
    const int colW = picW > 0 ? textW - picW - 18 : textW;
    auto *col = new QVBoxLayout;
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(14);
    if (!a.headline.isEmpty()) {
        auto *h = new QLabel(a.headline, &dlg);
        h->setObjectName(QStringLiteral("headline"));
        h->setWordWrap(true);
        h->setFixedWidth(colW);
        col->addWidget(h);
    }
    if (!a.body.isEmpty()) {
        auto *b = new QLabel(a.body, &dlg);
        b->setObjectName(QStringLiteral("body"));
        b->setWordWrap(true);
        b->setTextFormat(Qt::PlainText);
        b->setFixedWidth(colW);
        col->addWidget(b);
    }
    col->addStretch(1);
    if (picW > 0) {
        auto *hb = new QHBoxLayout;
        hb->setContentsMargins(0, 0, 0, 0);
        hb->setSpacing(18);
        hb->addLayout(col, 1);
        auto *pic = new QLabel(&dlg);
        pic->setPixmap(a.picture);
        pic->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
        hb->addWidget(pic, 0, Qt::AlignTop);
        v->addLayout(hb);
    } else {
        v->addLayout(col);
    }
    if (!a.note.isEmpty()) {
        auto *n = new QLabel(a.note, &dlg);
        n->setObjectName(a.noteDanger ? QStringLiteral("noteDanger") : QStringLiteral("note"));
        n->setWordWrap(true);
        n->setTextFormat(Qt::PlainText);
        n->setFixedWidth(textW);
        v->addWidget(n);
    }
    v->addSpacing(6);
    auto *row = new QHBoxLayout;
    row->setSpacing(12);
    row->addStretch(1);
    QPushButton *cancel = nullptr;
    if (!a.cancelText.isEmpty()) {
        cancel = new QPushButton(a.cancelText, &dlg);
        cancel->setObjectName(QStringLiteral("cancel"));
        QObject::connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
        row->addWidget(cancel);
    }
    constexpr int kAlt = 2;
    if (!a.altText.isEmpty()) {
        auto *alt = new QPushButton(a.altText, &dlg);
        alt->setObjectName(QStringLiteral("cancel"));   // the quiet look
        QObject::connect(alt, &QPushButton::clicked, &dlg, [&dlg] { dlg.done(kAlt); });
        row->addWidget(alt);
    }
    auto *ok = new QPushButton(a.okText, &dlg);
    ok->setObjectName(a.okDanger ? QStringLiteral("okDanger") : QStringLiteral("ok"));
    QObject::connect(ok, &QPushButton::clicked, &dlg, &QDialog::accept);
    row->addWidget(ok);
    v->addLayout(row);

    if (a.defaultOk || !cancel)
        ok->setDefault(true);
    else
        cancel->setDefault(true);
    (a.defaultOk || !cancel ? ok : cancel)->setFocus();
    const int r = dlg.exec();
    return r == QDialog::Accepted ? ShopAnswer::Ok
         : r == kAlt              ? ShopAnswer::Alt
                                  : ShopAnswer::Cancel;
}

bool shopAsk(QWidget *parent, const ShopAsk &a)
{
    return shopChoose(parent, a) == ShopAnswer::Ok;
}

void shopTell(QWidget *parent, const QString &title, const QString &headline,
              const QString &body)
{
    ShopAsk a;
    a.title = title;
    a.headline = headline;
    a.body = body;
    a.cancelText.clear();
    shopAsk(parent, a);
}

} // namespace c2d
