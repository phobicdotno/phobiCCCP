#include "tracedialog.h"
#include "canvas.h"
#include "c2ddocument.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QUndoCommand>
#include <QUndoStack>
#include <QVBoxLayout>
#include <QWidget>

namespace c2d {

// ---- preview widget --------------------------------------------------------

// Letterboxed picture with the binary mask (optional) and the traced
// contours drawn on top: outer loops green, holes amber.
class TracePreview : public QWidget
{
public:
    explicit TracePreview(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMinimumSize(360, 300);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
    void setImage(const QImage &img) { m_img = img; m_mask = QImage(); update(); }
    void setMask(const QImage &mask, bool show) { m_mask = mask; m_showMask = show; update(); }
    void setPaths(const QVector<QPainterPath> &outer, const QVector<QPainterPath> &holes,
                  const TraceOptions &o)
    {
        m_outer = outer; m_holes = holes; m_opts = o; update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(0x15, 0x17, 0x1c));
        if (m_img.isNull())
            return;
        const double sx = double(width()) / m_img.width();
        const double sy = double(height()) / m_img.height();
        const double s = qMin(sx, sy);
        const double dw = m_img.width() * s, dh = m_img.height() * s;
        const QRectF target((width() - dw) / 2, (height() - dh) / 2, dw, dh);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.drawImage(target, m_img);
        if (m_showMask && !m_mask.isNull()) {
            p.setOpacity(0.45);
            p.drawImage(target, tinted(m_mask));
            p.setOpacity(1.0);
        }
        // contour mm -> image px -> widget: px = (x - ox)/mmpp,
        // py = H - (y - oy)/mmpp, then the letterbox transform.
        const double mmpp = m_opts.mmPerPixel > 0 ? m_opts.mmPerPixel : 1.0;
        QTransform t;
        t.translate(target.left(), target.top());
        t.scale(s, s);
        t.translate(0, m_img.height());
        t.scale(1.0 / mmpp, -1.0 / mmpp);
        t.translate(-m_opts.origin.x(), -m_opts.origin.y());
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setBrush(Qt::NoBrush);
        p.setTransform(t, true);
        QPen pen(QColor(0x5c, 0xc8, 0x8a));
        pen.setCosmetic(true);
        pen.setWidthF(1.6);
        p.setPen(pen);
        for (const QPainterPath &pp : m_outer)
            p.drawPath(pp);
        pen.setColor(QColor(0xf2, 0xa5, 0x36));
        p.setPen(pen);
        for (const QPainterPath &pp : m_holes)
            p.drawPath(pp);
    }

private:
    static QImage tinted(const QImage &mask)
    {
        QImage t(mask.size(), QImage::Format_ARGB32);
        for (int y = 0; y < mask.height(); ++y) {
            const uchar *s = mask.constScanLine(y);
            QRgb *d = reinterpret_cast<QRgb *>(t.scanLine(y));
            for (int x = 0; x < mask.width(); ++x)
                d[x] = s[x] ? qRgba(0x3b, 0x8c, 0xe6, 255) : qRgba(0, 0, 0, 0);
        }
        return t;
    }
    QImage m_img, m_mask;
    bool m_showMask = false;
    QVector<QPainterPath> m_outer, m_holes;
    TraceOptions m_opts;
};

// ---- undo command --------------------------------------------------------

namespace {
class InsertElementsCmd : public QUndoCommand
{
public:
    InsertElementsCmd(Canvas *c, Document *d, const QVector<Element> &els)
        : m_c(c), m_d(d), m_els(els)
    { setText(QStringLiteral("trace image (%1 paths)").arg(els.size())); }
    void redo() override
    {
        for (const Element &e : m_els) m_d->addElement(e);
        m_c->rebuild();
        emit m_c->documentChanged();
    }
    void undo() override
    {
        for (const Element &e : m_els) m_d->removeElementById(e.id);
        m_c->rebuild();
        emit m_c->documentChanged();
    }
private:
    Canvas *m_c; Document *m_d; QVector<Element> m_els;
};
} // namespace

void TraceDialog::insertUndoable(Canvas *canvas, Document *doc, const QVector<Element> &els)
{
    if (!canvas || !doc || els.isEmpty())
        return;
    canvas->undoStack()->push(new InsertElementsCmd(canvas, doc, els));
}

// ---- dialog ----------------------------------------------------------------

TraceDialog::TraceDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Trace image"));
    resize(980, 640);

    m_preview = new TracePreview(this);

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    // threshold
    auto *thrRow = new QHBoxLayout;
    m_threshold = new QSlider(Qt::Horizontal, this);
    m_threshold->setRange(0, 255);
    m_threshold->setValue(128);
    m_thresholdSpin = new QSpinBox(this);
    m_thresholdSpin->setRange(0, 255);
    m_thresholdSpin->setValue(128);
    thrRow->addWidget(m_threshold, 1);
    thrRow->addWidget(m_thresholdSpin);
    form->addRow(tr("Threshold"), thrRow);
    connect(m_threshold, &QSlider::valueChanged, m_thresholdSpin, &QSpinBox::setValue);
    connect(m_thresholdSpin, &QSpinBox::valueChanged, m_threshold, &QSlider::setValue);
    m_invert = new QCheckBox(tr("Invert (trace light areas)"), this);
    form->addRow(QString(), m_invert);

    m_blur = new QSpinBox(this);
    m_blur->setRange(0, 12);
    m_blur->setSuffix(tr(" px"));
    m_blur->setToolTip(tr("Box blur radius before thresholding — smooths noise and JPEG grain"));
    form->addRow(tr("Blur"), m_blur);

    m_minArea = new QDoubleSpinBox(this);
    m_minArea->setRange(0, 1e6);
    m_minArea->setDecimals(2);
    m_minArea->setSuffix(tr(" mm²"));
    m_minArea->setValue(0.5);
    m_minArea->setToolTip(tr("Despeckle: drop regions and holes smaller than this"));
    form->addRow(tr("Min region area"), m_minArea);

    auto *wRow = new QHBoxLayout;
    m_width = new QDoubleSpinBox(this);
    m_width->setRange(0.1, 10000);
    m_width->setDecimals(2);
    m_width->setSuffix(tr(" mm"));
    m_width->setValue(100);
    m_heightLabel = new QLabel(this);
    wRow->addWidget(m_width, 1);
    wRow->addWidget(m_heightLabel);
    form->addRow(tr("Target width"), wRow);

    auto *tolRow = new QHBoxLayout;
    m_toleranceSlider = new QSlider(Qt::Horizontal, this);
    m_toleranceSlider->setRange(0, 200);          // hundredths of a mm
    m_tolerance = new QDoubleSpinBox(this);
    m_tolerance->setRange(0, 2.0);
    m_tolerance->setDecimals(2);
    m_tolerance->setSingleStep(0.05);
    m_tolerance->setSuffix(tr(" mm"));
    tolRow->addWidget(m_toleranceSlider, 1);
    tolRow->addWidget(m_tolerance);
    form->addRow(tr("Simplify"), tolRow);
    connect(m_toleranceSlider, &QSlider::valueChanged, this, [this](int v) {
        if (!qFuzzyCompare(m_tolerance->value(), v / 100.0))
            m_tolerance->setValue(v / 100.0);
    });
    connect(m_tolerance, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        const int iv = qRound(v * 100);
        if (m_toleranceSlider->value() != iv)
            m_toleranceSlider->setValue(iv);
    });

    auto *smRow = new QHBoxLayout;
    m_smooth = new QCheckBox(tr("Smooth curves, keep corners sharper than"), this);
    m_cornerAngle = new QDoubleSpinBox(this);
    m_cornerAngle->setRange(5, 180);
    m_cornerAngle->setDecimals(0);
    m_cornerAngle->setSuffix(tr("°"));
    m_cornerAngle->setValue(60);
    smRow->addWidget(m_smooth, 1);
    smRow->addWidget(m_cornerAngle);
    form->addRow(tr("Smoothing"), smRow);

    auto *posRow = new QHBoxLayout;
    m_posX = new QDoubleSpinBox(this);
    m_posX->setRange(-10000, 10000);
    m_posX->setDecimals(2);
    m_posX->setPrefix(QStringLiteral("X "));
    m_posX->setSuffix(tr(" mm"));
    m_posY = new QDoubleSpinBox(this);
    m_posY->setRange(-10000, 10000);
    m_posY->setDecimals(2);
    m_posY->setPrefix(QStringLiteral("Y "));
    m_posY->setSuffix(tr(" mm"));
    posRow->addWidget(m_posX);
    posRow->addWidget(m_posY);
    form->addRow(tr("Insert at (bottom-left)"), posRow);

    m_showMask = new QCheckBox(tr("Show ink mask"), this);
    form->addRow(QString(), m_showMask);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    form->addRow(m_status);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Insert paths"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *side = new QVBoxLayout;
    side->addLayout(form);
    side->addStretch(1);
    side->addWidget(buttons);
    auto *sideW = new QWidget(this);
    sideW->setLayout(side);
    sideW->setFixedWidth(380);

    auto *main = new QHBoxLayout(this);
    main->addWidget(m_preview, 1);
    main->addWidget(sideW);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(120);
    connect(m_debounce, &QTimer::timeout, this, &TraceDialog::runTrace);

    connect(m_threshold, &QSlider::valueChanged, this, &TraceDialog::scheduleTrace);
    connect(m_invert, &QCheckBox::toggled, this, &TraceDialog::scheduleTrace);
    connect(m_blur, &QSpinBox::valueChanged, this, &TraceDialog::scheduleTrace);
    connect(m_minArea, &QDoubleSpinBox::valueChanged, this, &TraceDialog::scheduleTrace);
    connect(m_width, &QDoubleSpinBox::valueChanged, this, [this] { updateHeightLabel(); scheduleTrace(); });
    connect(m_tolerance, &QDoubleSpinBox::valueChanged, this, &TraceDialog::scheduleTrace);
    connect(m_smooth, &QCheckBox::toggled, this, &TraceDialog::scheduleTrace);
    connect(m_cornerAngle, &QDoubleSpinBox::valueChanged, this, &TraceDialog::scheduleTrace);
    connect(m_posX, &QDoubleSpinBox::valueChanged, this, &TraceDialog::scheduleTrace);
    connect(m_posY, &QDoubleSpinBox::valueChanged, this, &TraceDialog::scheduleTrace);
    connect(m_showMask, &QCheckBox::toggled, this, &TraceDialog::scheduleTrace);
}

void TraceDialog::setImage(const QImage &img, double widthMm, QPointF origin)
{
    m_image = img;
    m_preview->setImage(img);
    m_width->setValue(widthMm > 0 ? widthMm : 100);
    m_posX->setValue(origin.x());
    m_posY->setValue(origin.y());
    // Sensible default tolerance: about 3/4 of a pixel at the target scale.
    const double mmpp = img.width() > 0 ? m_width->value() / img.width() : 0.1;
    m_tolerance->setValue(qBound(0.0, mmpp * 0.75, 2.0));
    updateHeightLabel();
    runTrace();
}

void TraceDialog::updateHeightLabel()
{
    if (m_image.width() <= 0) {
        m_heightLabel->clear();
        return;
    }
    const double h = m_width->value() * m_image.height() / m_image.width();
    m_heightLabel->setText(tr("× %1 mm").arg(h, 0, 'f', 2));
}

TraceOptions TraceDialog::options() const
{
    TraceOptions o;
    o.threshold = m_threshold->value();
    o.invert = m_invert->isChecked();
    o.blurRadius = m_blur->value();
    o.minAreaMm2 = m_minArea->value();
    o.mmPerPixel = m_image.width() > 0 ? m_width->value() / m_image.width() : 0.1;
    o.simplifyTolMm = m_tolerance->value();
    o.smooth = m_smooth->isChecked();
    o.cornerAngleDeg = m_cornerAngle->value();
    o.origin = QPointF(m_posX->value(), m_posY->value());
    return o;
}

void TraceDialog::scheduleTrace()
{
    m_debounce->start();
}

void TraceDialog::runTrace()
{
    if (m_image.isNull())
        return;
    const TraceOptions o = options();
    m_result = traceImage(m_image, o);
    m_preview->setMask(m_showMask->isChecked() ? traceMask(m_image, o) : QImage(),
                       m_showMask->isChecked());
    QVector<QPainterPath> outer, holes;
    int verts = 0;
    for (const TraceContour &c : m_result.contours) {
        (c.hole ? holes : outer).append(traceContourPath(c, o.smooth));
        verts += c.pts.size();
    }
    m_preview->setPaths(outer, holes, o);
    m_status->setText(tr("%1 outline(s), %2 hole(s), %3 vertices — %4 ms  (%5×%6 px)")
                          .arg(outer.size()).arg(holes.size()).arg(verts)
                          .arg(m_result.elapsedMs).arg(m_image.width()).arg(m_image.height()));
}

QVector<Element> TraceDialog::elements(const QJsonObject &layer) const
{
    QVector<Element> out;
    const bool smooth = m_smooth->isChecked();
    for (const TraceContour &c : m_result.contours)
        out.append(traceContourElement(c, smooth, layer));
    return out;
}

} // namespace c2d
