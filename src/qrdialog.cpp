#include "qrdialog.h"
#include "qrcode.h"
#include "canvas.h"
#include "c2ddocument.h"
#include "element.h"
#include "vectorops.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>
#include <algorithm>

namespace c2d {

bool runQrDialog(QWidget *parent, Canvas *canvas)
{
    if (!canvas || !canvas->document())
        return false;
    QSettings st;
    QDialog dlg(parent);
    dlg.setWindowTitle(QStringLiteral("QR code"));
    auto *lay = new QVBoxLayout(&dlg);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    auto *text = new QLineEdit(st.value(QStringLiteral("qr/text")).toString(), &dlg);
    text->setPlaceholderText(QStringLiteral("https://example.com"));
    text->setMinimumHeight(44);
    auto *size = new QDoubleSpinBox(&dlg);
    size->setRange(5, 2000);
    size->setDecimals(1);
    size->setSuffix(QStringLiteral(" mm"));
    size->setValue(st.value(QStringLiteral("qr/size"), 50.0).toDouble());
    size->setMinimumHeight(44);
    auto *ecc = new QComboBox(&dlg);
    ecc->addItems({QStringLiteral("L: low (7% can be damaged)"),
                   QStringLiteral("M: medium (15%)"),
                   QStringLiteral("Q: quartile (25%)"),
                   QStringLiteral("H: high (30%)")});
    ecc->setCurrentIndex(std::clamp(st.value(QStringLiteral("qr/ecc"), 1).toInt(), 0, 3));
    ecc->setMinimumHeight(44);
    auto *quiet = new QSpinBox(&dlg);
    quiet->setRange(0, 10);
    quiet->setSuffix(QStringLiteral(" squares"));
    quiet->setValue(st.value(QStringLiteral("qr/quiet"), 2).toInt());
    quiet->setMinimumHeight(44);
    auto *merge = new QCheckBox(QStringLiteral("Merge dark squares into outlines"), &dlg);
    merge->setChecked(st.value(QStringLiteral("qr/merge"), true).toBool());
    merge->setMinimumHeight(44);
    auto *border = new QCheckBox(QStringLiteral("Square border around the code"), &dlg);
    border->setChecked(st.value(QStringLiteral("qr/border"), false).toBool());
    border->setMinimumHeight(44);

    size->setToolTip(QStringLiteral("The whole code across, light margin included"));
    ecc->setToolTip(QStringLiteral("Higher reads better when scratched or dirty, but makes "
                                   "more and smaller squares"));
    quiet->setToolTip(QStringLiteral("Light margin on every side, in code squares. "
                                     "Phones read best with 2 or more"));
    merge->setToolTip(QStringLiteral("On: touching squares become one shape, so a Pocket cuts "
                                     "them in one go. Off: one small square per dark spot"));
    border->setToolTip(QStringLiteral("Adds the outer square as its own vector, for a "
                                      "Contour that cuts the tile out"));

    form->addRow(QStringLiteral("Text or web address"), text);
    form->addRow(QStringLiteral("Size"), size);
    form->addRow(QStringLiteral("Error correction"), ecc);
    form->addRow(QStringLiteral("Margin"), quiet);
    form->addRow(QString(), merge);
    form->addRow(QString(), border);
    lay->addLayout(form);

    auto *summary = new QLabel(&dlg);
    summary->setWordWrap(true);
    lay->addWidget(summary);
    auto *tip = new QLabel(QStringLiteral(
        "Tip: no toolpaths are added. Pocket the dark squares with a bit no wider than "
        "one square (or a V-bit), or pocket around them for a raised code. "
        "Scan it with a phone before you cut."), &dlg);
    tip->setWordWrap(true);
    lay->addWidget(tip);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Draw the code"));
    buttons->button(QDialogButtonBox::Ok)->setMinimumHeight(44);
    buttons->button(QDialogButtonBox::Cancel)->setMinimumHeight(44);
    lay->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    auto encode = [&] { return qrEncode(text->text().toUtf8(), QrEcc(ecc->currentIndex())); };
    auto refresh = [&] {
        const QrCode c = encode();
        if (c.version == 0) {
            summary->setText(c.error);
            buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
            return;
        }
        buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
        const double m = size->value() / (c.size + 2 * quiet->value());
        QString s = QStringLiteral("%1 × %1 squares (version %2), each %3 mm.")
                        .arg(c.size).arg(c.version).arg(m, 0, 'f', 2);
        if (m < 1.0)
            s += QStringLiteral(" Small squares: make the code bigger or the text shorter "
                                "if the bit cannot cut them cleanly.");
        summary->setText(s);
    };
    QObject::connect(text, &QLineEdit::textChanged, &dlg, refresh);
    QObject::connect(size, &QDoubleSpinBox::valueChanged, &dlg, refresh);
    QObject::connect(quiet, &QSpinBox::valueChanged, &dlg, refresh);
    QObject::connect(ecc, &QComboBox::currentIndexChanged, &dlg, refresh);
    refresh();

    if (dlg.exec() != QDialog::Accepted)
        return false;
    const QrCode c = encode();
    if (c.version == 0)
        return false;
    st.setValue(QStringLiteral("qr/text"), text->text());
    st.setValue(QStringLiteral("qr/size"), size->value());
    st.setValue(QStringLiteral("qr/ecc"), ecc->currentIndex());
    st.setValue(QStringLiteral("qr/quiet"), quiet->value());
    st.setValue(QStringLiteral("qr/merge"), merge->isChecked());
    st.setValue(QStringLiteral("qr/border"), border->isChecked());

    const Document *doc = canvas->document();
    const double bw = doc->boardWidth(), bh = doc->boardHeight();
    const QPointF centre = bw > 0 && bh > 0 ? QPointF(bw / 2, bh / 2)
                                            : QPointF(size->value() / 2, size->value() / 2);
    QrDrawing d = qrDrawing(c, size->value(), quiet->value(), merge->isChecked(), centre);
    if (border->isChecked())
        d.rings << d.border;
    canvas->addElements(vec::elementsFromRings(d.rings, doc->defaultLayer()),
                        QStringLiteral("QR code"));
    return true;
}

} // namespace c2d
