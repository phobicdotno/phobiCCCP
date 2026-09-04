#include "isopreview.h"

#include <QComboBox>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace c2d {

namespace {

// Same palette as the 2D canvas overlay so both previews read alike.
const QColor kViewBg(0x15, 0x17, 0x1c);
const QColor kBoardFill(0x21, 0x24, 0x2b);
const QColor kBoardEdge(0x4a, 0x52, 0x60);
const QColor kBoardEdgeBack(0x33, 0x38, 0x42);
const QColor kAxisX(0x8a, 0x4a, 0x4a);
const QColor kAxisY(0x4a, 0x7a, 0x4a);
const QColor kAxisZ(0x4a, 0x5a, 0x8a);
const QColor kRapid(0x8a, 0x94, 0xa6);
const QColor kTool(0x7f, 0xd4, 0xff);
const QColor kLive(0xff, 0x8c, 0x1a);
const QColor kText(0xd8, 0xdc, 0xe4);

const double kRapidFeed = 5000.0;      // mm/min, matches computeStats()
const double kDefaultFeed = 1000.0;    // for Feed/Arc ops with no F word
const int kBuckets = 8;
const int kMaxDrawSegs = 200000;

QColor bucketColor(int b)
{
    const double f = kBuckets > 1 ? double(b) / (kBuckets - 1) : 0;
    return QColor::fromHsvF(0.14 * (1.0 - f), 0.9, 1.0);   // yellow → red
}

struct Seg {
    double ax, ay, az, bx, by, bz;
    double t0, t1;      // seconds from program start
    int op;             // index into the ops list
    qint8 bucket;       // depth bucket (cuts only)
    bool rapid;
};

QString fmtTime(double sec)
{
    const int s = qMax(0, int(sec + 0.5));
    return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

} // namespace

// ---------------------------------------------------------------------------
// IsoView: the painted/orbitable 3D area.

class IsoView : public QWidget
{
public:
    explicit IsoView(QWidget *parent) : QWidget(parent)
    {
        setMinimumSize(200, 200);
        setCursor(Qt::OpenHandCursor);
    }

    void setJob(const QVector<Op> &ops, double w, double h, double t);
    void setLive(double x, double y, double z, bool valid)
    {
        m_lx = x; m_ly = y; m_lz = z; m_lvalid = valid;
        update();
    }

    double totalTime() const { return m_total; }
    double time() const { return m_time; }
    void setTime(double t) { m_time = qBound(0.0, t, m_total); update(); }
    bool empty() const { return m_segs.isEmpty(); }
    int opCount() const { return m_opCount; }

    struct Cursor { int op = 0; double x = 0, y = 0, z = 0; };
    Cursor cursorAt(double t) const;

    void resetView()
    {
        m_yaw = -45; m_pitch = 35; m_pan = QPointF();
        m_fitPending = true;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override
    {
        m_last = e->pos();
        setCursor(Qt::ClosedHandCursor);
    }
    void mouseReleaseEvent(QMouseEvent *) override { setCursor(Qt::OpenHandCursor); }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        const QPoint d = e->pos() - m_last;
        m_last = e->pos();
        const bool orbit = (e->buttons() & Qt::LeftButton)
                           && !(e->modifiers() & Qt::ShiftModifier);
        if (orbit) {
            m_yaw += d.x() * 0.5;
            m_pitch = qBound(-89.0, m_pitch + d.y() * 0.5, 89.0);
        } else if (e->buttons()) {
            m_pan += QPointF(d);
        }
        update();
    }
    void wheelEvent(QWheelEvent *e) override
    {
        const double steps = e->angleDelta().y() / 120.0;
        if (steps == 0)
            return;
        const double f = qPow(1.25, steps);
        // Zoom about the cursor: keep the world point under it fixed.
        const QPointF c(width() / 2.0, height() / 2.0);
        const QPointF pos = e->position();
        m_pan = pos - c - f * (pos - c - m_pan);
        m_scale *= f;
        update();
    }

private:
    QPointF raw(double x, double y, double z) const
    {
        const double yr = qDegreesToRadians(m_yaw), pr = qDegreesToRadians(m_pitch);
        const double x1 = x * qCos(yr) - y * qSin(yr);
        const double y1 = x * qSin(yr) + y * qCos(yr);
        return QPointF(x1, -(z * qCos(pr) + y1 * qSin(pr)));
    }
    QPointF proj(double x, double y, double z) const
    {
        return m_center + m_pan + m_scale * (raw(x, y, z) - m_rawCenter);
    }
    void fit();
    void boundsCorners(QVector<QPointF> &out) const;

    QVector<Seg> m_segs;
    double m_total = 0, m_time = 0;
    int m_opCount = 0, m_stride = 1;
    double m_w = 0, m_h = 0, m_t = 0;
    double m_bx0 = 0, m_by0 = 0, m_bz0 = -10, m_bx1 = 100, m_by1 = 100, m_bz1 = 0;   // union bounds
    double m_sx = 0, m_sy = 0, m_sz = 0;   // start position (first op target)

    double m_yaw = -45, m_pitch = 35, m_scale = 1;
    QPointF m_pan, m_center, m_rawCenter;
    QPoint m_last;
    bool m_fitPending = true;

    double m_lx = 0, m_ly = 0, m_lz = 0;
    bool m_lvalid = false;
};

void IsoView::setJob(const QVector<Op> &ops, double w, double h, double t)
{
    m_w = w; m_h = h; m_t = qMax(0.0, t);
    m_segs.clear();
    m_opCount = ops.size();

    double zMin = 0;
    for (const Op &op : ops)
        if ((op.kind == Op::Feed || op.kind == Op::Arc) && op.z < zMin)
            zMin = op.z;
    auto bucketOf = [&](double z) {
        if (zMin >= -1e-9)
            return 0;
        return qBound(0, int((z / zMin) * kBuckets), kBuckets - 1);
    };

    double x = 0, y = 0, z = 0, tm = 0;
    bool have = false, haveBounds = false;
    auto grow = [&](double px, double py, double pz) {
        if (!haveBounds) {
            m_bx0 = m_bx1 = px; m_by0 = m_by1 = py; m_bz0 = m_bz1 = pz;
            haveBounds = true;
        } else {
            m_bx0 = qMin(m_bx0, px); m_bx1 = qMax(m_bx1, px);
            m_by0 = qMin(m_by0, py); m_by1 = qMax(m_by1, py);
            m_bz0 = qMin(m_bz0, pz); m_bz1 = qMax(m_bz1, pz);
        }
    };
    auto addSeg = [&](double nx, double ny, double nz, double feed, bool rapid,
                      int op, int bucket) {
        const double len = qSqrt((nx - x) * (nx - x) + (ny - y) * (ny - y)
                                 + (nz - z) * (nz - z));
        const double dt = len / qMax(1.0, feed) * 60.0;
        m_segs.append({x, y, z, nx, ny, nz, tm, tm + dt, op, qint8(bucket), rapid});
        tm += dt;
        grow(nx, ny, nz);
    };

    for (int i = 0; i < ops.size(); ++i) {
        const Op &op = ops.at(i);
        switch (op.kind) {
        case Op::Rapid:
            if (have) addSeg(op.x, op.y, op.z, kRapidFeed, true, i, 0);
            else { m_sx = op.x; m_sy = op.y; m_sz = op.z; grow(op.x, op.y, op.z); }
            x = op.x; y = op.y; z = op.z; have = true;
            break;
        case Op::Feed:
            if (have) addSeg(op.x, op.y, op.z, op.feed > 0 ? op.feed : kDefaultFeed,
                             false, i, bucketOf(op.z));
            else { m_sx = op.x; m_sy = op.y; m_sz = op.z; grow(op.x, op.y, op.z); }
            x = op.x; y = op.y; z = op.z; have = true;
            break;
        case Op::Arc: {
            if (!have) { m_sx = op.x; m_sy = op.y; m_sz = op.z; grow(op.x, op.y, op.z);
                         x = op.x; y = op.y; z = op.z; have = true; break; }
            const double cx = x + op.ci, cy = y + op.cj;
            const double r = QLineF(QPointF(cx, cy), QPointF(x, y)).length();
            const double a0 = qAtan2(y - cy, x - cx);
            double a1 = qAtan2(op.y - cy, op.x - cx);
            // G2 = clockwise in Y-up space = decreasing angle. start==end → full circle.
            if (op.cw) { while (a1 >= a0 - 1e-12) a1 -= 2 * M_PI; }
            else       { while (a1 <= a0 + 1e-12) a1 += 2 * M_PI; }
            const int steps = qMax(8, int(qAbs(a1 - a0) / (M_PI / 36)));
            const double feed = op.feed > 0 ? op.feed : kDefaultFeed;
            const int b = bucketOf(op.z);
            const double z0 = z;
            for (int k = 1; k <= steps; ++k) {
                const double a = a0 + (a1 - a0) * k / steps;
                const double nz = z0 + (op.z - z0) * k / steps;
                double nx = cx + r * qCos(a), ny = cy + r * qSin(a);
                if (k == steps) { nx = op.x; ny = op.y; }
                addSeg(nx, ny, nz, feed, false, i, b);
                x = nx; y = ny; z = nz;
            }
            x = op.x; y = op.y; z = op.z;
            break;
        }
        default:
            break;
        }
    }
    m_total = tm;
    m_time = tm;   // start "finished": whole route bright, tool at the end
    const int n = m_segs.size();
    m_stride = n > kMaxDrawSegs ? (n + kMaxDrawSegs - 1) / kMaxDrawSegs : 1;

    if (m_w > 0 && m_h > 0) {
        if (!haveBounds) { m_bx0 = m_by0 = m_bz0 = 0; m_bx1 = m_bx0; m_by1 = m_by0; m_bz1 = 0; }
        m_bx0 = qMin(m_bx0, 0.0); m_bx1 = qMax(m_bx1, m_w);
        m_by0 = qMin(m_by0, 0.0); m_by1 = qMax(m_by1, m_h);
        m_bz0 = qMin(m_bz0, -m_t); m_bz1 = qMax(m_bz1, 0.0);
    } else if (!haveBounds) {
        m_bx0 = m_by0 = 0; m_bz0 = -10; m_bx1 = m_by1 = 100; m_bz1 = 0;   // empty: placeholder box
    }
    m_fitPending = true;
    update();
}

IsoView::Cursor IsoView::cursorAt(double t) const
{
    Cursor c;
    if (m_segs.isEmpty()) {
        c.x = m_sx; c.y = m_sy; c.z = m_sz;
        return c;
    }
    if (t >= m_total) {
        const Seg &s = m_segs.last();
        c.op = s.op; c.x = s.bx; c.y = s.by; c.z = s.bz;
        return c;
    }
    // first seg with t1 > t
    auto it = std::upper_bound(m_segs.constBegin(), m_segs.constEnd(), t,
                               [](double v, const Seg &s) { return v < s.t1; });
    if (it == m_segs.constEnd())
        it = m_segs.constEnd() - 1;
    const Seg &s = *it;
    const double dur = s.t1 - s.t0;
    const double f = dur > 1e-12 ? qBound(0.0, (t - s.t0) / dur, 1.0) : 1.0;
    c.op = s.op;
    c.x = s.ax + (s.bx - s.ax) * f;
    c.y = s.ay + (s.by - s.ay) * f;
    c.z = s.az + (s.bz - s.az) * f;
    return c;
}

void IsoView::boundsCorners(QVector<QPointF> &out) const
{
    for (int i = 0; i < 8; ++i)
        out.append(raw(i & 1 ? m_bx1 : m_bx0, i & 2 ? m_by1 : m_by0, i & 4 ? m_bz1 : m_bz0));
}

void IsoView::fit()
{
    m_fitPending = false;
    m_pan = QPointF();
    QVector<QPointF> pts;
    boundsCorners(pts);
    double x0 = pts[0].x(), x1 = x0, y0 = pts[0].y(), y1 = y0;
    for (const QPointF &p : pts) {
        x0 = qMin(x0, p.x()); x1 = qMax(x1, p.x());
        y0 = qMin(y0, p.y()); y1 = qMax(y1, p.y());
    }
    const double bw = qMax(1e-6, x1 - x0), bh = qMax(1e-6, y1 - y0);
    m_scale = 0.82 * qMin((width() - 24) / bw, (height() - 48) / bh);
    if (!(m_scale > 0) || !std::isfinite(m_scale))
        m_scale = 1;
}

void IsoView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), kViewBg);
    p.setRenderHint(QPainter::Antialiasing);

    m_center = QPointF(width() / 2.0, height() / 2.0);
    m_rawCenter = raw((m_bx0 + m_bx1) / 2, (m_by0 + m_by1) / 2, (m_bz0 + m_bz1) / 2);
    if (m_fitPending)
        fit();

    const bool haveStock = m_w > 0 && m_h > 0;
    const double dim = qMax(qMax(m_bx1 - m_bx0, m_by1 - m_by0), qMax(m_bz1 - m_bz0, 1.0));

    if (haveStock) {
        // Corners: bit0 = x, bit1 = y, bit2 = z (top when set).
        QPointF c[8];
        for (int i = 0; i < 8; ++i)
            c[i] = proj(i & 1 ? m_w : 0, i & 2 ? m_h : 0, i & 4 ? 0 : -m_t);
        QPolygonF top; top << c[4] << c[5] << c[7] << c[6];
        QColor fill = kBoardFill; fill.setAlpha(170);
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawPolygon(top);
        p.setBrush(Qt::NoBrush);
        // Bottom face + verticals in a dimmer tone, top rim brighter.
        p.setPen(QPen(kBoardEdgeBack, 1));
        p.drawPolygon(QPolygonF() << c[0] << c[1] << c[3] << c[2]);
        for (int i = 0; i < 4; ++i)
            p.drawLine(c[i], c[i | 4]);
        p.setPen(QPen(kBoardEdge, 1.2));
        p.drawPolygon(top);

        // Faint 10 mm grid on the top face (skipped when it would be dense).
        const double g = 10.0;
        if (g * m_scale >= 9.0) {
            QPen gp(kBoardEdgeBack, 0.8); gp.setColor(QColor(0x2c, 0x31, 0x3a));
            p.setPen(gp);
            for (double gx = g; gx < m_w; gx += g)
                p.drawLine(proj(gx, 0, 0), proj(gx, m_h, 0));
            for (double gy = g; gy < m_h; gy += g)
                p.drawLine(proj(0, gy, 0), proj(m_w, gy, 0));
        }
    }

    // Axis stubs at work zero (X red, Y green, Z blue).
    {
        const double L = 0.12 * dim;
        const QPointF o = proj(0, 0, 0);
        p.setPen(QPen(kAxisX, 1.6)); p.drawLine(o, proj(L, 0, 0));
        p.setPen(QPen(kAxisY, 1.6)); p.drawLine(o, proj(0, L, 0));
        p.setPen(QPen(kAxisZ, 1.6)); p.drawLine(o, proj(0, 0, L));
    }

    // Route: bucket lines into [done|remaining] x [rapid|cut buckets].
    const Cursor cur = cursorAt(m_time);
    const QPointF toolPt = proj(cur.x, cur.y, cur.z);
    QVector<QLineF> done[kBuckets + 1], todo[kBuckets + 1];   // index kBuckets = rapid
    const int n = m_segs.size();
    for (int i = 0; i < n; i += m_stride) {
        const Seg &a = m_segs.at(i);
        const Seg &b = m_segs.at(qMin(n - 1, i + m_stride - 1));
        const int slot = a.rapid ? kBuckets : a.bucket;
        const QPointF pa = proj(a.ax, a.ay, a.az), pb = proj(b.bx, b.by, b.bz);
        if (b.t1 <= m_time)
            done[slot].append(QLineF(pa, pb));
        else if (a.t0 >= m_time)
            todo[slot].append(QLineF(pa, pb));
        else {
            done[slot].append(QLineF(pa, toolPt));
            todo[slot].append(QLineF(toolPt, pb));
        }
    }
    const bool anyTodo = m_time < m_total;
    auto drawSlot = [&](int slot, const QVector<QLineF> &ls, bool bright) {
        if (ls.isEmpty())
            return;
        QPen pen;
        if (slot == kBuckets) {
            QColor col = kRapid; col.setAlpha(bright ? 140 : 55);
            pen = QPen(col, 1);
            pen.setStyle(Qt::DashLine);
        } else {
            QColor col = bucketColor(slot);
            col.setAlpha(bright ? 220 : (anyTodo ? 70 : 220));
            pen = QPen(col, bright ? 1.6 : 1.0);
        }
        p.setPen(pen);
        p.drawLines(ls);
    };
    for (int s = 0; s <= kBuckets; ++s) drawSlot(s, todo[s], false);
    for (int s = 0; s <= kBuckets; ++s) drawSlot(s, done[s], true);

    // Tool glyph: cone with tip at the current position, base above it.
    if (!m_segs.isEmpty() || m_opCount > 0) {
        const double h = qMax(3.0, 0.09 * dim), r = h * 0.38;
        QPolygonF base;
        for (int k = 0; k < 16; ++k) {
            const double a = 2 * M_PI * k / 16;
            base << proj(cur.x + r * qCos(a), cur.y + r * qSin(a), cur.z + h);
        }
        QColor fill = kTool; fill.setAlpha(120);
        p.setPen(QPen(kTool, 1.2));
        p.setBrush(fill);
        p.drawPolygon(base);
        // Silhouette: leftmost/rightmost base points to the tip.
        int li = 0, ri = 0;
        for (int k = 1; k < 16; ++k) {
            if (base[k].x() < base[li].x()) li = k;
            if (base[k].x() > base[ri].x()) ri = k;
        }
        p.drawLine(base[li], toolPt);
        p.drawLine(base[ri], toolPt);
        // Shank line up from the base, and a drop line to the stock top.
        p.drawLine(proj(cur.x, cur.y, cur.z + h), proj(cur.x, cur.y, cur.z + 2.2 * h));
        QColor drop = kTool; drop.setAlpha(80);
        p.setPen(QPen(drop, 1, Qt::DotLine));
        p.drawLine(toolPt, proj(cur.x, cur.y, 0));
        p.setBrush(Qt::NoBrush);
    }

    // Live machine position: orange crosshair.
    if (m_lvalid) {
        const double L = qMax(2.0, 0.06 * dim);
        p.setPen(QPen(kLive, 1.6));
        p.drawLine(proj(m_lx - L, m_ly, m_lz), proj(m_lx + L, m_ly, m_lz));
        p.drawLine(proj(m_lx, m_ly - L, m_lz), proj(m_lx, m_ly + L, m_lz));
        p.drawLine(proj(m_lx, m_ly, m_lz - L), proj(m_lx, m_ly, m_lz + L));
        p.drawEllipse(proj(m_lx, m_ly, m_lz), 4.0, 4.0);
    }

    // Corner caption.
    p.setPen(kText);
    QFont f = p.font(); f.setPointSizeF(f.pointSizeF() * 0.9); p.setFont(f);
    QString cap;
    if (m_opCount == 0)
        cap = QStringLiteral("No program — open a .c2d with toolpaths");
    else if (haveStock)
        cap = QStringLiteral("Stock %1 × %2 × %3 mm").arg(m_w, 0, 'g', 5).arg(m_h, 0, 'g', 5).arg(m_t, 0, 'g', 4);
    p.drawText(QRectF(8, 6, width() - 16, 18), Qt::AlignLeft | Qt::AlignVCenter, cap);
    QColor hint = kText; hint.setAlpha(110); p.setPen(hint);
    p.drawText(QRectF(8, height() - 22, width() - 16, 18), Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("drag: orbit   shift/right-drag: pan   wheel: zoom"));
}

// ---------------------------------------------------------------------------
// IsoPreview: view + transport controls.

IsoPreview::IsoPreview(QWidget *parent)
    : QWidget(parent)
{
    m_view = new IsoView(this);
    m_timer = new QTimer(this);
    m_timer->setInterval(33);

    m_playBtn = new QToolButton(this);
    m_playBtn->setText(QStringLiteral("▶ Play"));
    m_playBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_playBtn->setMinimumWidth(72);

    m_slider = new QSlider(Qt::Horizontal, this);
    m_slider->setRange(0, 1000);
    m_slider->setToolTip(QStringLiteral("Position along the program (%)"));

    m_speed = new QComboBox(this);
    for (int s : {1, 4, 16, 64})
        m_speed->addItem(QStringLiteral("%1×").arg(s), s);
    m_speed->setCurrentIndex(1);
    m_speed->setToolTip(QStringLiteral("Playback speed relative to real time\n"
                                       "(feed rates as programmed, rapids 5000 mm/min)"));

    auto *resetBtn = new QToolButton(this);
    resetBtn->setText(QStringLiteral("Reset view"));
    resetBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);

    m_status = new QLabel(this);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont mono = m_status->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::Monospace);
    m_status->setFont(mono);

    auto *row = new QHBoxLayout;
    row->setContentsMargins(6, 4, 6, 0);
    row->setSpacing(6);
    row->addWidget(m_playBtn);
    row->addWidget(m_slider, 1);
    row->addWidget(m_speed);
    row->addWidget(resetBtn);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(2);
    lay->addWidget(m_view, 1);
    lay->addLayout(row);
    m_status->setContentsMargins(8, 0, 8, 4);
    lay->addWidget(m_status);

    connect(m_playBtn, &QToolButton::clicked, this, [this] {
        if (isPlaying()) pause(); else play();
    });
    connect(resetBtn, &QToolButton::clicked, this, &IsoPreview::resetView);
    connect(m_slider, &QSlider::valueChanged, this, [this](int v) {
        if (m_slider->signalsBlocked())
            return;
        m_view->setTime(m_view->totalTime() * v / 1000.0);
        updateStatus();
    });
    connect(m_timer, &QTimer::timeout, this, &IsoPreview::tick);

    syncSlider();
    updateStatus();
}

void IsoPreview::setJob(const QVector<Op> &ops, double stockW, double stockH, double stockT)
{
    pause();
    m_view->setJob(ops, stockW, stockH, stockT);
    syncSlider();
    updateStatus();
}

void IsoPreview::setLivePosition(double x, double y, double z, bool valid)
{
    m_view->setLive(x, y, z, valid);
}

void IsoPreview::play()
{
    if (m_view->empty())
        return;
    if (m_view->time() >= m_view->totalTime())
        m_view->setTime(0);   // replay from the start
    m_clock.start();
    m_timer->start();
    m_playBtn->setText(QStringLiteral("⏸ Pause"));
    updateStatus();
}

void IsoPreview::pause()
{
    m_timer->stop();
    m_playBtn->setText(QStringLiteral("▶ Play"));
}

bool IsoPreview::isPlaying() const { return m_timer->isActive(); }

void IsoPreview::setProgress(double fraction)
{
    m_view->setTime(m_view->totalTime() * qBound(0.0, fraction, 1.0));
    syncSlider();
    updateStatus();
}

double IsoPreview::progress() const
{
    const double t = m_view->totalTime();
    return t > 0 ? m_view->time() / t : 0.0;
}

void IsoPreview::resetView() { m_view->resetView(); }

void IsoPreview::tick()
{
    const double dt = m_clock.restart() / 1000.0;
    const double speed = m_speed->currentData().toDouble();
    const double t = m_view->time() + dt * speed;
    m_view->setTime(t);
    if (t >= m_view->totalTime())
        pause();
    syncSlider();
    updateStatus();
}

void IsoPreview::syncSlider()
{
    const double tot = m_view->totalTime();
    m_slider->blockSignals(true);
    m_slider->setValue(tot > 0 ? qRound(m_view->time() / tot * 1000.0) : 0);
    m_slider->blockSignals(false);
    m_slider->setEnabled(tot > 0);
    m_playBtn->setEnabled(!m_view->empty());
}

void IsoPreview::updateStatus()
{
    if (m_view->opCount() == 0) {
        m_status->setText(QStringLiteral("—"));
        return;
    }
    const IsoView::Cursor c = m_view->cursorAt(m_view->time());
    m_status->setText(QStringLiteral("op %1/%2   X %3  Y %4  Z %5   %6 / %7")
                          .arg(c.op + 1).arg(m_view->opCount())
                          .arg(c.x, 7, 'f', 2).arg(c.y, 7, 'f', 2).arg(c.z, 6, 'f', 2)
                          .arg(fmtTime(m_view->time()), fmtTime(m_view->totalTime())));
}

} // namespace c2d
