#include "toolpathform.h"
#include "c2ddocument.h"
#include "tooldiagram.h"
#include "toollibrary.h"

#include <QAbstractButton>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>
#include <functional>

namespace c2d {

namespace {

// An on/off switch: a pill with a knob, big enough for a finger.
class Switch : public QAbstractButton
{
public:
    explicit Switch(QWidget *parent) : QAbstractButton(parent)
    {
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
    }
    QSize sizeHint() const override { return {56, 30}; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
        const bool on = isChecked();
        QColor track = on ? QColor(0xf2, 0xa5, 0x36) : QColor(0x3a, 0x40, 0x4b);
        if (!isEnabled())
            track.setAlpha(90);
        p.setPen(Qt::NoPen);
        p.setBrush(track);
        p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
        const double d = r.height() - 6;
        const double x = on ? r.right() - 3 - d : r.left() + 3;
        p.setBrush(isEnabled() ? QColor(0xf4, 0xf5, 0xf7) : QColor(0x8b, 0x93, 0xa1));
        p.drawEllipse(QRectF(x, r.top() + 3, d, d));
        if (hasFocus()) {
            p.setPen(QPen(QColor(0x4a, 0x90, 0xd9), 1.5));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
        }
    }
};

// Number fields ignore the wheel unless they have focus: scrolling the
// panel must not change a depth on the way past.
//
// They also take an expression: "stock_t + 0.5", "1/4 in", "plate_w / 2".
// One that names a parameter binds the field to it (the toolpath's
// `phobic_expr`), so the value follows when the parameter changes; a bound
// field shows its value in violet and its expression while it is edited.
class Spin : public QDoubleSpinBox
{
public:
    using QDoubleSpinBox::QDoubleSpinBox;
    using Eval = std::function<bool(const QString &, double *, QString *)>;
    void setEvaluator(const Eval &e) { m_eval = e; }
    // The field's own tooltip, shown under the binding and alone without one.
    void setBaseTip(const QString &tip)
    {
        m_baseTip = tip;
        const QString e = m_expr;
        m_expr = QStringLiteral("\x01");
        setBinding(e);
    }
    void setBinding(const QString &expr)
    {
        if (expr == m_expr)
            return;
        if (m_expr.isEmpty() && m_baseTip.isEmpty())
            m_baseTip = toolTip();
        m_expr = expr;
        setStyleSheet(expr.isEmpty() ? QString() : QStringLiteral("color: #c3a6ff;"));
        setToolTip(expr.isEmpty() ? m_baseTip
                                  : QStringLiteral("\u0192x  = %1\nType a number to unbind it.\n%2")
                                        .arg(expr, m_baseTip));
    }
    QString binding() const { return m_expr; }
    // The expression the last typed text was (empty: a plain number, or
    // the arrows). Typing or stepping starts a new edit; editingFinished
    // takes it.
    QString typed() const { return m_typed; }
    QString takeTyped() { QString t = m_typed; m_typed.clear(); return t; }

    QValidator::State validate(QString &input, int &pos) const override
    {
        const QValidator::State plain = QDoubleSpinBox::validate(input, pos);
        if (plain == QValidator::Acceptable || !m_eval)
            return plain;
        const QString t = strip(input);
        if (!isExpr(t))
            return plain;
        double v;
        return m_eval(t, &v, nullptr) ? QValidator::Acceptable : QValidator::Intermediate;
    }
    double valueFromText(const QString &text) const override
    {
        const QString t = strip(text);
        double v = 0;
        if (m_eval && isExpr(t) && m_eval(t, &v, nullptr)) {
            if (!m_stepping)
                m_typed = t;
            return property("depth").toBool() ? std::fabs(v) : v;
        }
        return QDoubleSpinBox::valueFromText(text);
    }
    void fixup(QString &input) const override
    {
        if (!isExpr(strip(input)))
            QDoubleSpinBox::fixup(input);
    }
    // The arrows give a plain number, so the field unbinds. The base stepBy
    // re-reads the shown text, which may still be the bound expression;
    // that read must not count as typing it.
    void stepBy(int steps) override
    {
        m_typed.clear();
        m_stepping = true;
        QDoubleSpinBox::stepBy(steps);
        m_stepping = false;
        m_typed.clear();
    }

protected:
    void wheelEvent(QWheelEvent *e) override
    {
        if (hasFocus())
            QDoubleSpinBox::wheelEvent(e);
        else
            e->ignore();
    }
    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() != Qt::Key_Return && e->key() != Qt::Key_Enter)
            m_typed.clear();
        QDoubleSpinBox::keyPressEvent(e);
    }
    void focusInEvent(QFocusEvent *e) override
    {
        QDoubleSpinBox::focusInEvent(e);
        if (!m_expr.isEmpty()) {
            lineEdit()->setText(m_expr);
            lineEdit()->selectAll();
        }
    }

private:
    QString strip(QString t) const
    {
        t = t.trimmed();
        if (!suffix().isEmpty() && t.endsWith(suffix().trimmed()))
            t.chop(suffix().trimmed().size());
        return t.trimmed();
    }
    static bool isExpr(const QString &t)
    {
        for (int i = 0; i < t.size(); ++i) {
            const QChar c = t.at(i);
            if (c.isLetter() || c == QLatin1Char('(') || c == QLatin1Char('*') || c == QLatin1Char('/')
                || c == QLatin1Char('^') || c == QLatin1Char('"') || (i > 0 && (c == QLatin1Char('+') || c == QLatin1Char('-'))))
                return true;
        }
        return false;
    }
    Eval m_eval;
    QString m_expr, m_baseTip;
    mutable QString m_typed;
    bool m_stepping = false;
};

class IntSpin : public QSpinBox
{
public:
    using QSpinBox::QSpinBox;

protected:
    void wheelEvent(QWheelEvent *e) override
    {
        if (hasFocus())
            QSpinBox::wheelEvent(e);
        else
            e->ignore();
    }
};

class Combo : public QComboBox
{
public:
    using QComboBox::QComboBox;

protected:
    void wheelEvent(QWheelEvent *e) override
    {
        if (hasFocus())
            QComboBox::wheelEvent(e);
        else
            e->ignore();
    }
};

// A text field marks itself "typed" while it holds text the user typed and
// has not committed yet (Enter or leaving it commits). updateValues() keeps
// such a field as it is; any other field, focused or not, shows the value
// it is given, so an undo with the cursor still in the field shows too.
void trackTyping(QLineEdit *le)
{
    QObject::connect(le, &QLineEdit::textEdited, le, [le] { le->setProperty("typed", true); });
    QObject::connect(le, &QLineEdit::editingFinished, le, [le] { le->setProperty("typed", false); });
}

void trackTyping(QAbstractSpinBox *sp)
{
    QObject::connect(sp->findChild<QLineEdit *>(), &QLineEdit::textEdited, sp, [sp] { sp->setProperty("typed", true); });
    QObject::connect(sp, &QAbstractSpinBox::editingFinished, sp, [sp] { sp->setProperty("typed", false); });
}

bool typing(const QWidget *w)
{
    return w->hasFocus() && w->property("typed").toBool();
}

QJsonValue flatGet(const QJsonObject &j, const QString &key)
{
    const int dot = key.indexOf(QLatin1Char('.'));
    if (dot < 0)
        return j.value(key);
    return j.value(key.left(dot)).toObject().value(key.mid(dot + 1));
}

void flatSet(QJsonObject &j, const QString &key, const QJsonValue &v)
{
    const int dot = key.indexOf(QLatin1Char('.'));
    if (dot < 0) {
        j.insert(key, v);
        return;
    }
    QJsonObject sub = j.value(key.left(dot)).toObject();
    sub.insert(key.mid(dot + 1), v);
    j.insert(key.left(dot), sub);
}

QStringList flatKeys(const QJsonObject &j)
{
    QStringList keys;
    for (auto it = j.constBegin(); it != j.constEnd(); ++it) {
        if (it.value().isObject()
            && (it.key() == QLatin1String("speeds") || it.key() == QLatin1String("tool"))) {
            const QJsonObject sub = it.value().toObject();
            for (auto s = sub.constBegin(); s != sub.constEnd(); ++s)
                if (!s.value().isObject() && !s.value().isArray())
                    keys << it.key() + QLatin1Char('.') + s.key();
        } else if (!it.value().isObject() && !it.value().isArray()) {
            keys << it.key();
        }
    }
    keys.sort();
    return keys;
}

double numberOf(const QJsonValue &v)
{
    return v.isString() ? v.toString().toDouble() : v.toDouble();
}

LibraryTool toolOf(const QJsonObject &tool)
{
    LibraryTool lt;
    lt.number = int(tool.value(QStringLiteral("number")).toDouble());
    lt.diameter = tool.value(QStringLiteral("diameter")).toDouble();
    lt.angle = tool.value(QStringLiteral("angle")).toDouble();
    lt.flutes = int(tool.value(QStringLiteral("flutes")).toDouble(2));
    lt.length = tool.value(QStringLiteral("length")).toDouble();
    lt.name = tool.value(QStringLiteral("name")).toString();
    const int type = int(tool.value(QStringLiteral("type")).toDouble());
    lt.kind = type == 1 ? QStringLiteral("ball")
            : (type == 2 || lt.angle > 0) ? QStringLiteral("vbit")
                                          : QStringLiteral("endmill");
    return lt;
}

// A number field with a finger-sized ▼ on the left and ▲ on the right
// instead of the spin box's own tiny arrows. Holding one repeats.
QWidget *stepper(QWidget *editor, QWidget *parent)
{
    auto *sb = qobject_cast<QAbstractSpinBox *>(editor);
    if (!sb)
        return editor;
    sb->setButtonSymbols(QAbstractSpinBox::NoButtons);
    sb->setAlignment(Qt::AlignCenter);
    auto *box = new QWidget(parent);
    auto *h = new QHBoxLayout(box);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(4);
    auto make = [box, sb](const QString &text, const QString &tip, bool up) {
        auto *b = new QToolButton(box);
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRepeat(true);
        b->setAutoRepeatDelay(400);
        b->setAutoRepeatInterval(90);
        b->setFixedSize(46, 40);
        b->setFocusPolicy(Qt::NoFocus);
        b->setStyleSheet(QStringLiteral(
            "QToolButton { font-size: 18px; border-radius: 6px; background: #2f343d; color: #d8dce4; }"
            "QToolButton:pressed { background: #f2a536; color: #16181d; }"
            "QToolButton:disabled { color: #5a616d; }"));
        QObject::connect(b, &QToolButton::clicked, sb, [sb, up] { up ? sb->stepUp() : sb->stepDown(); });
        return b;
    };
    h->addWidget(make(QStringLiteral("\u25bc"), QStringLiteral("Less"), false));
    h->addWidget(sb, 1);
    h->addWidget(make(QStringLiteral("\u25b2"), QStringLiteral("More"), true));
    box->setProperty("stepper", true);   // refreshEnabled() greys the whole group
    return box;
}

} // namespace

// One designed field: where it goes, what it is called and how it is edited.
struct ToolpathForm::Field {
    enum Kind { Depth, Number, Int, Bool, InvBool, Choice };
    QString card;
    QString label;
    Kind kind = Number;
    QString unit;
    double min = 0, max = 100000, step = 0.1;
    int decimals = 2;
    QList<QPair<QJsonValue, QString>> choices;
    QString tip;
    QString enabledBy;    // bool key; the field is greyed while it is off
};

static const QStringList kCardOrder = {
    QStringLiteral("depth"), QStringLiteral("cut"), QStringLiteral("entry"),
    QStringLiteral("tabs"), QStringLiteral("rest"), QStringLiteral("finish")};

static QString cardTitle(const QString &id)
{
    static const QHash<QString, QString> t = {
        {QStringLiteral("depth"), QStringLiteral("Depth")},
        {QStringLiteral("cut"), QStringLiteral("Cutting")},
        {QStringLiteral("entry"), QStringLiteral("Entry")},
        {QStringLiteral("tabs"), QStringLiteral("Tabs")},
        {QStringLiteral("rest"), QStringLiteral("Rest machining")},
        {QStringLiteral("finish"), QStringLiteral("Finishing passes")}};
    return t.value(id, id);
}

// The designed fields, in display order, for toolpath `type`.
static QList<QPair<QString, ToolpathForm::Field>> catalog(const QString &type)
{
    using F = ToolpathForm::Field;
    QList<QPair<QString, F>> c;
    auto add = [&c](const QString &key, F f) { c.append({key, f}); };
    auto mm = [](const QString &card, const QString &label, double max = 1000, const QString &tip = {}) {
        F f; f.card = card; f.label = label; f.kind = F::Number; f.unit = QStringLiteral("mm");
        f.max = max; f.tip = tip; return f;
    };
    auto depth = [](const QString &label, const QString &tip = {}) {
        F f; f.card = QStringLiteral("depth"); f.label = label; f.kind = F::Depth;
        f.unit = QStringLiteral("mm"); f.max = 1000; f.tip = tip; return f;
    };
    auto deg = [](const QString &card, const QString &label, double min, double max) {
        F f; f.card = card; f.label = label; f.kind = F::Number; f.unit = QStringLiteral("°");
        f.min = min; f.max = max; f.step = 1; f.decimals = 1; return f;
    };
    auto sw = [](const QString &card, const QString &label, const QString &tip = {}) {
        F f; f.card = card; f.label = label; f.kind = F::Bool; f.tip = tip; return f;
    };
    auto choice = [](const QString &card, const QString &label,
                     QList<QPair<QJsonValue, QString>> ch) {
        F f; f.card = card; f.label = label; f.kind = F::Choice; f.choices = ch; return f;
    };
    auto enabledBy = [](F f, const QString &key) { f.enabledBy = key; return f; };

    // Depth
    add(QStringLiteral("start_depth"), depth(QStringLiteral("Start"), QStringLiteral("Depth the first pass starts from")));
    add(QStringLiteral("end_depth"), depth(QStringLiteral("End")));
    add(QStringLiteral("cut_depth"), depth(QStringLiteral("Cut depth")));
    add(QStringLiteral("break_through"), depth(QStringLiteral("Break through"), QStringLiteral("Extra depth below the stock")));
    add(QStringLiteral("min_depth"), depth(QStringLiteral("Shallowest")));
    add(QStringLiteral("max_depth"), depth(QStringLiteral("Deepest")));
    add(QStringLiteral("stepdown"), mm(QStringLiteral("depth"), QStringLiteral("Per pass"), 100));
    add(QStringLiteral("depth_per_pass"), mm(QStringLiteral("depth"), QStringLiteral("Per pass"), 100));
    add(QStringLiteral("thread_depth"), mm(QStringLiteral("depth"), QStringLiteral("Thread depth"), 50,
                                           QStringLiteral("0 = from the pitch")));

    // Cutting
    if (type == QLatin1String("contour"))
        add(QStringLiteral("ofset_dir"), choice(QStringLiteral("cut"), QStringLiteral("Side"),
            {{-1, QStringLiteral("Inside")}, {0, QStringLiteral("On the line")}, {1, QStringLiteral("Outside")}}));
    if (type == QLatin1String("thread_toolpath"))
        add(QStringLiteral("side"), choice(QStringLiteral("cut"), QStringLiteral("Thread"),
            {{QStringLiteral("internal"), QStringLiteral("Internal (nut)")},
             {QStringLiteral("external"), QStringLiteral("External (bolt)")}}));
    else if (type == QLatin1String("circular_toolpath"))
        add(QStringLiteral("side"), choice(QStringLiteral("cut"), QStringLiteral("Shape"),
            {{QStringLiteral("inside"), QStringLiteral("Round pocket")},
             {QStringLiteral("outside"), QStringLiteral("Boss")}}));
    else
        add(QStringLiteral("side"), choice(QStringLiteral("cut"), QStringLiteral("Side"),
            {{QStringLiteral("outside"), QStringLiteral("Outside")}, {QStringLiteral("inside"), QStringLiteral("Inside")}}));
    add(QStringLiteral("flip_inside_outside"), sw(QStringLiteral("cut"), QStringLiteral("Cut inside the line")));
    add(QStringLiteral("mode"), choice(QStringLiteral("cut"), QStringLiteral("Engrave"),
        {{QStringLiteral("outline"), QStringLiteral("Outline")}, {QStringLiteral("fill"), QStringLiteral("Hatch fill")},
         {QStringLiteral("both"), QStringLiteral("Outline + fill")}}));
    if (type == QLatin1String("3d_finish_toolpath")) {
        F f = choice(QStringLiteral("cut"), QStringLiteral("Strategy"),
            {{QStringLiteral("raster"), QStringLiteral("Raster")},
             {QStringLiteral("waterline"), QStringLiteral("Waterline (Z-level)")}});
        f.tip = QStringLiteral("Raster: parallel passes over the surface. Waterline: one pass "
                               "around every wall per stepdown");
        add(QStringLiteral("strategy"), f);
        add(QStringLiteral("direction"), choice(QStringLiteral("cut"), QStringLiteral("Direction"),
            {{QStringLiteral("climb"), QStringLiteral("Climb")},
             {QStringLiteral("conventional"), QStringLiteral("Conventional")},
             {QStringLiteral("zigzag"), QStringLiteral("Zigzag")}}));
    }
    add(QStringLiteral("stepover"), mm(QStringLiteral("cut"), QStringLiteral("Stepover"), 100,
                                       QStringLiteral("Sideways step between passes")));
    add(QStringLiteral("stock_to_leave"), mm(QStringLiteral("cut"), QStringLiteral("Leave"), 50,
                                             QStringLiteral("Material left on the walls for a later pass")));
    add(QStringLiteral("climb"), sw(QStringLiteral("cut"), QStringLiteral("Climb milling")));
    add(QStringLiteral("chamfer_width"), mm(QStringLiteral("cut"), QStringLiteral("Chamfer width"), 50));
    add(QStringLiteral("tip_offset"), mm(QStringLiteral("cut"), QStringLiteral("Tip offset"), 20));
    add(QStringLiteral("boss_clearance"), mm(QStringLiteral("cut"), QStringLiteral("Boss clearance"), 100));
    add(QStringLiteral("pitch"), mm(QStringLiteral("cut"), QStringLiteral("Pitch"), 20));
    {
        F f; f.card = QStringLiteral("cut"); f.label = QStringLiteral("Passes"); f.kind = F::Int;
        f.min = 1; f.max = 50; add(QStringLiteral("passes"), f);
    }
    add(QStringLiteral("left_hand"), sw(QStringLiteral("cut"), QStringLiteral("Left-hand thread")));
    add(QStringLiteral("length"), mm(QStringLiteral("cut"), QStringLiteral("Slot length"), 500));
    add(QStringLiteral("angle"), deg(QStringLiteral("cut"), QStringLiteral("Angle"), -360, 360));
    add(QStringLiteral("line_spacing"), mm(QStringLiteral("cut"), QStringLiteral("Line spacing"), 50));
    add(QStringLiteral("crosshatch"), sw(QStringLiteral("cut"), QStringLiteral("Crosshatch")));
    add(QStringLiteral("min_length"), mm(QStringLiteral("cut"), QStringLiteral("Shortest stroke"), 1000));
    add(QStringLiteral("max_length"), mm(QStringLiteral("cut"), QStringLiteral("Longest stroke"), 1000));
    {
        F f; f.card = QStringLiteral("cut"); f.label = QStringLiteral("Stepover variation"); f.kind = F::Number;
        f.max = 1; f.step = 0.05; add(QStringLiteral("stepover_variation"), f);
    }
    add(QStringLiteral("peck_distance"), mm(QStringLiteral("cut"), QStringLiteral("Peck"), 100,
                                            QStringLiteral("0 = drill in one go")));
    {
        F f; f.card = QStringLiteral("cut"); f.label = QStringLiteral("Dwell"); f.kind = F::Number;
        f.unit = QStringLiteral("s"); f.max = 60; add(QStringLiteral("dwell"), f);
    }
    add(QStringLiteral("chip_break"), sw(QStringLiteral("cut"), QStringLiteral("Chip break")));
    add(QStringLiteral("chip_retract"), enabledBy(mm(QStringLiteral("cut"), QStringLiteral("Back off"), 10),
                                                  QStringLiteral("chip_break")));
    {
        F f; f.card = QStringLiteral("cut"); f.label = QStringLiteral("Feed"); f.kind = F::Number;
        f.unit = QStringLiteral("mm/min"); f.max = 20000; f.step = 50; f.decimals = 0;
        add(QStringLiteral("speeds.feedrate"), f);
        f.label = QStringLiteral("Plunge");
        add(QStringLiteral("speeds.plungerate"), f);
        f.label = QStringLiteral("Spindle"); f.unit = QStringLiteral("rpm"); f.max = 30000; f.step = 500;
        add(QStringLiteral("speeds.rpm"), f);
    }

    // Entry
    add(QStringLiteral("enable_ramping"), sw(QStringLiteral("entry"), QStringLiteral("Ramp in")));
    add(QStringLiteral("ramp_angle"), deg(QStringLiteral("entry"), QStringLiteral("Ramp angle"), 0, 45));
    add(QStringLiteral("helix_entry"), sw(QStringLiteral("entry"), QStringLiteral("Helix in")));
    add(QStringLiteral("helix_diameter"), enabledBy(mm(QStringLiteral("entry"), QStringLiteral("Helix Ø"), 200),
                                                    QStringLiteral("helix_entry")));
    add(QStringLiteral("helix_angle"), enabledBy(deg(QStringLiteral("entry"), QStringLiteral("Helix angle"), 0.5, 15),
                                                 QStringLiteral("helix_entry")));
    add(QStringLiteral("lead_radius"), mm(QStringLiteral("entry"), QStringLiteral("Lead-in arc"), 50,
                                          QStringLiteral("0 = straight in")));

    // Tabs
    {
        F f; f.card = QStringLiteral("tabs"); f.label = QStringLiteral("Tabs"); f.kind = F::InvBool;
        f.tip = QStringLiteral("Hold the part with small bridges on the last passes");
        add(QStringLiteral("ignore_tabs"), f);
    }
    add(QStringLiteral("tab_height"), mm(QStringLiteral("tabs"), QStringLiteral("Height"), 50));
    add(QStringLiteral("tab_width"), mm(QStringLiteral("tabs"), QStringLiteral("Width"), 500));

    // Rest machining
    add(QStringLiteral("rest_auto"), sw(QStringLiteral("rest"), QStringLiteral("Skip what the toolpaths above cut")));
    add(QStringLiteral("rest_overlap"), enabledBy(mm(QStringLiteral("rest"), QStringLiteral("Overlap"), 50),
                                                  QStringLiteral("rest_auto")));
    add(QStringLiteral("enable_rest"), sw(QStringLiteral("rest"), QStringLiteral("Only what a bigger tool left")));
    add(QStringLiteral("rest_diameter"), enabledBy(mm(QStringLiteral("rest"), QStringLiteral("Bigger tool Ø"), 100),
                                                   QStringLiteral("enable_rest")));

    // Finishing
    {
        F f; f.card = QStringLiteral("finish"); f.label = QStringLiteral("Passes"); f.kind = F::Int;
        f.min = 0; f.max = 10; add(QStringLiteral("finish_passes"), f);
    }
    add(QStringLiteral("finish_stepover"), mm(QStringLiteral("finish"), QStringLiteral("Stepover"), 10));
    add(QStringLiteral("spring_pass"), sw(QStringLiteral("finish"), QStringLiteral("Spring pass")));
    return c;
}

// Header keys and keys that must not be edited by hand.
static bool hiddenKey(const QString &k)
{
    static const QStringList h = {QStringLiteral("uuid"), QStringLiteral("type"), QStringLiteral("version"),
                                  QStringLiteral("toolpath_group"), QStringLiteral("name"),
                                  QStringLiteral("enabled")};
    return h.contains(k);
}

ToolpathForm::ToolpathForm(QWidget *parent)
    : QWidget(parent)
{
    m_lay = new QVBoxLayout(this);
    m_lay->setContentsMargins(0, 0, 0, 0);
    m_lay->setSpacing(8);
    setStyleSheet(QStringLiteral(
        "#tpCard { background: #23262e; border: 1px solid #2f343d; border-radius: 8px; }"
        "#tpCardHead { font-weight: bold; text-align: left; padding: 6px 8px; border: 0;"
        " background: transparent; color: #d8dce4; }"
        "#tpLabel { color: #aab1bd; }"
        "#tpToolText { color: #d8dce4; }"
        "QDoubleSpinBox, QSpinBox, QComboBox, QLineEdit { min-height: 34px; }"));
}

void ToolpathForm::clear()
{
    m_uuid.clear();
    m_json = QJsonObject();
    m_keys.clear();
    rebuild();
}

void ToolpathForm::setToolpath(const QString &uuid, const QString &type, const QJsonObject &json_)
{
    // The form only shows the keys a toolpath has. A 3D finish saved before
    // waterline existed has no `strategy`; offer it at its meaning (raster),
    // which is written back only with the user's next edit.
    QJsonObject json = json_;
    if (type == QLatin1String("3d_finish_toolpath") && !json.contains(QStringLiteral("strategy")))
        json.insert(QStringLiteral("strategy"), QStringLiteral("raster"));
    const QStringList keys = flatKeys(json);
    const bool same = uuid == m_uuid && type == m_type && keys == m_keys;
    m_uuid = uuid;
    m_type = type;
    m_json = json;
    m_keys = keys;
    if (same)
        updateValues();
    else
        rebuild();
}

QWidget *ToolpathForm::card(const QString &id, const QString &title, bool openByDefault, QVBoxLayout **body)
{
    auto *frame = new QFrame(this);
    frame->setObjectName(QStringLiteral("tpCard"));
    auto *v = new QVBoxLayout(frame);
    v->setContentsMargins(6, 2, 10, 8);
    v->setSpacing(6);
    auto *head = new QToolButton(frame);
    head->setObjectName(QStringLiteral("tpCardHead"));
    head->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    head->setCheckable(true);
    head->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    const QString setting = QStringLiteral("toolpathForm/open/") + id;
    const bool open = QSettings().value(setting, openByDefault).toBool();
    head->setChecked(open);
    head->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    head->setText(title.toUpper());
    v->addWidget(head);
    auto *inner = new QWidget(frame);
    auto *iv = new QVBoxLayout(inner);
    iv->setContentsMargins(4, 0, 0, 0);
    iv->setSpacing(6);
    inner->setVisible(open);
    v->addWidget(inner);
    connect(head, &QToolButton::toggled, inner, [head, inner, setting](bool on) {
        inner->setVisible(on);
        head->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        QSettings().setValue(setting, on);
    });
    *body = iv;
    return frame;
}

QWidget *ToolpathForm::editorFor(const QString &key, const Field &f)
{
    const QJsonValue v = flatGet(m_json, key);
    switch (f.kind) {
    case Field::Bool:
    case Field::InvBool: {
        auto *s = new Switch(this);
        const bool inv = f.kind == Field::InvBool;
        s->setProperty("inv", inv);
        s->setChecked(inv ? !v.toBool() : v.toBool());
        connect(s, &QAbstractButton::toggled, this, [this, key, inv](bool on) {
            commit(key, QJsonValue(inv ? !on : on));
        });
        return s;
    }
    case Field::Choice: {
        auto *c = new Combo(this);
        c->setFocusPolicy(Qt::StrongFocus); // the wheel scrolls the form, not the field
        int at = -1;
        for (int i = 0; i < f.choices.size(); ++i) {
            c->addItem(f.choices.at(i).second);
            if (f.choices.at(i).first == v
                || (v.isDouble() && f.choices.at(i).first.isDouble()
                    && qFuzzyCompare(v.toDouble() + 1, f.choices.at(i).first.toDouble() + 1)))
                at = i;
        }
        if (at < 0) {   // a value the file has that we have no name for: keep it
            c->addItem(v.isString() ? v.toString() : QString::number(v.toDouble()));
            at = c->count() - 1;
        }
        c->setCurrentIndex(at);
        QVariantList values;   // for updateValues()
        for (const auto &ch : f.choices)
            values.append(ch.first.toVariant());
        c->setProperty("choices", values);
        const auto choices = f.choices;
        connect(c, &QComboBox::activated, this, [this, key, choices](int i) {
            if (i < choices.size())
                commit(key, choices.at(i).first);
        });
        return c;
    }
    case Field::Int: {
        auto *s = new IntSpin(this);
        s->setFocusPolicy(Qt::StrongFocus); // the wheel scrolls the form, not the field
        s->setRange(int(f.min), int(f.max));
        s->setValue(int(numberOf(v)));
        s->setKeyboardTracking(false);
        connect(s, &QSpinBox::valueChanged, this, [this, key](int n) { commit(key, QJsonValue(n)); });
        trackTyping(s);
        return s;
    }
    case Field::Depth:
    case Field::Number: {
        auto *s = new Spin(this);
        s->setFocusPolicy(Qt::StrongFocus); // the wheel scrolls the form, not the field
        s->setDecimals(f.decimals);
        s->setRange(f.min, f.max);
        s->setSingleStep(f.step);
        if (!f.unit.isEmpty())
            s->setSuffix(QLatin1Char(' ') + f.unit);
        s->setKeyboardTracking(false);
        s->setProperty("depth", f.kind == Field::Depth);
        s->setValue(f.kind == Field::Depth ? std::fabs(numberOf(v)) : numberOf(v));
        s->setEvaluator([this](const QString &e, double *out, QString *why) {
            return m_doc && param::evaluateIn(*m_doc, e, out, why);
        });
        s->setBinding(bindingOf(key));
        const bool isDepth = f.kind == Field::Depth;
        // The JSON value for d, in the file's own convention.
        auto encode = [this, key, isDepth](double d, bool *negative) {
            const QJsonValue orig = flatGet(m_json, key);
            *negative = false;
            if (!isDepth)
                return orig.isString() ? QJsonValue(QString::number(d)) : QJsonValue(d);
            // Keep the file's depth convention: its sign, and string vs number.
            bool neg = orig.isString() ? orig.toString().trimmed().startsWith(QLatin1Char('-'))
                                       : orig.toDouble() < 0;
            if (!neg && numberOf(orig) == 0)
                for (const char *k : {"end_depth", "start_depth", "max_depth", "cut_depth"})
                    if (m_json.value(QLatin1String(k)).toString().trimmed().startsWith(QLatin1Char('-')))
                        neg = true;
            *negative = neg;
            const double out = neg ? -d : d;
            return orig.isString() ? QJsonValue(QString::number(out, 'f', 3)) : QJsonValue(out);
        };
        // Bind when the typed text names a parameter, unbind on a plain number.
        // The binding keeps the field's range, so a parameter changed later
        // is clamped to it the way typing into the field is.
        const double lo = f.min, hi = f.max;
        auto bindFor = [key, isDepth, lo, hi, this](const QString &typed, bool negative) {
            if (typed.isEmpty() || param::namesIn(typed).isEmpty())
                return QJsonObject();
            QJsonObject b;
            b.insert("expr", typed);
            b.insert("min", lo);
            b.insert("max", hi);
            if (isDepth)
                b.insert(negative ? "neg" : "abs", true);
            if (flatGet(m_json, key).isString())
                b.insert("str", true);
            return b;
        };
        connect(s, &QDoubleSpinBox::valueChanged, this, [this, s, key, encode, bindFor](double d) {
            bool negative = false;
            const QJsonValue out = encode(d, &negative);
            commitBound(key, out, bindFor(s->typed(), negative));
        });
        connect(s, &QAbstractSpinBox::editingFinished, this, [this, s, key, encode, bindFor] {
            // Same value, new expression (or a number over a bound field).
            const QString typed = s->takeTyped();
            const QString shown = s->text().trimmed();
            bool negative = false;
            const QJsonValue out = encode(s->value(), &negative);
            const QJsonObject b = bindFor(typed, negative);
            if (b.value("expr").toString() != bindingOf(key)
                && (!b.isEmpty() || !param::namesIn(shown).contains(bindingOf(key))))
                commitBound(key, out, b);
        });
        trackTyping(s);   // after the handlers: they commit first
        return s;
    }
    }
    return nullptr;
}

void ToolpathForm::rebuild()
{
    m_loading = true;
    while (QLayoutItem *it = m_lay->takeAt(0)) {
        if (QWidget *w = it->widget())
            w->deleteLater();
        delete it;
    }
    m_editors.clear();
    m_enabledBy.clear();
    m_toolIcon = m_toolText = nullptr;
    m_name = nullptr;
    if (m_uuid.isEmpty()) {
        m_loading = false;
        return;
    }

    // Header: name and enabled.
    {
        auto *row = new QHBoxLayout;
        m_name = new QLineEdit(m_json.value(QStringLiteral("name")).toString(), this);
        m_name->setStyleSheet(QStringLiteral("font-size: 19px; font-weight: 600;"));
        connect(m_name, &QLineEdit::editingFinished, this, [this] {
            const QString n = m_name->text().trimmed();
            if (!n.isEmpty() && n != m_json.value(QStringLiteral("name")).toString())
                commit(QStringLiteral("name"), n);
        });
        trackTyping(m_name);
        row->addWidget(m_name, 1);
        auto *on = new Switch(this);
        on->setChecked(m_json.value(QStringLiteral("enabled")).toBool(true));
        on->setToolTip(QStringLiteral("Cut this toolpath"));
        connect(on, &QAbstractButton::toggled, this, [this](bool b) { commit(QStringLiteral("enabled"), b); });
        m_editors.insert(QStringLiteral("enabled"), on);
        row->addWidget(on);
        auto *w = new QWidget(this);
        w->setLayout(row);
        row->setContentsMargins(0, 0, 0, 0);
        m_lay->addWidget(w);
    }

    // The tool, as a card with its outline and a Change button.
    if (m_json.value(QStringLiteral("tool")).isObject()) {
        auto *frame = new QFrame(this);
        frame->setObjectName(QStringLiteral("tpCard"));
        auto *h = new QHBoxLayout(frame);
        h->setContentsMargins(10, 8, 10, 8);
        m_toolIcon = new QLabel(frame);
        m_toolText = new QLabel(frame);
        m_toolText->setObjectName(QStringLiteral("tpToolText"));
        m_toolText->setWordWrap(true);
        auto *change = new QPushButton(QStringLiteral("Change…"), frame);
        change->setToolTip(QStringLiteral("Pick a bit and material from the tool library"));
        connect(change, &QPushButton::clicked, this, &ToolpathForm::changeToolRequested);
        h->addWidget(m_toolIcon);
        h->addWidget(m_toolText, 1);
        h->addWidget(change);
        m_lay->addWidget(frame);
    }

    // Designed cards, only with the fields this toolpath has.
    QSet<QString> designed;
    QHash<QString, QList<QPair<QString, Field>>> byCard;
    for (const auto &kf : catalog(m_type)) {
        if (!flatGet(m_json, kf.first).isUndefined() && !designed.contains(kf.first)) {
            byCard[kf.second.card].append(kf);
            designed.insert(kf.first);
        }
    }
    for (const QString &id : kCardOrder) {
        const auto fields = byCard.value(id);
        if (fields.isEmpty())
            continue;
        QVBoxLayout *body = nullptr;
        const bool open = id == QLatin1String("depth") || id == QLatin1String("cut")
                       || id == QLatin1String("tabs") || id == QLatin1String("rest");
        m_lay->addWidget(card(id, cardTitle(id), open, &body));
        for (const auto &kf : fields) {
            auto *row = new QHBoxLayout;
            row->setSpacing(8);
            auto *label = new QLabel(kf.second.label, this);
            label->setObjectName(QStringLiteral("tpLabel"));
            label->setToolTip(kf.second.tip.isEmpty() ? kf.first : kf.second.tip + QStringLiteral("\n(") + kf.first + QLatin1Char(')'));
            QWidget *ed = editorFor(kf.first, kf.second);
            if (auto *bs = dynamic_cast<Spin *>(ed))
                bs->setBaseTip(label->toolTip());
            else
                ed->setToolTip(label->toolTip());
            const bool isSwitch = kf.second.kind == Field::Bool || kf.second.kind == Field::InvBool;
            row->addWidget(label, 1);
            row->addWidget(stepper(ed, this), isSwitch ? 0 : 1, isSwitch ? Qt::AlignRight : Qt::Alignment());
            body->addLayout(row);
            m_editors.insert(kf.first, ed);
            if (!kf.second.enabledBy.isEmpty()) {
                m_enabledBy.insert(kf.first, kf.second.enabledBy);
                label->setProperty("forKey", kf.first);
                ed->setProperty("label", QVariant::fromValue<QObject *>(label));
            }
        }
    }

    // Advanced: every other key, typed.
    {
        QVBoxLayout *body = nullptr;
        QStringList rest;
        for (const QString &k : m_keys)
            if (!designed.contains(k) && !hiddenKey(k))
                rest << k;
        if (!rest.isEmpty()) {
            m_lay->addWidget(card(QStringLiteral("advanced"),
                                  QStringLiteral("Advanced (%1)").arg(rest.size()), false, &body));
            for (const QString &k : rest) {
                const QJsonValue v = flatGet(m_json, k);
                auto *row = new QHBoxLayout;
                auto *label = new QLabel(k, this);
                label->setObjectName(QStringLiteral("tpLabel"));
                row->addWidget(label, 1);
                QWidget *ed = nullptr;
                if (v.isBool()) {
                    Field f;
                    f.kind = Field::Bool;
                    ed = editorFor(k, f);
                    row->addWidget(ed, 0, Qt::AlignRight);
                } else if (v.isDouble()) {
                    Field f;
                    f.kind = Field::Number;
                    f.min = -1e6;
                    f.max = 1e6;
                    f.decimals = 3;
                    ed = editorFor(k, f);
                    row->addWidget(stepper(ed, this), 1);
                } else {
                    auto *e = new QLineEdit(v.toString(), this);
                    connect(e, &QLineEdit::editingFinished, this, [this, k, e] {
                        if (e->text() != flatGet(m_json, k).toString())
                            commit(k, e->text());
                    });
                    trackTyping(e);
                    ed = e;
                    row->addWidget(ed, 1);
                }
                if (auto *bs = dynamic_cast<Spin *>(ed))
                    bs->setBaseTip(k);
                else
                    ed->setToolTip(k);
                label->setToolTip(k);
                body->addLayout(row);
                m_editors.insert(k, ed);
            }
        }
    }
    m_lay->addStretch(1);
    m_loading = false;
    updateValues();
}

void ToolpathForm::updateValues()
{
    m_loading = true;
    if (m_name && !typing(m_name))
        m_name->setText(m_json.value(QStringLiteral("name")).toString());
    if (m_toolIcon) {
        const LibraryTool t = toolOf(m_json.value(QStringLiteral("tool")).toObject());
        m_toolIcon->setPixmap(toolKindIcon(t).pixmap(QSize(34, 52)));
        QString s = QStringLiteral("<b>T%1</b>  Ø %2 mm").arg(t.number).arg(t.diameter, 0, 'g', 4);
        if (t.angle > 0)
            s += QStringLiteral(" · %1°").arg(t.angle);
        if (t.flutes > 0)
            s += QStringLiteral(" · %1 fl").arg(t.flutes);
        if (!t.name.isEmpty())
            s += QStringLiteral("<br><span style='color:#8b93a1'>%1</span>").arg(t.name.toHtmlEscaped());
        m_toolText->setText(s);
    }
    for (auto it = m_editors.constBegin(); it != m_editors.constEnd(); ++it) {
        const QJsonValue v = flatGet(m_json, it.key());
        QWidget *w = it.value();
        const QSignalBlocker block(w);
        if (auto *sw = dynamic_cast<Switch *>(w)) {
            const bool inv = sw->property("inv").toBool();
            sw->setChecked(inv ? !v.toBool(true) : v.toBool(it.key() == QLatin1String("enabled")));
            sw->update();
        } else if (auto *sp = qobject_cast<QDoubleSpinBox *>(w)) {
            auto *bs = dynamic_cast<Spin *>(sp);
            if (!typing(sp)) {
                const double want = sp->property("depth").toBool() ? std::fabs(numberOf(v)) : numberOf(v);
                // Rewriting an unchanged focused field would only move its cursor.
                if (!sp->hasFocus() || std::fabs(sp->value() - want) > 0.5 * std::pow(10.0, -sp->decimals())
                    || (bs && bs->binding() != bindingOf(it.key()))) {
                    sp->setValue(want);
                    if (sp->hasFocus() && !bindingOf(it.key()).isEmpty())
                        sp->findChild<QLineEdit *>()->setText(bindingOf(it.key()));   // as focusInEvent shows it
                }
            }
            if (bs)
                bs->setBinding(bindingOf(it.key()));
        } else if (auto *ip = qobject_cast<QSpinBox *>(w)) {
            if (!typing(ip) && ip->value() != int(numberOf(v)))
                ip->setValue(int(numberOf(v)));
        } else if (auto *le = qobject_cast<QLineEdit *>(w)) {
            if (!typing(le) && le->text() != v.toString())
                le->setText(v.toString());
        } else if (auto *cb = qobject_cast<QComboBox *>(w)) {
            // Undo/redo of a side, mode or direction must show in the box too.
            const QVariantList choices = cb->property("choices").toList();
            int at = -1;
            for (int i = 0; i < choices.size() && at < 0; ++i) {
                const QJsonValue c = QJsonValue::fromVariant(choices.at(i));
                if (c == v || (v.isDouble() && c.isDouble()
                               && qFuzzyCompare(v.toDouble() + 1, c.toDouble() + 1)))
                    at = i;
            }
            if (at < 0) {   // a value with no name: the extra item, as built
                const QString text = v.isString() ? v.toString() : QString::number(v.toDouble());
                if (cb->count() > choices.size())
                    cb->setItemText(choices.size(), text);
                else
                    cb->addItem(text);
                at = int(choices.size());
            }
            cb->setCurrentIndex(at);
        }
    }
    m_loading = false;
    refreshEnabled();
}

void ToolpathForm::refreshEnabled()
{
    for (auto it = m_enabledBy.constBegin(); it != m_enabledBy.constEnd(); ++it) {
        const bool on = flatGet(m_json, it.value()).toBool();
        if (QWidget *ed = m_editors.value(it.key())) {
            QWidget *group = ed->parentWidget();
            (group && group->property("stepper").toBool() ? group : ed)->setEnabled(on);
            if (auto *label = qobject_cast<QWidget *>(ed->property("label").value<QObject *>()))
                label->setEnabled(on);
        }
    }
}

QString ToolpathForm::bindingOf(const QString &key) const
{
    return m_json.value(QStringLiteral("phobic_expr")).toObject().value(key).toObject()
        .value(QStringLiteral("expr")).toString();
}

void ToolpathForm::commitBound(const QString &key, const QJsonValue &v, const QJsonObject &binding)
{
    if (m_loading || m_uuid.isEmpty())
        return;
    QJsonObject binds = m_json.value(QStringLiteral("phobic_expr")).toObject();
    if (binding.isEmpty())
        binds.remove(key);
    else
        binds.insert(key, binding);
    QJsonObject j = m_json;
    flatSet(j, key, v);
    if (binds.isEmpty())
        j.remove(QStringLiteral("phobic_expr"));
    else
        j.insert(QStringLiteral("phobic_expr"), binds);
    if (j == m_json)
        return;
    m_json = j;
    refreshEnabled();
    emit edited(m_json);
}

void ToolpathForm::commit(const QString &key, const QJsonValue &v)
{
    if (m_loading || m_uuid.isEmpty())
        return;
    if (flatGet(m_json, key) == v)
        return;
    flatSet(m_json, key, v);
    refreshEnabled();
    emit edited(m_json);
}

} // namespace c2d
