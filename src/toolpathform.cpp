#include "toolpathform.h"
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
class Spin : public QDoubleSpinBox
{
public:
    using QDoubleSpinBox::QDoubleSpinBox;

protected:
    void wheelEvent(QWheelEvent *e) override
    {
        if (hasFocus())
            QDoubleSpinBox::wheelEvent(e);
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
    add(QStringLiteral("rest_auto"), sw(QStringLiteral("rest"), QStringLiteral("Start where the toolpaths above stopped")));
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

void ToolpathForm::setToolpath(const QString &uuid, const QString &type, const QJsonObject &json)
{
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
        const auto choices = f.choices;
        connect(c, &QComboBox::activated, this, [this, key, choices](int i) {
            if (i < choices.size())
                commit(key, choices.at(i).first);
        });
        return c;
    }
    case Field::Int: {
        auto *s = new QSpinBox(this);
        s->setRange(int(f.min), int(f.max));
        s->setValue(int(numberOf(v)));
        s->setKeyboardTracking(false);
        connect(s, &QSpinBox::valueChanged, this, [this, key](int n) { commit(key, QJsonValue(n)); });
        return s;
    }
    case Field::Depth:
    case Field::Number: {
        auto *s = new Spin(this);
        s->setDecimals(f.decimals);
        s->setRange(f.min, f.max);
        s->setSingleStep(f.step);
        if (!f.unit.isEmpty())
            s->setSuffix(QLatin1Char(' ') + f.unit);
        s->setKeyboardTracking(false);
        s->setProperty("depth", f.kind == Field::Depth);
        s->setValue(f.kind == Field::Depth ? std::fabs(numberOf(v)) : numberOf(v));
        const bool isDepth = f.kind == Field::Depth;
        connect(s, &QDoubleSpinBox::valueChanged, this, [this, key, isDepth](double d) {
            const QJsonValue orig = flatGet(m_json, key);
            if (!isDepth) {
                commit(key, orig.isString() ? QJsonValue(QString::number(d)) : QJsonValue(d));
                return;
            }
            // Keep the file's depth convention: its sign, and string vs number.
            bool negative = orig.isString() ? orig.toString().trimmed().startsWith(QLatin1Char('-'))
                                            : orig.toDouble() < 0;
            if (!negative && numberOf(orig) == 0)
                for (const char *k : {"end_depth", "start_depth", "max_depth", "cut_depth"})
                    if (m_json.value(QLatin1String(k)).toString().trimmed().startsWith(QLatin1Char('-')))
                        negative = true;
            const double out = negative ? -d : d;
            commit(key, orig.isString() ? QJsonValue(QString::number(out, 'f', 3)) : QJsonValue(out));
        });
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
                    ed = e;
                    row->addWidget(ed, 1);
                }
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
    if (m_name && !m_name->hasFocus())
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
            if (!sp->hasFocus())
                sp->setValue(sp->property("depth").toBool() ? std::fabs(numberOf(v)) : numberOf(v));
        } else if (auto *ip = qobject_cast<QSpinBox *>(w)) {
            if (!ip->hasFocus())
                ip->setValue(int(numberOf(v)));
        } else if (auto *le = qobject_cast<QLineEdit *>(w)) {
            if (!le->hasFocus())
                le->setText(v.toString());
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
