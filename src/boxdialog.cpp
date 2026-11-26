#include "boxdialog.h"
#include "canvas.h"
#include "c2ddocument.h"
#include "element.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>

namespace c2d {

namespace {

QDoubleSpinBox *mmBox(QWidget *parent, double lo, double hi, double value, int decimals = 2)
{
    auto *b = new QDoubleSpinBox(parent);
    b->setRange(lo, hi);
    b->setDecimals(decimals);
    b->setSuffix(QStringLiteral(" mm"));
    b->setValue(value);
    b->setMinimumHeight(44);
    return b;
}

} // namespace

bool runBoxDialog(QWidget *parent, Canvas *canvas, double stockThickness, double stockWidth)
{
    if (!canvas || !canvas->document())
        return false;
    QDialog dlg(parent);
    dlg.setWindowTitle(QStringLiteral("Box with finger joints"));
    auto *lay = new QVBoxLayout(&dlg);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    const double t0 = stockThickness > 0 ? stockThickness : 6.0;
    auto *sizeIs = new QComboBox(&dlg);
    sizeIs->addItems({QStringLiteral("Outside of the box"), QStringLiteral("Inside of the box")});
    sizeIs->setMinimumHeight(44);
    auto *w = mmBox(&dlg, 1, 5000, 150);
    auto *d = mmBox(&dlg, 1, 5000, 100);
    auto *h = mmBox(&dlg, 1, 5000, 60);
    auto *t = mmBox(&dlg, 0.5, 200, t0);
    auto *f = mmBox(&dlg, 1, 1000, boxDefaultFinger(t0), 1);
    auto *bit = mmBox(&dlg, 0.1, 50, 3.175, 3);
    auto *gap = mmBox(&dlg, 0, 2, 0.1, 2);
    auto *lid = new QCheckBox(QStringLiteral("Lid (a sixth part, fingered all round)"), &dlg);
    lid->setMinimumHeight(44);

    t->setToolTip(QStringLiteral("Starts at the stock thickness of this design"));
    f->setToolTip(QStringLiteral("Each joint rounds it to an odd number of fingers, "
                                 "so both ends of a joint look the same"));
    bit->setToolTip(QStringLiteral("The bit that cuts the parts out: they are laid out "
                                   "at least two bit widths apart"));
    gap->setToolTip(QStringLiteral("Taken off every finger so the joints go together "
                                   "without a mallet. 0.1 mm is a snug glue fit"));

    form->addRow(QStringLiteral("Sizes are"), sizeIs);
    form->addRow(QStringLiteral("Width (X)"), w);
    form->addRow(QStringLiteral("Depth (Y)"), d);
    form->addRow(QStringLiteral("Height"), h);
    form->addRow(QStringLiteral("Material thickness"), t);
    form->addRow(QStringLiteral("Finger width"), f);
    form->addRow(QStringLiteral("Bit diameter"), bit);
    form->addRow(QStringLiteral("Fit gap"), gap);
    form->addRow(QString(), lid);
    lay->addLayout(form);

    auto *summary = new QLabel(&dlg);
    summary->setWordWrap(true);
    lay->addWidget(summary);
    auto *tip = new QLabel(QStringLiteral(
        "Tip: no toolpaths are added. Cut the parts with a Contour toolpath, Side: "
        "Outside, full material depth, and set Corners: Dogbone on it: the "
        "round bit leaves the inside corners of the slots rounded, and the dogbones "
        "let the fingers seat all the way."), &dlg);
    tip->setWordWrap(true);
    lay->addWidget(tip);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Draw the parts"));
    lay->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    auto spec = [&] {
        BoxSpec s;
        s.width = w->value(); s.depth = d->value(); s.height = h->value();
        s.insideSize = sizeIs->currentIndex() == 1;
        s.thickness = t->value(); s.fingerWidth = f->value();
        s.bitDiameter = bit->value(); s.clearance = gap->value();
        s.lid = lid->isChecked(); s.stockWidth = stockWidth;
        return s;
    };
    bool fingerTouched = false;
    QObject::connect(f, &QDoubleSpinBox::valueChanged, &dlg, [&] { fingerTouched = true; });
    QObject::connect(t, &QDoubleSpinBox::valueChanged, &dlg, [&](double v) {
        if (fingerTouched) return;
        const QSignalBlocker block(f);
        f->setValue(boxDefaultFinger(v));
    });
    auto refresh = [&] {
        const BoxSpec s = spec();
        const BoxResult r = makeFingerBox(s);
        if (!r.error.isEmpty()) {
            summary->setText(r.error);
            buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
            return;
        }
        buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
        const double th = s.thickness;
        summary->setText(QStringLiteral(
            "%1 parts. Outside %2 × %3 × %4 mm. Fingers per joint: %5 along the width, "
            "%6 along the depth, %7 up the corners.")
            .arg(r.panels.size())
            .arg(r.outerW, 0, 'f', 1).arg(r.outerD, 0, 'f', 1).arg(r.outerH, 0, 'f', 1)
            .arg(boxFingerCount(r.outerW - 2 * th, s.fingerWidth))
            .arg(boxFingerCount(r.outerD - 2 * th, s.fingerWidth))
            .arg(boxFingerCount(r.outerH - 2 * th, s.fingerWidth)));
    };
    for (QDoubleSpinBox *b : {w, d, h, t, f, bit, gap})
        QObject::connect(b, &QDoubleSpinBox::valueChanged, &dlg, refresh);
    QObject::connect(sizeIs, &QComboBox::currentIndexChanged, &dlg, refresh);
    QObject::connect(lid, &QCheckBox::toggled, &dlg, refresh);
    refresh();

    if (dlg.exec() != QDialog::Accepted)
        return false;
    const BoxResult r = makeFingerBox(spec());
    if (!r.error.isEmpty())
        return false;
    const QJsonObject layer = canvas->document()->defaultLayer();
    QVector<Element> els;
    for (const BoxPanel &p : r.panels)
        els << Element::makePath(p.outline, true, layer);
    canvas->addElements(els, QStringLiteral("box with finger joints"));
    return true;
}

} // namespace c2d
