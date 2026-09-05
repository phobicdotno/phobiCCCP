#include "modelpanel.h"
#include "backgroundimage.h"
#include "c2ddocument.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtMath>

#include <atomic>
#include <cmath>
#include <functional>

namespace c2d {

namespace {
const QColor kViewBg(0x15, 0x17, 0x1c);
const QColor kBoardEdge(0x4a, 0x52, 0x60);
const QColor kText(0xd8, 0xdc, 0xe4);
const QString kImageFilter = QStringLiteral("Images (*.png *.jpg *.jpeg *.bmp *.tif *.tiff);;All files (*)");
}

// ---------------------------------------------------------------------------
// ModelJob: composites on a worker thread.

class ModelJob : public QThread
{
    Q_OBJECT
public:
    explicit ModelJob(QObject *parent) : QThread(parent) {}
    Model3D model;
    QVector<Element> elements;
    double boardW = 0, boardH = 0;
    std::atomic<bool> cancel{false};
    HeightModel result;

signals:
    void progress(int percent);

protected:
    void run() override
    {
        result = buildHeightModel(model, elements, boardW, boardH,
                                  [this](int p) { emit progress(p); }, &cancel);
    }
};

// ---------------------------------------------------------------------------
// ModelView: zoomable shaded relief (same interaction as the Simulation tab).

class ModelView : public QWidget
{
public:
    using HoverFn = std::function<void(int c, int r, bool valid)>;

    explicit ModelView(QWidget *parent) : QWidget(parent)
    {
        setMinimumSize(200, 160);
        setMouseTracking(true);
        setCursor(Qt::CrossCursor);
    }
    void setImage(const QImage &img)
    {
        if (m_img.isNull() || img.size() != m_img.size())
            m_fitPending = true;
        m_img = img;
        update();
    }
    void setHoverHandler(HoverFn fn) { m_hover = std::move(fn); }
    void setCaption(const QString &c) { m_caption = c; update(); }
    void setEmptyText(const QString &t) { m_empty = t; update(); }
    void resetView() { m_fitPending = true; update(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), kViewBg);
        if (m_img.isNull()) {
            p.setPen(kText);
            p.drawText(rect().adjusted(12, 12, -12, -12), Qt::AlignCenter | Qt::TextWordWrap, m_empty);
            return;
        }
        if (m_fitPending)
            fit();
        p.save();
        p.translate(m_off);
        p.scale(m_scale, m_scale);
        p.setRenderHint(QPainter::SmoothPixmapTransform, m_scale < 1.0);
        p.drawImage(QPointF(0, 0), m_img);
        p.restore();
        p.setPen(QPen(kBoardEdge, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(m_off, QSizeF(m_img.width() * m_scale, m_img.height() * m_scale)));
        p.setPen(kText);
        QFont f = p.font(); f.setPointSizeF(f.pointSizeF() * 0.9); p.setFont(f);
        p.drawText(QRectF(8, 6, width() - 16, 18), Qt::AlignLeft | Qt::AlignVCenter, m_caption);
        QColor hint = kText; hint.setAlpha(110); p.setPen(hint);
        p.drawText(QRectF(8, height() - 22, width() - 16, 18), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("drag: pan   wheel: zoom   double-click: fit"));
    }
    void wheelEvent(QWheelEvent *e) override
    {
        if (m_img.isNull()) return;
        const double steps = e->angleDelta().y() / 120.0;
        if (steps == 0) return;
        const double f = qPow(1.25, steps);
        const QPointF pos = e->position();
        m_off = pos - f * (pos - m_off);
        m_scale *= f;
        m_fitPending = false;
        update();
        report(pos);
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        m_last = e->pos();
        m_drag = true;
        setCursor(Qt::ClosedHandCursor);
    }
    void mouseReleaseEvent(QMouseEvent *) override
    {
        m_drag = false;
        setCursor(Qt::CrossCursor);
    }
    void mouseDoubleClickEvent(QMouseEvent *) override { resetView(); }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (m_drag && e->buttons()) {
            m_off += QPointF(e->pos() - m_last);
            m_last = e->pos();
            m_fitPending = false;
            update();
        }
        report(e->position());
    }
    void leaveEvent(QEvent *) override
    {
        if (m_hover) m_hover(0, 0, false);
    }
    void resizeEvent(QResizeEvent *) override
    {
        if (m_fitPending) update();
    }

private:
    void fit()
    {
        m_fitPending = false;
        if (m_img.isNull()) return;
        const double sx = (width() - 24.0) / m_img.width();
        const double sy = (height() - 52.0) / m_img.height();
        m_scale = qMin(sx, sy);
        if (!(m_scale > 0) || !std::isfinite(m_scale)) m_scale = 1;
        m_off = QPointF((width() - m_img.width() * m_scale) / 2.0,
                        (height() - m_img.height() * m_scale) / 2.0 + 4);
    }
    void report(const QPointF &pos)
    {
        if (!m_hover) return;
        if (m_img.isNull()) { m_hover(0, 0, false); return; }
        const QPointF ip = (pos - m_off) / m_scale;
        const int px = int(std::floor(ip.x())), py = int(std::floor(ip.y()));
        if (px < 0 || py < 0 || px >= m_img.width() || py >= m_img.height()) {
            m_hover(0, 0, false);
            return;
        }
        m_hover(px, m_img.height() - 1 - py, true);   // image row 0 = top = max Y
    }

    QImage m_img;
    double m_scale = 1;
    QPointF m_off;
    QPoint m_last;
    bool m_drag = false, m_fitPending = true;
    QString m_caption, m_empty = QStringLiteral("No 3D model — add a component");
    HoverFn m_hover;
};

// ---------------------------------------------------------------------------
// STL placement dialog

namespace {
struct StlChoice {
    ModelComponent::Units units = ModelComponent::Millimetres;
    bool fitWidth = false;
    double width = 0, x = 0, y = 0, height = 0;
};

bool askStlPlacement(QWidget *parent, const QString &name, double bw, double bd, double bz,
                     double boardW, double boardH, StlChoice *out)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(QStringLiteral("Import STL — %1").arg(name));
    auto *form = new QFormLayout(&dlg);
    auto *info = new QLabel(QStringLiteral("Mesh extent: %1 × %2 × %3 (file units)")
                                .arg(bw, 0, 'g', 5).arg(bd, 0, 'g', 5).arg(bz, 0, 'g', 5), &dlg);
    form->addRow(info);
    auto *units = new QComboBox(&dlg);
    units->addItem(QStringLiteral("Millimetres"), int(ModelComponent::Millimetres));
    units->addItem(QStringLiteral("Inches"), int(ModelComponent::Inches));
    form->addRow(QStringLiteral("File units"), units);
    auto *fit = new QCheckBox(QStringLiteral("Fit to width"), &dlg);
    auto *width = new QDoubleSpinBox(&dlg);
    width->setRange(0.1, 10000); width->setDecimals(2); width->setSuffix(QStringLiteral(" mm"));
    // default: natural size when it fits, else fit to 80 % of the board
    const double natural = bw;
    const bool tooBig = natural > boardW || bd > boardH;
    fit->setChecked(tooBig);
    width->setValue(tooBig ? boardW * 0.8 : natural);
    width->setEnabled(tooBig);
    QObject::connect(fit, &QCheckBox::toggled, width, &QWidget::setEnabled);
    auto *fitRow = new QHBoxLayout;
    fitRow->addWidget(fit);
    fitRow->addWidget(width, 1);
    form->addRow(QStringLiteral("Size"), fitRow);
    auto *x = new QDoubleSpinBox(&dlg);
    x->setRange(-10000, 10000); x->setDecimals(2); x->setSuffix(QStringLiteral(" mm"));
    auto *y = new QDoubleSpinBox(&dlg);
    y->setRange(-10000, 10000); y->setDecimals(2); y->setSuffix(QStringLiteral(" mm"));
    auto place = [&] {
        const double s = units->currentData().toInt() == int(ModelComponent::Inches) ? 25.4 : 1.0;
        const double w = fit->isChecked() ? width->value() : bw * s;
        const double h = w * bd / bw;
        x->setValue((boardW - w) / 2);
        y->setValue((boardH - h) / 2);
    };
    place();
    QObject::connect(fit, &QCheckBox::toggled, &dlg, place);
    QObject::connect(width, qOverload<double>(&QDoubleSpinBox::valueChanged), &dlg, place);
    QObject::connect(units, qOverload<int>(&QComboBox::currentIndexChanged), &dlg, place);
    form->addRow(QStringLiteral("Position X (left)"), x);
    form->addRow(QStringLiteral("Position Y (bottom)"), y);
    auto *height = new QDoubleSpinBox(&dlg);
    height->setRange(0, 1000); height->setDecimals(2); height->setSuffix(QStringLiteral(" mm"));
    height->setSpecialValueText(QStringLiteral("natural (scaled Z)"));
    height->setValue(0);
    form->addRow(QStringLiteral("Model height"), height);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(bb);
    if (dlg.exec() != QDialog::Accepted)
        return false;
    out->units = ModelComponent::Units(units->currentData().toInt());
    out->fitWidth = fit->isChecked();
    out->width = width->value();
    out->x = x->value();
    out->y = y->value();
    out->height = height->value();
    return true;
}
}

// ---------------------------------------------------------------------------
// ModelPanel

ModelPanel::ModelPanel(QWidget *parent)
    : QWidget(parent)
{
    installModelProvider();
    m_job = new ModelJob(this);
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(250);
    connect(m_debounce, &QTimer::timeout, this, &ModelPanel::startRebuild);

    // --- add / list actions
    m_addVec = new QToolButton(this);
    m_addVec->setText(QStringLiteral("From vectors"));
    m_addVec->setToolTip(QStringLiteral("Create a relief component from the selected closed vectors"));
    m_addVec->setPopupMode(QToolButton::InstantPopup);
    auto *shapes = new QMenu(m_addVec);
    for (auto s : {ModelComponent::Flat, ModelComponent::Round, ModelComponent::Angle,
                   ModelComponent::Smooth, ModelComponent::Dome})
        shapes->addAction(ModelComponent::shapeName(s), this, [this, s] { addFromVectors(s); });
    m_addVec->setMenu(shapes);
    m_addImg = new QPushButton(QStringLiteral("Image…"), this);
    m_addImg->setToolTip(QStringLiteral("Load a picture as a heightmap (white = high, black = low)"));
    m_addTex = new QPushButton(QStringLiteral("Texture…"), this);
    m_addTex->setToolTip(QStringLiteral("Tile a picture as a texture over the selected vectors (or the whole model)"));
    m_addStl = new QPushButton(QStringLiteral("STL…"), this);
    m_addStl->setToolTip(QStringLiteral("Import a mesh (binary or ASCII STL) rasterised from above"));
    m_remove = new QPushButton(QStringLiteral("Remove"), this);
    m_up = new QPushButton(QStringLiteral("▲"), this);
    m_down = new QPushButton(QStringLiteral("▼"), this);
    for (QPushButton *b : {m_up, m_down})
        b->setMaximumWidth(30);
    connect(m_addImg, &QPushButton::clicked, this, &ModelPanel::addImage);
    connect(m_addTex, &QPushButton::clicked, this, &ModelPanel::addTexture);
    connect(m_addStl, &QPushButton::clicked, this, &ModelPanel::addStl);
    connect(m_remove, &QPushButton::clicked, this, &ModelPanel::removeSelected);
    connect(m_up, &QPushButton::clicked, this, [this] { moveSelected(-1); });
    connect(m_down, &QPushButton::clicked, this, [this] { moveSelected(+1); });

    auto *addRow = new QHBoxLayout;
    addRow->setContentsMargins(6, 4, 6, 0);
    addRow->setSpacing(4);
    addRow->addWidget(m_addVec);
    addRow->addWidget(m_addImg);
    addRow->addWidget(m_addTex);
    addRow->addWidget(m_addStl);
    addRow->addStretch(1);
    addRow->addWidget(m_remove);
    addRow->addWidget(m_up);
    addRow->addWidget(m_down);

    m_list = new QTableWidget(0, 5, this);
    m_list->setHorizontalHeaderLabels({QStringLiteral("On"), QStringLiteral("Name"),
                                       QStringLiteral("Kind"), QStringLiteral("Height"),
                                       QStringLiteral("Combine")});
    m_list->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_list->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_list->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_list->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_list->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_list->verticalHeader()->setVisible(false);
    m_list->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setMaximumHeight(150);
    connect(m_list, &QTableWidget::itemSelectionChanged, this, &ModelPanel::refreshProps);
    connect(m_list, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *it) {
        if (m_updating || it->column() != 0) return;
        const int row = it->row();
        if (row < 0 || row >= m_store.model.components.size()) return;
        m_store.model.components[row].enabled = (it->checkState() == Qt::Checked);
        touch();
    });

    // --- properties
    m_propBox = new QGroupBox(QStringLiteral("Component"), this);
    auto *form = new QFormLayout(m_propBox);
    form->setContentsMargins(8, 4, 8, 6);
    form->setVerticalSpacing(3);
    auto spin = [this](double lo, double hi, double step, const QString &suffix, int dec = 2) {
        auto *s = new QDoubleSpinBox(this);
        s->setRange(lo, hi); s->setSingleStep(step); s->setDecimals(dec); s->setSuffix(suffix);
        connect(s, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &ModelPanel::applyProps);
        return s;
    };
    auto row = [this, form](const QString &label, QWidget *w) {
        auto *l = new QLabel(label, this);
        form->addRow(l, w);
        m_propRows << l << w;
    };
    m_name = new QLineEdit(this);
    connect(m_name, &QLineEdit::editingFinished, this, &ModelPanel::applyProps);
    row(QStringLiteral("Name"), m_name);
    m_shape = new QComboBox(this);
    for (auto s : {ModelComponent::Flat, ModelComponent::Round, ModelComponent::Angle,
                   ModelComponent::Smooth, ModelComponent::Dome})
        m_shape->addItem(ModelComponent::shapeName(s), int(s));
    m_shape->setToolTip(QStringLiteral("Cross-section from the vector's edge inwards: Flat plateau, "
                                       "Round (circular), Angle (straight slope), Smooth (S-curve), Dome (parabola)"));
    connect(m_shape, qOverload<int>(&QComboBox::currentIndexChanged), this, &ModelPanel::applyProps);
    row(QStringLiteral("Shape"), m_shape);
    m_height = spin(-1000, 1000, 0.5, QStringLiteral(" mm"));
    m_height->setToolTip(QStringLiteral("Vectors: peak above the base. Image/texture: amplitude of white. "
                                        "STL: total Z extent (0 = the mesh's own height)"));
    row(QStringLiteral("Height"), m_height);
    m_base = spin(0, 1000, 0.5, QStringLiteral(" mm"));
    m_base->setToolTip(QStringLiteral("Flat base added under the whole component"));
    row(QStringLiteral("Base height"), m_base);
    m_angle = spin(1, 89, 5, QStringLiteral("°"), 1);
    row(QStringLiteral("Angle"), m_angle);
    m_combine = new QComboBox(this);
    for (auto c : {ModelComponent::Add, ModelComponent::Subtract, ModelComponent::Merge, ModelComponent::Multiply})
        m_combine->addItem(ModelComponent::combineName(c), int(c));
    m_combine->setToolTip(QStringLiteral("How this component joins what is below it in the list: "
                                         "Add, Subtract, Merge (keep the higher), Multiply (fade)"));
    connect(m_combine, qOverload<int>(&QComboBox::currentIndexChanged), this, &ModelPanel::applyProps);
    row(QStringLiteral("Combine"), m_combine);
    m_invert = new QCheckBox(QStringLiteral("white = low"), this);
    connect(m_invert, &QCheckBox::toggled, this, &ModelPanel::applyProps);
    row(QStringLiteral("Invert"), m_invert);
    m_blur = spin(0, 50, 0.5, QStringLiteral(" px"), 1);
    row(QStringLiteral("Blur"), m_blur);
    m_x = spin(-10000, 10000, 1, QStringLiteral(" mm"));
    row(QStringLiteral("X (left)"), m_x);
    m_y = spin(-10000, 10000, 1, QStringLiteral(" mm"));
    row(QStringLiteral("Y (bottom)"), m_y);
    m_width = spin(0, 10000, 1, QStringLiteral(" mm"));
    m_width->setSpecialValueText(QStringLiteral("natural"));
    m_width->setToolTip(QStringLiteral("Placed width (height keeps the aspect). 0 = natural: "
                                       "image pixels at 96 dpi, STL file units"));
    row(QStringLiteral("Width"), m_width);
    m_tile = spin(0.1, 1000, 1, QStringLiteral(" mm"));
    row(QStringLiteral("Tile width"), m_tile);
    m_units = new QComboBox(this);
    m_units->addItem(QStringLiteral("Millimetres"), int(ModelComponent::Millimetres));
    m_units->addItem(QStringLiteral("Inches"), int(ModelComponent::Inches));
    connect(m_units, qOverload<int>(&QComboBox::currentIndexChanged), this, &ModelPanel::applyProps);
    row(QStringLiteral("File units"), m_units);
    m_source = new QLabel(this);
    m_source->setTextInteractionFlags(Qt::TextSelectableByMouse);
    row(QStringLiteral("Source"), m_source);

    // --- build controls
    m_res = new QComboBox(this);
    m_res->addItem(QStringLiteral("Auto"), 0.0);
    m_res->addItem(QStringLiteral("Fine (0.1 mm)"), 0.1);
    m_res->addItem(QStringLiteral("Normal (0.25 mm)"), 0.25);
    m_res->addItem(QStringLiteral("Coarse (0.5 mm)"), 0.5);
    m_res->addItem(QStringLiteral("Draft (1 mm)"), 1.0);
    m_res->setToolTip(QStringLiteral("Model cell size. Auto keeps the grid at ≤ 4 M cells (never finer than 0.1 mm)"));
    connect(m_res, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        if (m_updating) return;
        m_store.model.resolution = m_res->currentData().toDouble();
        touch();
    });
    m_rebuild = new QPushButton(QStringLiteral("Rebuild"), this);
    m_cancel = new QPushButton(QStringLiteral("Cancel"), this);
    m_cancel->setEnabled(false);
    connect(m_rebuild, &QPushButton::clicked, this, [this] { m_store.invalidate(); startRebuild(); });
    connect(m_cancel, &QPushButton::clicked, this, &ModelPanel::cancelRebuild);
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setTextVisible(false);
    m_progress->setMaximumHeight(8);
    auto *buildRow = new QHBoxLayout;
    buildRow->setContentsMargins(6, 2, 6, 0);
    buildRow->setSpacing(6);
    buildRow->addWidget(new QLabel(QStringLiteral("Resolution"), this));
    buildRow->addWidget(m_res, 1);
    buildRow->addWidget(m_rebuild);
    buildRow->addWidget(m_cancel);

    m_view = new ModelView(this);
    m_readout = new QLabel(this);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    for (QLabel *l : {m_readout, m_status}) {
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        QFont mono = l->font();
        mono.setFamily(QStringLiteral("monospace"));
        mono.setStyleHint(QFont::Monospace);
        l->setFont(mono);
        l->setContentsMargins(8, 0, 8, 2);
    }
    m_view->setHoverHandler([this](int c, int r, bool valid) {
        const HeightModel &hm = m_store.cached();
        if (!valid || !hm.valid() || c < 0 || r < 0 || c >= hm.cols || r >= hm.rows) {
            m_readout->setText(QStringLiteral("X    —      Y    —      Z    —"));
            return;
        }
        const float z = hm.at(c, r);
        QString zs = z == HeightModel::NoModel
                         ? QStringLiteral("   —   (no model)")
                         : QStringLiteral("%1  (%2 above floor)")
                               .arg(z, 7, 'f', 3).arg(z - hm.baseZ, 0, 'f', 3);
        m_readout->setText(QStringLiteral("X %1  Y %2  Z %3")
                               .arg(hm.originX + (c + 0.5) * hm.cell, 7, 'f', 2)
                               .arg(hm.originY + (r + 0.5) * hm.cell, 7, 'f', 2)
                               .arg(zs));
    });
    m_readout->setText(QStringLiteral("X    —      Y    —      Z    —"));

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(2);
    lay->addLayout(addRow);
    lay->addWidget(m_list);
    lay->addWidget(m_propBox);
    lay->addLayout(buildRow);
    lay->addWidget(m_progress);
    lay->addWidget(m_view, 1);
    lay->addWidget(m_readout);
    lay->addWidget(m_status);

    connect(m_job, &ModelJob::progress, this, [this](int p) { m_progress->setValue(p); });
    connect(m_job, &QThread::finished, this, [this] {
        const bool cancelled = m_job->cancel.load();
        setBusy(false);
        if (!cancelled)
            takeResult(m_job->result);
        if (m_pending) {
            m_pending = false;
            startRebuild();
        } else if (cancelled) {
            updateStatus();
        }
    });

    refreshList();
    refreshProps();
    updateStatus();
}

ModelPanel::~ModelPanel()
{
    m_job->cancel = true;
    m_job->wait();
}

// ---- document plumbing -----------------------------------------------------

void ModelPanel::setDocument(Document *doc)
{
    cancelRebuild();
    m_pending = false;
    m_doc = doc;
    m_store.model = Model3D();
    if (doc && !doc->filePath().isEmpty()) {
        QString err;
        if (!m_store.model.loadFrom(doc->filePath(), &err) && !err.isEmpty())
            m_status->setText(QStringLiteral("Model: %1").arg(err));
    }
    m_store.attach(doc);
    m_store.setHeightModel(HeightModel());
    m_store.invalidate();
    m_updating = true;
    int idx = 0;
    for (int i = 0; i < m_res->count(); ++i)
        if (qFuzzyCompare(1 + m_res->itemData(i).toDouble(), 1 + m_store.model.resolution))
            idx = i;
    m_res->setCurrentIndex(idx);
    m_updating = false;
    m_image = QImage();
    m_view->setImage(m_image);
    refreshList();
    refreshProps();
    updateStatus();
    m_stale = true;
    if (isVisible())
        scheduleRebuild();
}

bool ModelPanel::saveTo(const QString &c2dPath, QString *error) const
{
    return m_store.model.saveTo(c2dPath, error);
}

void ModelPanel::setSelection(const QStringList &ids)
{
    m_selection = ids;
    m_addVec->setEnabled(m_doc && !ids.isEmpty());
}

void ModelPanel::documentChanged()
{
    bool usesVectors = false;
    for (const ModelComponent &c : m_store.model.components)
        if (c.enabled && (c.kind == ModelComponent::FromVectors || c.kind == ModelComponent::Texture))
            usesVectors = true;
    if (!usesVectors)
        return;
    m_store.invalidate();
    m_stale = true;
    if (isVisible())
        scheduleRebuild();
    else
        updateStatus();
}

void ModelPanel::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    if (m_stale && !m_job->isRunning())
        scheduleRebuild();
}

// ---- components -------------------------------------------------------------

QString ModelPanel::uniqueName(const QString &base) const
{
    for (int n = 1;; ++n) {
        const QString cand = QStringLiteral("%1 %2").arg(base).arg(n);
        bool used = false;
        for (const ModelComponent &c : m_store.model.components)
            if (c.name == cand) used = true;
        if (!used)
            return cand;
    }
}

void ModelPanel::appendComponent(ModelComponent c)
{
    if (c.id.isEmpty())
        c.id = Model3D::newId();
    m_store.model.components.append(c);
    refreshList();
    m_list->selectRow(m_store.model.components.size() - 1);
    touch();
}

void ModelPanel::addFromVectors(ModelComponent::Shape shape)
{
    if (!m_doc) return;
    if (m_selection.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("From vectors"),
                                 QStringLiteral("Select one or more closed vectors on the canvas first."));
        return;
    }
    ModelComponent c;
    c.kind = ModelComponent::FromVectors;
    c.shape = shape;
    c.vectorIds = m_selection;
    c.name = uniqueName(ModelComponent::shapeName(shape));
    c.height = 5;
    appendComponent(c);
}

void ModelPanel::addImage()
{
    if (!m_doc) return;
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Load image as heightmap"), {}, kImageFilter);
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("Load image"), f.errorString());
        return;
    }
    const QByteArray bytes = f.readAll();
    QImage img;
    if (!img.loadFromData(bytes)) {
        QMessageBox::warning(this, QStringLiteral("Load image"), QStringLiteral("Could not decode %1").arg(path));
        return;
    }
    ModelComponent c;
    c.kind = ModelComponent::ImageHeightmap;
    c.data = bytes;
    c.sourceName = QFileInfo(path).fileName();
    c.name = uniqueName(QStringLiteral("Image"));
    c.height = 5;
    // Placement like the background image: one pixel = 1/96 in; shrink to
    // 80 % of the board when the picture is bigger than the stock.
    const double bw = m_doc->boardWidth(), bh = m_doc->boardHeight();
    double w = img.width() * BackgroundImage::kMmPerPixel;
    double h = img.height() * BackgroundImage::kMmPerPixel;
    if (w > bw || h > bh) {
        const double s = std::min(bw * 0.8 / w, bh * 0.8 / h);
        w *= s; h *= s;
        c.width = w;
    }
    c.x = (bw - w) / 2;
    c.y = (bh - h) / 2;
    appendComponent(c);
}

void ModelPanel::addTexture()
{
    if (!m_doc) return;
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Apply texture"), {}, kImageFilter);
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("Apply texture"), f.errorString());
        return;
    }
    const QByteArray bytes = f.readAll();
    QImage img;
    if (!img.loadFromData(bytes)) {
        QMessageBox::warning(this, QStringLiteral("Apply texture"), QStringLiteral("Could not decode %1").arg(path));
        return;
    }
    ModelComponent c;
    c.kind = ModelComponent::Texture;
    c.data = bytes;
    c.sourceName = QFileInfo(path).fileName();
    c.name = uniqueName(QStringLiteral("Texture"));
    c.vectorIds = m_selection;   // empty = everything modelled so far
    c.height = 1;
    c.tileWidth = std::max(1.0, std::min(20.0, m_doc->boardWidth() / 8));
    appendComponent(c);
}

void ModelPanel::addStl()
{
    if (!m_doc) return;
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Import STL"), {},
                                                      QStringLiteral("STL mesh (*.stl);;All files (*)"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("Import STL"), f.errorString());
        return;
    }
    const QByteArray bytes = f.readAll();
    QVector<StlTriangle> tris;
    QString err;
    if (!parseStl(bytes, &tris, &err)) {
        QMessageBox::warning(this, QStringLiteral("Import STL"), err);
        return;
    }
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (const StlTriangle &t : tris)
        for (int v = 0; v < 3; ++v)
            for (int k = 0; k < 3; ++k) {
                mn[k] = std::min(mn[k], t.v[v][k]);
                mx[k] = std::max(mx[k], t.v[v][k]);
            }
    const double bw = mx[0] - mn[0], bd = mx[1] - mn[1], bz = mx[2] - mn[2];
    if (!(bw > 0) || !(bd > 0)) {
        QMessageBox::warning(this, QStringLiteral("Import STL"), QStringLiteral("The mesh has no XY extent."));
        return;
    }
    StlChoice ch;
    if (!askStlPlacement(this, QFileInfo(path).fileName(), bw, bd, bz,
                         m_doc->boardWidth(), m_doc->boardHeight(), &ch))
        return;
    ModelComponent c;
    c.kind = ModelComponent::StlMesh;
    c.data = bytes;
    c.sourceName = QStringLiteral("%1 (%2 triangles)").arg(QFileInfo(path).fileName()).arg(tris.size());
    c.name = uniqueName(QStringLiteral("STL"));
    c.units = ch.units;
    c.width = ch.fitWidth ? ch.width : 0;
    c.x = ch.x;
    c.y = ch.y;
    c.height = ch.height;
    c.combine = ModelComponent::Merge;
    appendComponent(c);
}

void ModelPanel::removeSelected()
{
    const int row = selectedRow();
    if (row < 0) return;
    m_store.model.components.removeAt(row);
    refreshList();
    if (!m_store.model.components.isEmpty())
        m_list->selectRow(std::min(row, int(m_store.model.components.size()) - 1));
    refreshProps();
    touch();
}

void ModelPanel::moveSelected(int delta)
{
    const int row = selectedRow();
    const int to = row + delta;
    if (row < 0 || to < 0 || to >= m_store.model.components.size()) return;
    m_store.model.components.move(row, to);
    refreshList();
    m_list->selectRow(to);
    touch();
}

int ModelPanel::selectedRow() const
{
    const auto rows = m_list->selectionModel() ? m_list->selectionModel()->selectedRows() : QModelIndexList();
    if (rows.isEmpty()) return -1;
    const int r = rows.first().row();
    return (r >= 0 && r < m_store.model.components.size()) ? r : -1;
}

ModelComponent *ModelPanel::selected()
{
    const int r = selectedRow();
    return r < 0 ? nullptr : &m_store.model.components[r];
}

void ModelPanel::refreshList()
{
    m_updating = true;
    const int keep = selectedRow();
    m_list->setRowCount(m_store.model.components.size());
    for (int i = 0; i < m_store.model.components.size(); ++i) {
        const ModelComponent &c = m_store.model.components.at(i);
        auto *on = new QTableWidgetItem;
        on->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        on->setCheckState(c.enabled ? Qt::Checked : Qt::Unchecked);
        m_list->setItem(i, 0, on);
        m_list->setItem(i, 1, new QTableWidgetItem(c.name));
        QString kind = ModelComponent::kindName(c.kind);
        if (c.kind == ModelComponent::FromVectors)
            kind += QStringLiteral(" · ") + ModelComponent::shapeName(c.shape);
        m_list->setItem(i, 2, new QTableWidgetItem(kind));
        QString h = c.kind == ModelComponent::StlMesh && c.height <= 0
                        ? QStringLiteral("natural")
                        : QStringLiteral("%1 mm").arg(c.height, 0, 'g', 4);
        if (c.baseHeight > 0)
            h += QStringLiteral(" +%1").arg(c.baseHeight, 0, 'g', 4);
        m_list->setItem(i, 3, new QTableWidgetItem(h));
        m_list->setItem(i, 4, new QTableWidgetItem(ModelComponent::combineName(c.combine)));
    }
    if (keep >= 0 && keep < m_list->rowCount())
        m_list->selectRow(keep);
    m_updating = false;
    const bool have = m_doc != nullptr;
    for (QWidget *w : {static_cast<QWidget *>(m_addImg), static_cast<QWidget *>(m_addTex),
                       static_cast<QWidget *>(m_addStl)})
        w->setEnabled(have);
    m_addVec->setEnabled(have && !m_selection.isEmpty());
}

void ModelPanel::refreshProps()
{
    const ModelComponent *c = selected();
    m_remove->setEnabled(c != nullptr);
    m_up->setEnabled(c && selectedRow() > 0);
    m_down->setEnabled(c && selectedRow() < m_store.model.components.size() - 1);
    m_propBox->setEnabled(c != nullptr);
    m_updating = true;
    auto show = [this](QWidget *field, bool on) {
        const int i = m_propRows.indexOf(field);
        if (i > 0) {
            m_propRows[i - 1]->setVisible(on);
            m_propRows[i]->setVisible(on);
        }
    };
    if (!c) {
        for (QWidget *w : m_propRows) w->setVisible(false);
        show(m_name, true);
        m_name->clear();
        m_propBox->setTitle(QStringLiteral("Component — none selected"));
        m_updating = false;
        return;
    }
    const bool vec = c->kind == ModelComponent::FromVectors;
    const bool img = c->kind == ModelComponent::ImageHeightmap;
    const bool tex = c->kind == ModelComponent::Texture;
    const bool stl = c->kind == ModelComponent::StlMesh;
    m_propBox->setTitle(QStringLiteral("Component — %1%2")
                            .arg(ModelComponent::kindName(c->kind))
                            .arg(vec ? QStringLiteral(" (%1 vector%2)").arg(c->vectorIds.size())
                                           .arg(c->vectorIds.size() == 1 ? QString() : QStringLiteral("s"))
                                     : QString()));
    show(m_name, true);
    show(m_shape, vec);
    show(m_height, true);
    show(m_base, true);
    show(m_angle, vec && c->shape == ModelComponent::Angle);
    show(m_combine, true);
    show(m_invert, img || tex);
    show(m_blur, img || tex);
    show(m_x, img || stl || tex);
    show(m_y, img || stl || tex);
    show(m_width, img || stl);
    show(m_tile, tex);
    show(m_units, stl);
    show(m_source, img || tex || stl);
    m_name->setText(c->name);
    m_shape->setCurrentIndex(m_shape->findData(int(c->shape)));
    m_height->setValue(c->height);
    m_height->setSpecialValueText(QString());
    m_height->setMinimum(stl ? 0 : -1000);
    if (stl) m_height->setSpecialValueText(QStringLiteral("natural"));
    m_base->setValue(c->baseHeight);
    m_angle->setValue(c->angleDeg);
    m_combine->setCurrentIndex(m_combine->findData(int(c->combine)));
    m_invert->setChecked(c->invert);
    m_blur->setValue(c->blurPx);
    m_x->setValue(c->x);
    m_y->setValue(c->y);
    m_width->setValue(c->width);
    m_tile->setValue(c->tileWidth);
    m_units->setCurrentIndex(m_units->findData(int(c->units)));
    QString src = c->sourceName;
    if (tex && !c->vectorIds.isEmpty())
        src += QStringLiteral("  over %1 vector(s)").arg(c->vectorIds.size());
    else if (tex)
        src += QStringLiteral("  over the whole model");
    m_source->setText(src);
    m_updating = false;
}

void ModelPanel::applyProps()
{
    if (m_updating) return;
    ModelComponent *c = selected();
    if (!c) return;
    const ModelComponent::Shape shapeWas = c->shape;
    c->name = m_name->text();
    c->shape = ModelComponent::Shape(m_shape->currentData().toInt());
    c->height = m_height->value();
    c->baseHeight = m_base->value();
    c->angleDeg = m_angle->value();
    c->combine = ModelComponent::Combine(m_combine->currentData().toInt());
    c->invert = m_invert->isChecked();
    c->blurPx = m_blur->value();
    c->x = m_x->value();
    c->y = m_y->value();
    c->width = m_width->value();
    c->tileWidth = m_tile->value();
    c->units = ModelComponent::Units(m_units->currentData().toInt());
    const int row = selectedRow();
    refreshList();
    if (row >= 0) {
        m_updating = true;
        m_list->selectRow(row);
        m_updating = false;
    }
    if (shapeWas != c->shape)
        refreshProps();   // the Angle row appears/disappears
    touch();
}

// ---- rebuild ----------------------------------------------------------------

void ModelPanel::touch()
{
    m_store.invalidate();
    m_stale = true;
    emit modelChanged();
    scheduleRebuild();
}

void ModelPanel::scheduleRebuild()
{
    m_debounce->start();
}

void ModelPanel::startRebuild()
{
    if (!m_doc) return;
    if (m_job->isRunning()) {
        m_pending = true;
        m_job->cancel = true;
        return;
    }
    m_stale = false;
    if (!m_store.model.hasEnabled()) {
        takeResult(HeightModel());
        return;
    }
    m_job->cancel = false;
    m_job->model = m_store.model;
    // The worker only needs id + outline; rebuild the paths so it never shares
    // (lazily cached, implicitly shared) QPainterPath data with the canvas.
    m_job->elements.clear();
    for (const Element &e : m_doc->elements()) {
        Element copy;
        copy.id = e.id;
        copy.painterPath.addPath(e.painterPath);
        copy.painterPath.setFillRule(e.painterPath.fillRule());
        m_job->elements.append(copy);
    }
    m_job->boardW = m_doc->boardWidth();
    m_job->boardH = m_doc->boardHeight();
    setBusy(true);
    m_job->start();
}

void ModelPanel::cancelRebuild()
{
    m_debounce->stop();
    if (m_job->isRunning()) {
        m_job->cancel = true;
        m_job->wait();
    }
}

void ModelPanel::rebuildBlocking()
{
    cancelRebuild();
    m_pending = false;
    if (!m_doc) return;
    m_stale = false;
    takeResult(m_store.model.hasEnabled() ? buildHeightModel(m_store.model, *m_doc) : HeightModel());
}

void ModelPanel::setBusy(bool on)
{
    m_rebuild->setEnabled(!on);
    m_cancel->setEnabled(on);
    m_res->setEnabled(!on);
    if (on) {
        m_progress->setValue(0);
        m_status->setText(QStringLiteral("Building model…"));
    }
}

void ModelPanel::takeResult(const HeightModel &hm)
{
    m_store.setHeightModel(hm);
    m_image = renderHeightModel(hm);
    m_progress->setValue(100);
    m_view->setImage(m_image);
    updateStatus();
    emit modelRebuilt();
}

void ModelPanel::updateStatus()
{
    if (m_doc && m_doc->boardWidth() > 0)
        m_view->setCaption(QStringLiteral("Stock %1 × %2 mm").arg(m_doc->boardWidth(), 0, 'g', 5)
                               .arg(m_doc->boardHeight(), 0, 'g', 5));
    else
        m_view->setCaption(QString());
    if (!m_doc) {
        m_status->setText(QStringLiteral("Open a .c2d file to model"));
        return;
    }
    const HeightModel &hm = m_store.cached();
    int enabled = 0;
    for (const ModelComponent &c : m_store.model.components)
        if (c.enabled) ++enabled;
    if (!hm.valid()) {
        m_status->setText(m_store.model.components.isEmpty()
                              ? QStringLiteral("No components — select vectors and use From vectors, or load an image / STL")
                              : (enabled == 0 ? QStringLiteral("%1 component(s), none enabled").arg(m_store.model.components.size())
                                              : QStringLiteral("%1 component(s) — rebuilding…").arg(m_store.model.components.size())));
        return;
    }
    QString s = QStringLiteral("%1 component(s) · %2×%3 cells @ %4 mm · relief height %5 mm")
                    .arg(enabled).arg(hm.cols).arg(hm.rows).arg(hm.cell, 0, 'g', 3)
                    .arg(-hm.baseZ, 0, 'f', 2);
    if (m_stale)
        s += QStringLiteral(" · edited — rebuilding");
    m_status->setText(s);
}

} // namespace c2d

#include "modelpanel.moc"
