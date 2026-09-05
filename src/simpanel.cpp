#include "simpanel.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtMath>
#include <cmath>
#include <functional>

namespace c2d {

namespace {
// Same palette as the isometric preview so the two tabs read alike.
const QColor kViewBg(0x15, 0x17, 0x1c);
const QColor kBoardEdge(0x4a, 0x52, 0x60);
const QColor kText(0xd8, 0xdc, 0xe4);
}

// ---------------------------------------------------------------------------
// SimView: the zoomable image area.

class SimView : public QWidget
{
public:
    using HoverFn = std::function<void(int ix, int iy, bool valid)>;

    explicit SimView(QWidget *parent) : QWidget(parent)
    {
        setMinimumSize(200, 200);
        setMouseTracking(true);
        setCursor(Qt::CrossCursor);
    }

    void setImage(const QImage &img, double stockW, double stockH, double stockT)
    {
        if (m_img.isNull() || img.size() != m_img.size())
            m_fitPending = true;   // new map size: refit; same size keeps the user's zoom
        m_img = img;
        m_w = stockW; m_h = stockH; m_t = stockT;
        update();
    }
    void setHoverHandler(HoverFn fn) { m_hover = std::move(fn); }
    void setCaption(const QString &c) { m_caption = c; update(); }
    void resetView() { m_fitPending = true; update(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), kViewBg);
        if (m_img.isNull()) {
            p.setPen(kText);
            p.drawText(rect(), Qt::AlignCenter,
                       m_caption.isEmpty() ? QStringLiteral("No simulation yet — press Simulate")
                                           : m_caption);
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
        if (m_img.isNull())
            return;
        const double steps = e->angleDelta().y() / 120.0;
        if (steps == 0)
            return;
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
        if (m_hover)
            m_hover(0, 0, false);
    }
    void resizeEvent(QResizeEvent *) override
    {
        if (m_fitPending)
            update();
    }

private:
    void fit()
    {
        m_fitPending = false;
        if (m_img.isNull())
            return;
        const double sx = (width() - 24.0) / m_img.width();
        const double sy = (height() - 52.0) / m_img.height();
        m_scale = qMin(sx, sy);
        if (!(m_scale > 0) || !std::isfinite(m_scale))
            m_scale = 1;
        m_off = QPointF((width() - m_img.width() * m_scale) / 2.0,
                        (height() - m_img.height() * m_scale) / 2.0 + 4);
    }
    void report(const QPointF &pos)
    {
        if (!m_hover)
            return;
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
    double m_w = 0, m_h = 0, m_t = 0;
    double m_scale = 1;
    QPointF m_off;
    QPoint m_last;
    bool m_drag = false, m_fitPending = true;
    QString m_caption;
    HoverFn m_hover;
};

// ---------------------------------------------------------------------------
// SimPanel

SimPanel::SimPanel(QWidget *parent)
    : QWidget(parent)
{
    m_job = new SimulationJob(this);
    m_view = new SimView(this);

    m_simBtn = new QPushButton(QStringLiteral("Simulate"), this);
    m_simBtn->setToolTip(QStringLiteral("Run the material-removal simulation of the current program"));
    m_cancelBtn = new QPushButton(QStringLiteral("Cancel"), this);
    m_cancelBtn->setEnabled(false);

    m_res = new QComboBox(this);
    m_res->addItem(QStringLiteral("Fine (0.1 mm)"), 0.1);
    m_res->addItem(QStringLiteral("Normal (0.2 mm)"), 0.2);
    m_res->addItem(QStringLiteral("Coarse (0.4 mm)"), 0.4);
    m_res->setCurrentIndex(1);
    m_res->setToolTip(QStringLiteral("Heightmap cell size (coarsened further so the map stays ≤ 4 M cells)"));

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setTextVisible(false);
    m_progress->setMaximumHeight(8);

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

    auto *row = new QHBoxLayout;
    row->setContentsMargins(6, 4, 6, 0);
    row->setSpacing(6);
    row->addWidget(m_simBtn);
    row->addWidget(m_cancelBtn);
    row->addWidget(new QLabel(QStringLiteral("Resolution"), this));
    row->addWidget(m_res, 1);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(2);
    lay->addLayout(row);
    lay->addWidget(m_progress);
    lay->addWidget(m_view, 1);
    lay->addWidget(m_readout);
    lay->addWidget(m_status);

    connect(m_simBtn, &QPushButton::clicked, this, &SimPanel::simulate);
    connect(m_cancelBtn, &QPushButton::clicked, this, &SimPanel::cancel);
    connect(m_job, &SimulationJob::progress, this, [this](int p) { m_progress->setValue(p); });
    connect(m_job, &SimulationJob::finished, this, [this](bool) {
        takeResult(m_job->result());
        setBusy(false);
    });
    m_view->setHoverHandler([this](int ix, int iy, bool valid) {
        const HeightMap &m = m_result.map;
        if (!valid || m.isNull() || ix < 0 || iy < 0 || ix >= m.width() || iy >= m.height()) {
            m_readout->setText(QStringLiteral("X    —      Y    —      Z    —"));
            return;
        }
        const double z = m.at(ix, iy);
        QString zs = QStringLiteral("%1").arg(z, 7, 'f', 3);
        if (m_t > 0 && z <= -m_t + 1e-6)
            zs += QStringLiteral(" (through)");
        m_readout->setText(QStringLiteral("X %1  Y %2  Z %3")
                               .arg(m.cellCenterX(ix), 7, 'f', 2)
                               .arg(m.cellCenterY(iy), 7, 'f', 2)
                               .arg(zs));
    });

    m_readout->setText(QStringLiteral("X    —      Y    —      Z    —"));
    updateStatus();
}

void SimPanel::setJob(const QVector<Op> &ops, const QHash<int, ToolGeom> &tools,
                      double stockW, double stockH, double stockT)
{
    // A run already in flight is computing the previous program: stop it, or
    // its result would land as if it were current.
    if (isRunning())
        cancel();
    m_ops = ops;
    m_tools = tools;
    m_w = stockW; m_h = stockH; m_t = qMax(0.0, stockT);
    m_stale = !m_result.map.isNull();
    m_simBtn->setEnabled(!m_ops.isEmpty() && !isRunning());
    updateStatus();
}

SimSettings SimPanel::settings() const
{
    SimSettings s;
    s.stockW = m_w; s.stockH = m_h; s.stockT = m_t;
    s.minCell = m_res->currentData().toDouble();
    if (!(s.minCell > 0))
        s.minCell = 0.2;
    s.maxCells = 4000000;
    return s;
}

bool SimPanel::isRunning() const { return m_job->isRunning(); }

void SimPanel::simulate()
{
    if (isRunning() || m_ops.isEmpty())
        return;
    setBusy(true);
    m_job->start(m_ops, m_tools, settings());
}

void SimPanel::simulateBlocking()
{
    if (isRunning() || m_ops.isEmpty())
        return;
    takeResult(c2d::simulate(m_ops, m_tools, settings()));
}

void SimPanel::cancel()
{
    if (isRunning())
        m_job->cancel();
}

void SimPanel::setBusy(bool on)
{
    m_simBtn->setEnabled(!on && !m_ops.isEmpty());
    m_cancelBtn->setEnabled(on);
    m_res->setEnabled(!on);
    m_progress->setValue(on ? 0 : m_progress->value());
    if (on)
        m_status->setText(QStringLiteral("Simulating…"));
}

void SimPanel::takeResult(const SimResult &r)
{
    m_result = r;
    m_image = renderHeightMap(m_result.map, m_t);
    m_stale = false;
    m_progress->setValue(100);
    m_view->setImage(m_image, m_w, m_h, m_t);
    updateStatus();
    emit simulationFinished();
}

void SimPanel::updateStatus()
{
    if (m_w > 0 && m_h > 0)
        m_view->setCaption(QStringLiteral("Stock %1 × %2 × %3 mm")
                               .arg(m_w, 0, 'g', 5).arg(m_h, 0, 'g', 5).arg(m_t, 0, 'g', 4));
    else
        m_view->setCaption(QString());

    if (m_ops.isEmpty()) {
        m_status->setText(QStringLiteral("No program — open a .c2d with toolpaths"));
        return;
    }
    if (m_result.map.isNull()) {
        m_status->setText(QStringLiteral("%1 ops — press Simulate").arg(m_ops.size()));
        return;
    }
    const HeightMap &m = m_result.map;
    QString s = QStringLiteral("%1×%2 cells @ %3 mm · %4 segments · %5 s · min Z %6 mm")
                    .arg(m.width()).arg(m.height()).arg(m.cellSize(), 0, 'g', 3)
                    .arg(m_result.segments).arg(m_result.elapsedMs / 1000.0, 0, 'f', 2)
                    .arg(m_result.minZ, 0, 'f', 2);
    if (m_result.throughCut)
        s += QStringLiteral(" · THROUGH-CUT");
    if (m_result.cancelled)
        s += QStringLiteral(" · cancelled (partial)");
    if (!m_result.missingTools.isEmpty()) {
        QStringList nums;
        for (int n : m_result.missingTools)
            nums << QString::number(n);
        s += QStringLiteral(" · unknown tool %1 (3.175 mm flat assumed)").arg(nums.join(QStringLiteral(", ")));
    }
    if (m_stale)
        s += QStringLiteral(" · program changed — simulate again");
    m_status->setText(s);
}

} // namespace c2d
