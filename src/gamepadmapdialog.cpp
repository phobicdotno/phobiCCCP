#include "gamepadmapdialog.h"

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRadialGradient>
#include <QSet>
#include <QSettings>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>

namespace c2d {

namespace {

const QString kNone = QStringLiteral("none");
const QColor kAmber(0xf2, 0xa5, 0x36);
constexpr int kBoxW = 196;
constexpr int kBoxH = 80;
constexpr int kLineGap = 60;        // between the callout rows and the pad

// The drawn controls, per style, and which row of callouts each one uses.
struct Control {
    const char *name;
    const char *label;
    bool top;
};

QList<Control> controls(const QString &style)
{
    if (style == QLatin1String("nes"))
        return {{"DPAD", "D-PAD", true}, {"SELECT", "SELECT", true}, {"START", "START", true},
                {"B", "B", false}, {"A", "A", false}};
    return {{"L", "L", true}, {"SELECT", "SELECT", true}, {"START", "START", true},
            {"X", "X", true}, {"R", "R", true},
            {"DPAD", "D-PAD", false}, {"Y", "Y", false}, {"B", "B", false}, {"A", "A", false}};
}

// Pad numbering of the clones on the shop PC, learned with the mapper.
int defaultNumber(const QString &style, const QString &control)
{
    static const QHash<QString, int> nes = {
        {QStringLiteral("SELECT"), 8}, {QStringLiteral("START"), 9},
        {QStringLiteral("B"), 0}, {QStringLiteral("A"), 1}};
    static const QHash<QString, int> snes = {
        {QStringLiteral("SELECT"), 8}, {QStringLiteral("START"), 9},
        {QStringLiteral("Y"), 3}, {QStringLiteral("B"), 2}, {QStringLiteral("X"), 0},
        {QStringLiteral("A"), 1}, {QStringLiteral("L"), 4}, {QStringLiteral("R"), 5}};
    return (style == QLatin1String("nes") ? nes : snes).value(control, -1);
}

int layoutNumber(const QString &style, const QString &control)
{
    QSettings s;
    return s.value(QStringLiteral("gamepadLayout/%1/%2").arg(style, control),
                   defaultNumber(style, control)).toInt();
}

void glow(QPainter &p, const QPointF &c, double r)
{
    QRadialGradient g(c, r * 2.4);
    g.setColorAt(0.0, QColor(242, 165, 54, 230));
    g.setColorAt(0.4, QColor(242, 165, 54, 110));
    g.setColorAt(1.0, QColor(242, 165, 54, 0));
    p.setPen(Qt::NoPen);
    p.setBrush(g);
    p.drawEllipse(c, r * 2.4, r * 2.4);
}

QLinearGradient bodyGradient(double h)
{
    QLinearGradient g(0, 0, 0, h);
    g.setColorAt(0, QColor(0xdc, 0xdf, 0xe5));
    g.setColorAt(1, QColor(0xb4, 0xb8, 0xc0));
    return g;
}

} // namespace

// ---- the drawing ------------------------------------------------------------

class PadCanvas : public QWidget
{
public:
    struct Callout {
        QString control;
        QFrame *box;
        bool top;
    };

    explicit PadCanvas(QWidget *parent) : QWidget(parent)
    {
        setMinimumSize(5 * kBoxW + 60, 2 * kBoxH + 2 * kLineGap + 230);
    }

    void setPadStyle(const QString &s)
    {
        m_style = s;
        m_pressed.clear();
        m_dir[0] = m_dir[1] = 0;
        update();
    }
    void setCallouts(const QList<Callout> &c)
    {
        m_callouts = c;
        relayout();
        update();
    }
    void setPressed(const QString &control, bool on)
    {
        if (on)
            m_pressed.insert(control);
        else
            m_pressed.remove(control);
        update();
    }
    // Axis 0 is X (-1 left, +1 right), 1 is Y (-1 up, +1 down).
    void setAxis(int axis, int dir)
    {
        if (axis < 0 || axis > 1 || m_dir[axis] == dir)
            return;
        m_dir[axis] = dir;
        update();
    }
    bool dpadActive() const { return m_dir[0] || m_dir[1]; }

protected:
    void resizeEvent(QResizeEvent *) override { relayout(); }
    void paintEvent(QPaintEvent *) override;

private:
    bool nes() const { return m_style == QLatin1String("nes"); }
    QSizeF design() const { return nes() ? QSizeF(420, 180) : QSizeF(440, 200); }
    QPointF designAnchor(const QString &c) const;
    QRectF padRect() const;
    QPointF anchor(const QString &c) const
    {
        const QRectF r = padRect();
        const double k = r.width() / design().width();
        return r.topLeft() + designAnchor(c) * k;
    }
    void relayout();
    void paintNes(QPainter &p);
    void paintSnes(QPainter &p);
    void paintDpad(QPainter &p, const QPointF &c, double armW, double armL, const QColor &col);
    void paintRoundButton(QPainter &p, const QString &name, const QPointF &c, double r,
                          const QColor &col, const QString &letter, const QColor &letterCol);

    QString m_style = QStringLiteral("snes");
    QSet<QString> m_pressed;
    int m_dir[2] = {0, 0};
    QList<Callout> m_callouts;
};

QPointF PadCanvas::designAnchor(const QString &c) const
{
    static const QHash<QString, QPointF> n = {
        {QStringLiteral("DPAD"), {85, 103}}, {QStringLiteral("SELECT"), {182, 128}},
        {QStringLiteral("START"), {238, 128}}, {QStringLiteral("B"), {305, 118}},
        {QStringLiteral("A"), {365, 118}}};
    static const QHash<QString, QPointF> s = {
        {QStringLiteral("DPAD"), {112, 108}}, {QStringLiteral("SELECT"), {196, 122}},
        {QStringLiteral("START"), {244, 122}}, {QStringLiteral("X"), {328, 66}},
        {QStringLiteral("Y"), {286, 108}}, {QStringLiteral("A"), {370, 108}},
        {QStringLiteral("B"), {328, 150}}, {QStringLiteral("L"), {95, 10}},
        {QStringLiteral("R"), {345, 10}}};
    return (nes() ? n : s).value(c);
}

QRectF PadCanvas::padRect() const
{
    const QRectF area(20, kBoxH + kLineGap, width() - 40, height() - 2 * (kBoxH + kLineGap));
    const QSizeF d = design();
    const double k = std::min({area.width() / d.width(), area.height() / d.height(), 1.6});
    const QSizeF sz = d * k;
    return QRectF(area.center().x() - sz.width() / 2, area.center().y() - sz.height() / 2,
                  sz.width(), sz.height());
}

void PadCanvas::relayout()
{
    for (bool top : {true, false}) {
        QList<Callout> row;
        for (const Callout &c : m_callouts)
            if (c.top == top)
                row << c;
        std::sort(row.begin(), row.end(), [this](const Callout &a, const Callout &b) {
            return anchor(a.control).x() < anchor(b.control).x();
        });
        // Each box over (or under) its own button, pushed apart only as far
        // as needed to not overlap, and kept inside the canvas.
        const int n = int(row.size());
        constexpr double gap = 16;
        QList<double> x;
        for (const Callout &c : row)
            x << anchor(c.control).x() - kBoxW / 2.0;
        for (int i = 1; i < n; ++i)
            x[i] = std::max(x[i], x[i - 1] + kBoxW + gap);
        if (n && x.last() > width() - kBoxW)
            x.last() = width() - kBoxW;
        for (int i = n - 2; i >= 0; --i)
            x[i] = std::min(x[i], x[i + 1] - kBoxW - gap);
        if (n && x.first() < 0) {
            x.first() = 0;
            for (int i = 1; i < n; ++i)
                x[i] = std::max(x[i], x[i - 1] + kBoxW + gap);
        }
        for (int i = 0; i < n; ++i)
            row[i].box->setGeometry(int(x[i]), top ? 0 : height() - kBoxH, kBoxW, kBoxH);
    }
}

void PadCanvas::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = padRect();
    p.save();
    p.translate(r.topLeft());
    p.scale(r.width() / design().width(), r.width() / design().width());
    if (nes())
        paintNes(p);
    else
        paintSnes(p);
    p.restore();

    // Callout lines: straight out of the pad to its edge, then to the box.
    const double k = r.width() / design().width();
    for (const Callout &c : m_callouts) {
        // From the button's edge on the callout's side, so the dot never
        // covers the letter printed on it.
        const double edge = c.control == QLatin1String("DPAD") ? 0.0
            : (c.control == QLatin1String("SELECT") || c.control == QLatin1String("START")) ? 0.0
            : (c.control == QLatin1String("L") || c.control == QLatin1String("R")) ? 0.0
            : nes() ? 20.0 : 19.0;
        const QPointF a = anchor(c.control) + QPointF(0, (c.top ? -edge : edge) * k);
        const QRect b = c.box->geometry();
        const QPointF end(b.center().x(), c.top ? b.bottom() + 1 : b.top() - 1);
        const QPointF knee(a.x(), c.top ? r.top() - 18 : r.bottom() + 18);
        const bool hot = c.control == QLatin1String("DPAD") ? dpadActive()
                                                           : m_pressed.contains(c.control);
        const QColor col = hot ? kAmber : QColor(0x7a, 0x82, 0x90);
        p.setPen(QPen(col, hot ? 2.6 : 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(QPolygonF({a, knee, end}));
        p.setBrush(QColor(0x16, 0x18, 0x1d));
        p.drawEllipse(a, 4.5, 4.5);
        p.setBrush(col);
        p.drawEllipse(end, 3.5, 3.5);
    }
}

void PadCanvas::paintDpad(QPainter &p, const QPointF &c, double w, double l, const QColor &col)
{
    const QRectF v(c.x() - w / 2, c.y() - l / 2, w, l), h(c.x() - l / 2, c.y() - w / 2, l, w);
    // Glow under the pressed arm(s).
    const double off = l / 2 - w / 2;
    if (m_dir[0])
        glow(p, QPointF(c.x() + m_dir[0] * off, c.y()), w * 0.75);
    if (m_dir[1])
        glow(p, QPointF(c.x(), c.y() + m_dir[1] * off), w * 0.75);
    QPainterPath cross;
    cross.addRoundedRect(v, 3, 3);
    cross.addRoundedRect(h, 3, 3);
    cross = cross.simplified();
    p.setPen(QPen(QColor(0x45, 0x49, 0x52), 1.2));
    p.setBrush(col);
    p.drawPath(cross);
    // Lit arm tips.
    p.setPen(Qt::NoPen);
    p.setBrush(kAmber);
    const double t = w * 0.9;
    if (m_dir[0] < 0) p.drawRoundedRect(QRectF(h.left(), h.top(), t, w), 3, 3);
    if (m_dir[0] > 0) p.drawRoundedRect(QRectF(h.right() - t, h.top(), t, w), 3, 3);
    if (m_dir[1] < 0) p.drawRoundedRect(QRectF(v.left(), v.top(), w, t), 3, 3);
    if (m_dir[1] > 0) p.drawRoundedRect(QRectF(v.left(), v.bottom() - t, w, t), 3, 3);
    // the dimple in the middle
    p.setBrush(QColor(0, 0, 0, 70));
    p.drawEllipse(c, w * 0.28, w * 0.28);
}

void PadCanvas::paintRoundButton(QPainter &p, const QString &name, const QPointF &c, double r,
                                 const QColor &col, const QString &letter,
                                 const QColor &letterCol)
{
    const bool on = m_pressed.contains(name);
    if (on)
        glow(p, c, r);
    QRadialGradient g(c - QPointF(r * 0.35, r * 0.4), r * 1.4);
    g.setColorAt(0, (on ? col.lighter(150) : col.lighter(125)));
    g.setColorAt(1, (on ? col.lighter(115) : col.darker(115)));
    p.setPen(on ? QPen(kAmber, 2.5) : QPen(col.darker(150), 1.0));
    p.setBrush(g);
    p.drawEllipse(c, r, r);
    if (!letter.isEmpty()) {
        QFont f = p.font();
        f.setBold(true);
        f.setPixelSize(int(r * 0.9));
        p.setFont(f);
        p.setPen(letterCol);
        p.drawText(QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r), Qt::AlignCenter, letter);
    }
}

void PadCanvas::paintNes(QPainter &p)
{
    // shadow, body, the black face
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 110));
    p.drawRoundedRect(QRectF(3, 7, 420, 180), 12, 12);
    p.setPen(QPen(QColor(0x8b, 0x90, 0x9a), 1.5));
    p.setBrush(bodyGradient(180));
    p.drawRoundedRect(QRectF(0, 0, 420, 180), 10, 10);
    QFont f = p.font();
    f.setBold(true);
    f.setPixelSize(15);
    p.setFont(f);
    p.setPen(QColor(0xc4, 0x1e, 0x1e));
    p.drawText(QPointF(22, 28), QStringLiteral("NES"));
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x1b, 0x1d, 0x22));
    p.drawRoundedRect(QRectF(14, 40, 392, 126), 6, 6);

    paintDpad(p, QPointF(85, 103), 28, 88, QColor(0x0c, 0x0d, 0x0f));

    // the grey stripes, labels and the select/start well
    p.setBrush(QColor(0x9e, 0xa2, 0xaa));
    for (int i = 0; i < 4; ++i)
        p.drawRoundedRect(QRectF(150, 52 + i * 10, 120, 6), 2, 2);
    f.setPixelSize(9);
    p.setFont(f);
    p.setPen(QColor(0xd8, 0x2a, 0x2a));
    p.drawText(QRectF(160, 94, 44, 12), Qt::AlignCenter, QStringLiteral("SELECT"));
    p.drawText(QRectF(216, 94, 44, 12), Qt::AlignCenter, QStringLiteral("START"));
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0xc9, 0xcc, 0xd3));
    p.drawRoundedRect(QRectF(152, 110, 116, 36), 8, 8);
    for (const auto &[name, x] : {std::pair{QStringLiteral("SELECT"), 182.0},
                                  std::pair{QStringLiteral("START"), 238.0}}) {
        const bool on = m_pressed.contains(name);
        if (on)
            glow(p, QPointF(x, 128), 12);
        p.setPen(on ? QPen(kAmber, 2.5) : Qt::NoPen);
        p.setBrush(on ? QColor(0x55, 0x48, 0x30) : QColor(0x22, 0x24, 0x29));
        p.drawRoundedRect(QRectF(x - 16, 122, 32, 12), 6, 6);
    }

    // B and A in their grey squares, letters underneath
    for (const auto &[name, x] : {std::pair{QStringLiteral("B"), 305.0},
                                  std::pair{QStringLiteral("A"), 365.0}}) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xc9, 0xcc, 0xd3));
        p.drawRoundedRect(QRectF(x - 26, 92, 52, 52), 6, 6);
        paintRoundButton(p, name, QPointF(x, 118), 20, QColor(0xd0, 0x22, 0x22), QString(),
                         QColor());
        f.setPixelSize(14);
        p.setFont(f);
        p.setPen(QColor(0xd8, 0x2a, 0x2a));
        p.drawText(QRectF(x + 14, 146, 20, 16), Qt::AlignCenter, name);
    }
}

void PadCanvas::paintSnes(QPainter &p)
{
    // shoulder buttons behind the body
    for (const auto &[name, x] : {std::pair{QStringLiteral("L"), 40.0},
                                  std::pair{QStringLiteral("R"), 270.0}}) {
        const bool on = m_pressed.contains(name);
        if (on)
            glow(p, QPointF(x + 65, 12), 28);
        p.setPen(on ? QPen(kAmber, 2.5) : QPen(QColor(0x7e, 0x83, 0x8c), 1.2));
        p.setBrush(on ? QColor(0xe8, 0xc8, 0x90) : QColor(0x9f, 0xa3, 0xab));
        p.drawRoundedRect(QRectF(x, 0, 130, 44), 18, 18);
    }
    QPainterPath body, l, r, mid;
    l.addEllipse(QPointF(112, 108), 92, 92);
    r.addEllipse(QPointF(328, 108), 92, 92);
    mid.addRect(QRectF(112, 16, 216, 184));
    body = l.united(r).united(mid);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 110));
    p.drawPath(body.translated(3, 7));
    p.setPen(QPen(QColor(0x8b, 0x90, 0x9a), 1.5));
    p.setBrush(bodyGradient(200));
    p.drawPath(body);

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0xb0, 0xb4, 0xbc));
    p.drawEllipse(QPointF(112, 108), 54, 54);
    paintDpad(p, QPointF(112, 108), 26, 86, QColor(0x1e, 0x20, 0x26));

    QFont f = p.font();
    f.setBold(true);
    f.setPixelSize(9);
    for (const auto &[name, x] : {std::pair{QStringLiteral("SELECT"), 196.0},
                                  std::pair{QStringLiteral("START"), 244.0}}) {
        const bool on = m_pressed.contains(name);
        if (on)
            glow(p, QPointF(x, 122), 12);
        p.save();
        p.translate(x, 122);
        p.rotate(-28);
        p.setPen(on ? QPen(kAmber, 2.5) : Qt::NoPen);
        p.setBrush(on ? QColor(0x8a, 0x70, 0x40) : QColor(0x5a, 0x5f, 0x69));
        p.drawRoundedRect(QRectF(-17, -5.5, 34, 11), 5.5, 5.5);
        p.restore();
        p.setFont(f);
        p.setPen(QColor(0x6b, 0x70, 0x80));
        p.drawText(QRectF(x - 24, 136, 48, 12), Qt::AlignCenter, name);
    }

    // the button diamond in its darker well
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0xa4, 0xa8, 0xb0));
    p.drawEllipse(QPointF(328, 108), 68, 68);
    paintRoundButton(p, QStringLiteral("X"), QPointF(328, 66), 19, QColor(0x3b, 0x6f, 0xd8),
                     QStringLiteral("X"), QColor(0x1d, 0x3c, 0x80));
    paintRoundButton(p, QStringLiteral("Y"), QPointF(286, 108), 19, QColor(0x2f, 0xa8, 0x5a),
                     QStringLiteral("Y"), QColor(0x15, 0x5a, 0x2e));
    paintRoundButton(p, QStringLiteral("A"), QPointF(370, 108), 19, QColor(0xd8, 0x3a, 0x3a),
                     QStringLiteral("A"), QColor(0x7a, 0x15, 0x15));
    paintRoundButton(p, QStringLiteral("B"), QPointF(328, 150), 19, QColor(0xf0, 0xc0, 0x30),
                     QStringLiteral("B"), QColor(0x80, 0x60, 0x10));
}

// ---- the dialog -------------------------------------------------------------

GamepadMapDialog::GamepadMapDialog(const GamepadMap &map, Gamepad *pad, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Gamepad buttons"));
    setObjectName(QStringLiteral("padDialog"));
    setAttribute(Qt::WA_StyledBackground);
    setStyleSheet(QStringLiteral(
        "#padDialog { background: #16181d; }"
        "QLabel { color: #e6e9ef; }"
        "QLabel#caption { color: #8b93a1; font-size: 11px; letter-spacing: 1px; }"
        "QLabel#headline { font-size: 22px; font-weight: bold; }"
        "QLabel#hint { color: #a3abb8; font-size: 14px; }"
        "QLabel#note { color: #6b7380; font-size: 13px; }"
        "QFrame#callout { background: #1e2127; border: 1px solid #3a404b; border-radius: 10px; }"
        "QFrame#callout[hot=\"true\"] { background: #2e2716; border: 2px solid #f2a536; }"
        "QLabel#calloutName { color: #f2a536; font-size: 13px; font-weight: bold; "
        "  letter-spacing: 1px; }"
        "QLabel#calloutNum { color: #6b7380; font-size: 11px; }"
        "QLabel#calloutFixed { color: #c5cad3; font-size: 15px; }"
        "QComboBox { font-size: 15px; min-height: 36px; }"
        "QPushButton { min-height: 48px; padding: 0 20px; font-size: 15px; border-radius: 10px; "
        "  background: #2a2e36; color: #e6e9ef; border: 1px solid #3a404b; }"
        "QPushButton:pressed { background: #3a4150; }"
        "QPushButton#save { font-weight: bold; background: #f2a536; color: #1a1a1a; border: 0; "
        "  min-width: 140px; }"
        "QPushButton#styleBtn { min-height: 36px; min-width: 72px; border-radius: 8px; "
        "  background: transparent; color: #a3abb8; border: 0; font-family: monospace; }"
        "QPushButton#styleBtn:checked { background: #f2a536; color: #1a1a1a; font-weight: bold; }"
        "QFrame#seg { background: #1e2127; border-radius: 10px; }"));

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(24, 18, 24, 18);
    lay->setSpacing(10);

    auto *head = new QHBoxLayout;
    auto *titles = new QVBoxLayout;
    titles->setSpacing(2);
    auto *cap = new QLabel(QStringLiteral("GAMEPAD"), this);
    cap->setObjectName(QStringLiteral("caption"));
    auto *headline = new QLabel(QStringLiteral("What does each button do?"), this);
    headline->setObjectName(QStringLiteral("headline"));
    m_hint = new QLabel(this);
    m_hint->setObjectName(QStringLiteral("hint"));
    titles->addWidget(cap);
    titles->addWidget(headline);
    titles->addWidget(m_hint);
    head->addLayout(titles, 1);
    auto *seg = new QFrame(this);
    seg->setObjectName(QStringLiteral("seg"));
    auto *segLay = new QHBoxLayout(seg);
    segLay->setContentsMargins(4, 4, 4, 4);
    segLay->setSpacing(4);
    m_nesBtn = new QPushButton(QStringLiteral("NES"), seg);
    m_snesBtn = new QPushButton(QStringLiteral("SNES"), seg);
    for (QPushButton *b : {m_nesBtn, m_snesBtn}) {
        b->setObjectName(QStringLiteral("styleBtn"));
        b->setCheckable(true);
        b->setAutoExclusive(true);
        segLay->addWidget(b);
    }
    head->addWidget(seg, 0, Qt::AlignTop);
    lay->addLayout(head);

    m_canvas = new PadCanvas(this);
    lay->addWidget(m_canvas, 1);

    m_others = new QWidget(this);
    m_othersLay = new QHBoxLayout(m_others);
    m_othersLay->setContentsMargins(0, 0, 0, 0);
    m_othersLay->setSpacing(10);
    auto *othersCap = new QLabel(QStringLiteral("OTHER\nBUTTONS"), m_others);
    othersCap->setObjectName(QStringLiteral("caption"));
    m_othersLay->addWidget(othersCap);
    m_othersLay->addStretch(1);
    lay->addWidget(m_others);

    auto *foot = new QHBoxLayout;
    auto *note = new QLabel(QStringLiteral(
        "The D-pad always jogs X and Y. Starting a program is never on the pad."), this);
    note->setObjectName(QStringLiteral("note"));
    note->setWordWrap(true);
    foot->addWidget(note, 1);
    auto *reset = new QPushButton(QStringLiteral("Reset to defaults"), this);
    reset->setToolTip(QStringLiteral("The layout phobiCCCP ships with"));
    auto *cancel = new QPushButton(QStringLiteral("Cancel"), this);
    auto *save = new QPushButton(QStringLiteral("Save"), this);
    save->setObjectName(QStringLiteral("save"));
    save->setDefault(true);
    foot->addWidget(reset);
    foot->addWidget(cancel);
    foot->addWidget(save);
    lay->addLayout(foot);
    connect(reset, &QPushButton::clicked, this, [this] { build(gamepadmap::defaults()); });
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(save, &QPushButton::clicked, this, &QDialog::accept);

    const QString who = pad && pad->isConnected() ? pad->deviceName() : QString();
    m_hint->setText(who.isEmpty()
                        ? QStringLiteral("No gamepad connected. Plug one in, then press a button "
                                         "to find it.")
                        : QStringLiteral("Press a button on %1: it lights up and its picker is "
                                         "selected.").arg(who));
    if (pad) {
        connect(pad, &Gamepad::buttonChanged, this, [this](int number, bool pressed) {
            if (pressed)
                selectButton(number);
            setPressed(number, pressed);
        });
        connect(pad, &Gamepad::axisChanged, this, [this](int number, int value) {
            showAxis(number, value > 16000 ? 1 : value < -16000 ? -1 : 0);
        });
        connect(pad, &Gamepad::connected, this, [this](const QString &name) {
            m_hint->setText(QStringLiteral("Press a button on %1: it lights up and its picker "
                                           "is selected.").arg(name));
        });
    }

    m_style = QSettings().value(QStringLiteral("ui/padStyle")).toString() == QLatin1String("nes")
        ? QStringLiteral("nes") : QStringLiteral("snes");
    (m_style == QLatin1String("nes") ? m_nesBtn : m_snesBtn)->setChecked(true);
    for (QPushButton *b : {m_nesBtn, m_snesBtn})
        connect(b, &QPushButton::toggled, this, [this, b](bool on) {
            if (!on)
                return;
            const GamepadMap keep = mapping();
            m_style = b == m_nesBtn ? QStringLiteral("nes") : QStringLiteral("snes");
            build(keep);
        });
    build(map);
    resize(1180, 740);
}

GamepadMapDialog::Slot GamepadMapDialog::makeSlot(int number, const QString &control,
                                                  const QString &action, QWidget *parent)
{
    Slot s;
    s.number = number;
    s.control = control;
    s.box = new QFrame(parent);
    s.box->setObjectName(QStringLiteral("callout"));
    s.box->setAttribute(Qt::WA_StyledBackground);
    auto *v = new QVBoxLayout(s.box);
    v->setContentsMargins(10, 6, 10, 8);
    v->setSpacing(4);
    auto *top = new QHBoxLayout;
    QString label = QStringLiteral("BUTTON %1").arg(number);
    for (const Control &c : controls(m_style))
        if (control == QLatin1String(c.name))
            label = QString::fromLatin1(c.label);
    auto *name = new QLabel(label, s.box);
    name->setObjectName(QStringLiteral("calloutName"));
    top->addWidget(name);
    top->addStretch(1);
    if (number >= 0) {
        auto *num = new QLabel(QStringLiteral("#%1").arg(number), s.box);
        num->setObjectName(QStringLiteral("calloutNum"));
        top->addWidget(num);
    }
    v->addLayout(top);
    if (control == QLatin1String("DPAD")) {
        auto *fixed = new QLabel(QStringLiteral("Jog X / Y"), s.box);
        fixed->setObjectName(QStringLiteral("calloutFixed"));
        v->addWidget(fixed);
        return s;
    }
    s.combo = new QComboBox(s.box);
    s.combo->addItem(QStringLiteral("(nothing)"), kNone);
    for (const QString &a : gamepadmap::actions())
        s.combo->addItem(gamepadmap::label(a), a);
    int i = s.combo->findData(action);
    if (i < 0 && !action.isEmpty() && action != kNone) {
        // Typed into the settings by hand and not an action: keep it visible
        // rather than silently turning it into something else.
        s.combo->addItem(QStringLiteral("%1 (unknown)").arg(action), action);
        i = s.combo->count() - 1;
    }
    s.combo->setCurrentIndex(qMax(0, i));
    v->addWidget(s.combo);
    return s;
}

void GamepadMapDialog::build(const GamepadMap &map)
{
    for (const Slot &s : std::as_const(m_slots))
        delete s.box.data();
    m_slots.clear();
    m_canvas->setPadStyle(m_style);

    QList<PadCanvas::Callout> callouts;
    QSet<int> shown;
    for (const Control &c : controls(m_style)) {
        const QString name = QString::fromLatin1(c.name);
        const int number = name == QLatin1String("DPAD") ? -1 : layoutNumber(m_style, name);
        if (name != QLatin1String("DPAD") && number < 0)
            continue;
        Slot s = makeSlot(number, name, map.value(number, kNone), m_canvas);
        callouts << PadCanvas::Callout{name, s.box, c.top};
        s.box->show();
        m_slots << s;
        if (number >= 0)
            shown.insert(number);
    }
    m_canvas->setCallouts(callouts);

    QList<int> extra;
    for (auto it = map.cbegin(); it != map.cend(); ++it)
        if (!shown.contains(it.key()))
            extra << it.key();
    std::sort(extra.begin(), extra.end());
    for (int n : extra) {
        Slot s = makeSlot(n, QString(), map.value(n), m_others);
        s.box->setFixedSize(kBoxW, kBoxH);
        m_othersLay->insertWidget(m_othersLay->count() - 1, s.box);
        m_slots << s;
    }
    m_others->setVisible(!extra.isEmpty());
}

void GamepadMapDialog::showAxis(int axis, int dir)
{
    if (axis < 0 || axis > 1)
        return;
    m_canvas->setAxis(axis, dir);
    for (const Slot &s : std::as_const(m_slots))
        if (s.control == QLatin1String("DPAD") && s.box) {
            s.box->setProperty("hot", m_canvas->dpadActive());
            s.box->style()->unpolish(s.box);
            s.box->style()->polish(s.box);
        }
}

int GamepadMapDialog::slotFor(int number) const
{
    for (int i = 0; i < m_slots.size(); ++i)
        if (m_slots.at(i).number == number && m_slots.at(i).combo)
            return i;
    return -1;
}

void GamepadMapDialog::setPressed(int number, bool on)
{
    const int i = slotFor(number);
    if (i < 0)
        return;
    const Slot &s = m_slots.at(i);
    if (!s.control.isEmpty())
        m_canvas->setPressed(s.control, on);
    s.box->setProperty("hot", on);
    s.box->style()->unpolish(s.box);
    s.box->style()->polish(s.box);
}

void GamepadMapDialog::selectButton(int number)
{
    int i = slotFor(number);
    if (i < 0) {
        Slot s = makeSlot(number, QString(), kNone, m_others);
        s.box->setFixedSize(kBoxW, kBoxH);
        m_othersLay->insertWidget(m_othersLay->count() - 1, s.box);
        m_others->show();
        m_slots << s;
        i = int(m_slots.size()) - 1;
    }
    if (QComboBox *c = m_slots.at(i).combo)
        c->setFocus();
}

QString GamepadMapDialog::actionFor(int number) const
{
    const int i = slotFor(number);
    return i < 0 ? QString() : m_slots.at(i).combo->currentData().toString();
}

void GamepadMapDialog::setAction(int number, const QString &action)
{
    const int i = slotFor(number);
    if (i < 0)
        return;
    QComboBox *c = m_slots.at(i).combo;
    const int k = c->findData(action);
    if (k >= 0)
        c->setCurrentIndex(k);
}

GamepadMap GamepadMapDialog::mapping() const
{
    GamepadMap map;
    for (const Slot &s : m_slots) {
        if (!s.combo)
            continue;
        const QString action = s.combo->currentData().toString();
        if (action != kNone)
            map.insert(s.number, action);
    }
    return map;
}

} // namespace c2d
