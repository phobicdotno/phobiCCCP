#include "parametric.h"
#include "c2ddocument.h"
#include "element.h"
#include <QJsonArray>
#include <QLineF>
#include <QRegularExpression>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace c2d {

// ---- expressions -------------------------------------------------------------
namespace param {
namespace {

struct Unit { const char *name; double factor; };
const Unit kUnits[] = {
    {"mm", 1.0}, {"cm", 10.0}, {"m", 1000.0}, {"in", 25.4}, {"inch", 25.4},
    {"ft", 304.8}, {"deg", 1.0}, {"rad", 180.0 / M_PI},
};
const char *const kFunctions[] = {
    "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "sqrt", "abs",
    "min", "max", "round", "floor", "ceil",
};

bool isUnit(const QString &s, double *factor = nullptr)
{
    for (const Unit &u : kUnits)
        if (s == QLatin1String(u.name)) {
            if (factor) *factor = u.factor;
            return true;
        }
    return false;
}

bool isFunction(const QString &s)
{
    for (const char *f : kFunctions)
        if (s == QLatin1String(f))
            return true;
    return false;
}

class Parser
{
public:
    Parser(const QString &s, const Lookup &lookup) : m_s(s), m_lookup(lookup) {}

    bool parse(double *out, QString *error)
    {
        skip();
        if (m_pos >= m_s.size())
            return fail(QStringLiteral("empty"), error);
        double v = 0;
        if (!expr(&v))
            return fail(m_error, error);
        skip();
        if (m_pos < m_s.size())
            return fail(QStringLiteral("unexpected '%1'").arg(m_s.mid(m_pos, 1)), error);
        if (!std::isfinite(v))
            return fail(QStringLiteral("not a finite number"), error);
        *out = v;
        return true;
    }

private:
    bool fail(const QString &why, QString *error)
    {
        if (error) *error = why;
        return false;
    }
    bool err(const QString &why)
    {
        if (m_error.isEmpty())
            m_error = why;
        return false;
    }
    void skip()
    {
        while (m_pos < m_s.size() && m_s.at(m_pos).isSpace())
            ++m_pos;
    }
    bool eat(QChar c)
    {
        skip();
        if (m_pos < m_s.size() && m_s.at(m_pos) == c) {
            ++m_pos;
            return true;
        }
        return false;
    }
    QString ident()
    {
        skip();
        const int start = m_pos;
        if (m_pos < m_s.size() && (m_s.at(m_pos).isLetter() || m_s.at(m_pos) == QLatin1Char('_'))) {
            ++m_pos;
            while (m_pos < m_s.size()
                   && (m_s.at(m_pos).isLetterOrNumber() || m_s.at(m_pos) == QLatin1Char('_')))
                ++m_pos;
        }
        return m_s.mid(start, m_pos - start);
    }
    // An optional unit at the current position: its factor, or 0 for none.
    double unit()
    {
        const int save = m_pos;
        skip();
        if (m_pos < m_s.size() && m_s.at(m_pos) == QLatin1Char('"')) {
            ++m_pos;
            return 25.4;
        }
        const QString id = ident();
        double f = 1;
        if (!id.isEmpty() && isUnit(id, &f))
            return f;
        m_pos = save;
        return 0;
    }

    bool expr(double *v)
    {
        if (!term(v))
            return false;
        for (;;) {
            if (eat(QLatin1Char('+'))) {
                double r;
                if (!term(&r)) return false;
                *v += r;
            } else if (eat(QLatin1Char('-'))) {
                double r;
                if (!term(&r)) return false;
                *v -= r;
            } else {
                return true;
            }
        }
    }
    // A unit after a product of plain numbers scales the whole product:
    // "1/4 in" is a quarter inch, "3 cm + 2 mm" is 32. With a name in the
    // product, "x * 2 in" is still x times 2 inches, but after a divide it
    // is ambiguous ("x / 2 in": by 2, or by 2 inches?), so it is refused
    // and asks for brackets; so is a unit right after a name, which is in
    // mm already.
    bool term(double *v)
    {
        int names = m_names;
        if (!unary(v))
            return false;
        bool named = m_names != names, lastNamed = named;
        bool divided = false, hasUnit = false;
        for (;;) {
            if (const double f = unit()) {
                // "1 cm / 4 in" or "2 in * 3 in" would scale the first number twice.
                if (hasUnit)
                    return err(QStringLiteral("one unit per product; put each number with its unit in brackets"));
                hasUnit = true;
                if (lastNamed)
                    return err(QStringLiteral("a unit can't follow a name; it is in mm already"));
                if (named && divided)
                    return err(QStringLiteral("put the unit in brackets with its number: x / (2 in)"));
                *v *= f;
            }
            const bool mul = eat(QLatin1Char('*'));
            if (!mul && !eat(QLatin1Char('/')))
                return true;
            double r;
            names = m_names;
            if (!unary(&r)) return false;
            lastNamed = m_names != names;
            named = named || lastNamed;
            divided = !mul;
            if (mul) {
                *v *= r;
            } else {
                if (r == 0) return err(QStringLiteral("division by zero"));
                *v /= r;
            }
        }
    }
    // Every nesting ("(", "-", "^", a call) goes through here: a crafted
    // expression of thousands of "(" must fail, not overflow the stack.
    bool unary(double *v)
    {
        if (m_depth >= 200)
            return err(QStringLiteral("nested too deeply"));
        ++m_depth;
        const bool ok = unaryInner(v);
        --m_depth;
        return ok;
    }
    // Unary minus binds looser than ^: -2^2 is -4, 2^-1 is 0.5.
    bool unaryInner(double *v)
    {
        if (eat(QLatin1Char('-'))) {
            if (!unary(v)) return false;
            *v = -*v;
            return true;
        }
        if (eat(QLatin1Char('+')))
            return unary(v);
        return power(v);
    }
    bool power(double *v)
    {
        if (!primary(v))
            return false;
        if (eat(QLatin1Char('^'))) {
            double r;
            if (!unary(&r)) return false;   // right-associative
            *v = std::pow(*v, r);
        }
        return true;
    }
    bool primary(double *v)
    {
        skip();
        if (m_pos >= m_s.size())
            return err(QStringLiteral("unexpected end"));
        const QChar c = m_s.at(m_pos);
        if (c == QLatin1Char('(')) {
            ++m_pos;
            if (!expr(v)) return false;
            if (!eat(QLatin1Char(')'))) return err(QStringLiteral("missing ')'"));
            return true;
        }
        if (c.isDigit() || c == QLatin1Char('.')) {
            const int start = m_pos;
            // A decimal comma (Norwegian keyboards) only outside a function
            // call, where the comma separates the arguments.
            auto decimalComma = [&] {
                return m_calls == 0 && m_s.at(m_pos) == QLatin1Char(',') && m_pos + 1 < m_s.size()
                       && m_s.at(m_pos + 1).isDigit();
            };
            while (m_pos < m_s.size() && (m_s.at(m_pos).isDigit() || m_s.at(m_pos) == QLatin1Char('.')
                                          || decimalComma()))
                ++m_pos;
            // Exponent: 1e-3 (but not "1 in", which is a unit).
            if (m_pos + 1 < m_s.size() && (m_s.at(m_pos) == QLatin1Char('e') || m_s.at(m_pos) == QLatin1Char('E'))
                && (m_s.at(m_pos + 1).isDigit()
                    || ((m_s.at(m_pos + 1) == QLatin1Char('-') || m_s.at(m_pos + 1) == QLatin1Char('+'))
                        && m_pos + 2 < m_s.size() && m_s.at(m_pos + 2).isDigit()))) {
                m_pos += 2;
                while (m_pos < m_s.size() && m_s.at(m_pos).isDigit())
                    ++m_pos;
            }
            QString num = m_s.mid(start, m_pos - start);
            num.replace(QLatin1Char(','), QLatin1Char('.'));
            bool ok = false;
            *v = num.toDouble(&ok);
            if (!ok) return err(QStringLiteral("bad number '%1'").arg(num));
            return true;
        }
        const QString id = ident();
        if (id.isEmpty())
            return err(QStringLiteral("unexpected '%1'").arg(c));
        if (isFunction(id)) {
            if (!eat(QLatin1Char('('))) return err(QStringLiteral("%1 needs ( )").arg(id));
            QVector<double> args;
            if (!eat(QLatin1Char(')'))) {
                ++m_calls;
                bool commas = false, semis = false;
                for (;;) {
                    double a;
                    if (!expr(&a)) return false;
                    args << a;
                    if (eat(QLatin1Char(',')))
                        commas = true;
                    else if (eat(QLatin1Char(';')))
                        semis = true;
                    else
                        break;
                }
                --m_calls;
                // "max(2,5; 3)": 2,5 meant as a decimal comma reads as two values.
                if (commas && semis)
                    return err(QStringLiteral("separate the values with , or with ; - not both"));
                if (!eat(QLatin1Char(')'))) return err(QStringLiteral("missing ')'"));
            }
            return call(id, args, v);
        }
        if (id == QLatin1String("pi")) {
            *v = M_PI;
            return true;
        }
        if (isUnit(id))
            return err(QStringLiteral("'%1' needs a number before it").arg(id));
        if (!m_lookup || !m_lookup(id, v))
            return err(QStringLiteral("unknown name '%1'").arg(id));
        ++m_names;
        return true;
    }
    bool call(const QString &f, const QVector<double> &a, double *v)
    {
        auto need = [&](int n) {
            if (a.size() == n) return true;
            return err(QStringLiteral("%1 takes %2 value(s)").arg(f).arg(n));
        };
        const double d2r = M_PI / 180.0, r2d = 180.0 / M_PI;
        if (f == QLatin1String("min") || f == QLatin1String("max")) {
            if (a.isEmpty()) return err(QStringLiteral("%1 needs values").arg(f));
            *v = f == QLatin1String("min") ? *std::min_element(a.begin(), a.end())
                                           : *std::max_element(a.begin(), a.end());
            return true;
        }
        if (f == QLatin1String("atan2")) {
            if (!need(2)) return false;
            *v = std::atan2(a[0], a[1]) * r2d;
            return true;
        }
        if (!need(1)) return false;
        const double x = a[0];
        if (f == QLatin1String("sin")) *v = std::sin(x * d2r);
        else if (f == QLatin1String("cos")) *v = std::cos(x * d2r);
        else if (f == QLatin1String("tan")) *v = std::tan(x * d2r);
        else if (f == QLatin1String("asin")) *v = std::asin(x) * r2d;
        else if (f == QLatin1String("acos")) *v = std::acos(x) * r2d;
        else if (f == QLatin1String("atan")) *v = std::atan(x) * r2d;
        else if (f == QLatin1String("sqrt")) {
            if (x < 0) return err(QStringLiteral("sqrt of a negative number"));
            *v = std::sqrt(x);
        }
        else if (f == QLatin1String("abs")) *v = std::fabs(x);
        else if (f == QLatin1String("round")) *v = std::round(x);
        else if (f == QLatin1String("floor")) *v = std::floor(x);
        else if (f == QLatin1String("ceil")) *v = std::ceil(x);
        if (!std::isfinite(*v))
            return err(QStringLiteral("%1 is out of range here").arg(f));
        return true;
    }

    const QString &m_s;
    const Lookup &m_lookup;
    int m_pos = 0;
    int m_calls = 0;        // function calls open around the current position
    int m_depth = 0;        // unary() calls open, for the nesting limit
    int m_names = 0;        // names read so far
    QString m_error;
};

} // namespace

bool evaluate(const QString &expr, const Lookup &lookup, double *value, QString *error)
{
    double v = 0;
    Parser p(expr, lookup);
    if (!p.parse(&v, error))
        return false;
    if (value) *value = v;
    return true;
}

QStringList namesIn(const QString &expr)
{
    QStringList out;
    static const QRegularExpression re(QStringLiteral("[A-Za-z_][A-Za-z0-9_]*"));
    auto it = re.globalMatch(expr);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString id = m.captured(0);
        // A name glued to a number ("1e3", "2in") belongs to the number.
        const int at = m.capturedStart(0);
        if (at > 0 && (expr.at(at - 1).isDigit() || expr.at(at - 1) == QLatin1Char('.')))
            continue;
        if (isFunction(id) || isUnit(id) || id == QLatin1String("pi") || out.contains(id))
            continue;
        out << id;
    }
    return out;
}

QStringList builtinNames()
{
    return {QStringLiteral("stock_w"), QStringLiteral("stock_h"), QStringLiteral("stock_t")};
}

bool isValidName(const QString &name)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
    return re.match(name).hasMatch() && !isFunction(name) && !isUnit(name)
           && name != QLatin1String("pi") && name != QLatin1String("origin")
           && !builtinNames().contains(name);
}

QString formatNumber(double v)
{
    if (std::fabs(v) < 5e-7)
        v = 0;
    QString s = QString::number(v, 'f', 3);
    if (s.contains(QLatin1Char('.'))) {
        while (s.endsWith(QLatin1Char('0')))
            s.chop(1);
        if (s.endsWith(QLatin1Char('.')))
            s.chop(1);
    }
    return s == QLatin1String("-0") ? QStringLiteral("0") : s;
}

} // namespace param

// ---- the data ------------------------------------------------------------------

QString Constraint::typeName(Type t)
{
    switch (t) {
    case Coincident:    return QStringLiteral("Coincident");
    case Horizontal:    return QStringLiteral("Horizontal");
    case Vertical:      return QStringLiteral("Vertical");
    case Parallel:      return QStringLiteral("Parallel");
    case Perpendicular: return QStringLiteral("Perpendicular");
    case Tangent:       return QStringLiteral("Tangent");
    case Equal:         return QStringLiteral("Equal");
    case Concentric:    return QStringLiteral("Concentric");
    case Midpoint:      return QStringLiteral("Midpoint");
    case Collinear:     return QStringLiteral("Collinear");
    case Fix:           return QStringLiteral("Fix");
    case Symmetric:     return QStringLiteral("Symmetric");
    case Distance:      return QStringLiteral("Distance");
    case HDistance:     return QStringLiteral("Horizontal distance");
    case VDistance:     return QStringLiteral("Vertical distance");
    case Radius:        return QStringLiteral("Radius");
    case Diameter:      return QStringLiteral("Diameter");
    case Angle:         return QStringLiteral("Angle");
    }
    return QString();
}

QString Constraint::symbol(Type t)
{
    switch (t) {
    case Coincident:    return QStringLiteral("●");   // ●
    case Horizontal:    return QStringLiteral("H");
    case Vertical:      return QStringLiteral("V");
    case Parallel:      return QStringLiteral("∥");   // ∥
    case Perpendicular: return QStringLiteral("⟂");   // ⟂
    case Tangent:       return QStringLiteral("T");
    case Equal:         return QStringLiteral("=");
    case Concentric:    return QStringLiteral("◎");   // ◎
    case Midpoint:      return QStringLiteral("△");   // △
    case Collinear:     return QStringLiteral("≡");   // ≡
    case Fix:           return QStringLiteral("⌂");   // ⌂ (drawn as a lock)
    case Symmetric:     return QStringLiteral("[ ]");
    case Distance:
    case HDistance:
    case VDistance:     return QStringLiteral("↔");   // ↔
    case Radius:        return QStringLiteral("R");
    case Diameter:      return QStringLiteral("⌀");   // ⌀
    case Angle:         return QStringLiteral("∠");   // ∠
    }
    return QString();
}

static QJsonArray xyArr(const QPointF &p) { return QJsonArray{p.x(), p.y()}; }
static QPointF arrXy(const QJsonValue &v)
{
    const QJsonArray a = v.toArray();
    return a.size() == 2 ? QPointF(a.at(0).toDouble(), a.at(1).toDouble()) : QPointF();
}

QJsonObject Parametric::toJson() const
{
    QJsonArray ps;
    for (const UserParameter &p : params) {
        QJsonObject o;
        o.insert("name", p.name);
        o.insert("expr", p.expr);
        if (!p.comment.isEmpty())
            o.insert("comment", p.comment);
        ps.append(o);
    }
    QJsonArray cs;
    for (const Constraint &c : constraints) {
        QJsonObject o;
        o.insert("type", int(c.type));
        o.insert("uid", c.uid);
        QJsonArray refs;
        for (const GeoRef &r : c.refs) {
            QJsonArray ra{int(r.kind), r.id, r.sub, r.index};
            if (r.reversed)
                ra.append(1);
            refs.append(ra);
        }
        o.insert("refs", refs);
        if (c.sense != 1)
            o.insert("sense", c.sense);
        if (c.type == Constraint::Fix)
            o.insert("at", xyArr(c.at));
        if (c.isDimension()) {
            o.insert("name", c.name);
            o.insert("expr", c.expr);
            o.insert("value", c.value);
            o.insert("label", xyArr(c.label));
        }
        cs.append(o);
    }
    QJsonObject o;
    o.insert("version", 1);
    o.insert("params", ps);
    o.insert("constraints", cs);
    return o;
}

Parametric Parametric::fromJson(const QJsonObject &o)
{
    Parametric p;
    for (const QJsonValue &v : o.value("params").toArray()) {
        const QJsonObject po = v.toObject();
        UserParameter up;
        up.name = po.value("name").toString();
        up.expr = po.value("expr").toString();
        up.comment = po.value("comment").toString();
        // A name twice (a hand-edited or old file) would hide one of them.
        if (!up.name.isEmpty() && !p.nameInUse(up.name))
            p.params.append(up);
    }
    for (const QJsonValue &v : o.value("constraints").toArray()) {
        const QJsonObject co = v.toObject();
        Constraint c;
        const int t = co.value("type").toInt(-1);
        if (t < Constraint::Coincident || t > Constraint::Angle)
            continue;
        c.type = Constraint::Type(t);
        c.uid = co.value("uid").toString();
        for (const QJsonValue &rv : co.value("refs").toArray()) {
            const QJsonArray ra = rv.toArray();
            if (ra.size() != 4 && ra.size() != 5)
                continue;
            GeoRef r;
            r.kind = GeoRef::Kind(qBound(0, ra.at(0).toInt(), 2));
            r.id = ra.at(1).toString();
            r.sub = ra.at(2).toInt();
            r.index = ra.at(3).toInt();
            r.reversed = r.kind == GeoRef::Line && ra.size() == 5 && ra.at(4).toInt() != 0;
            c.refs.append(r);
        }
        c.sense = co.value("sense").toInt(1) < 0 ? -1 : 1;
        c.at = arrXy(co.value("at"));
        c.name = co.value("name").toString();
        c.expr = co.value("expr").toString();
        c.value = co.value("value").toDouble();
        c.label = arrXy(co.value("label"));
        p.constraints.append(c);
    }
    // Uids made unique once all are read: one without a uid read before a
    // "c1" further on would otherwise get "c1" too, and the panel and Del
    // could only ever reach the first.
    QSet<QString> uids;
    for (Constraint &c : p.constraints) {
        if (c.uid.isEmpty() || uids.contains(c.uid))
            c.uid.clear();
        else
            uids.insert(c.uid);
    }
    for (Constraint &c : p.constraints)
        if (c.uid.isEmpty()) {
            c.uid = p.nextUid();
            uids.insert(c.uid);
        }
    // Likewise a dimension sharing a parameter's or another dimension's name.
    QSet<QString> names;
    for (const UserParameter &up : p.params)
        names.insert(up.name);
    for (Constraint &c : p.constraints) {
        if (!c.isDimension() || c.name.isEmpty())
            continue;
        if (names.contains(c.name))
            c.name = p.nextDimensionName();
        names.insert(c.name);
    }
    return p;
}

QString Parametric::nextDimensionName() const
{
    for (int i = 1;; ++i) {
        const QString n = QStringLiteral("d%1").arg(i);
        if (!nameInUse(n))
            return n;
    }
}

QString Parametric::nextUid() const
{
    int top = 0;
    for (const Constraint &c : constraints)
        if (c.uid.startsWith(QLatin1Char('c')))
            top = qMax(top, c.uid.mid(1).toInt());
    return QStringLiteral("c%1").arg(top + 1);
}

int Parametric::indexOfUid(const QString &uid) const
{
    for (int i = 0; i < constraints.size(); ++i)
        if (constraints.at(i).uid == uid)
            return i;
    return -1;
}

int Parametric::indexOfParam(const QString &name) const
{
    for (int i = 0; i < params.size(); ++i)
        if (params.at(i).name == name)
            return i;
    return -1;
}

bool Parametric::nameInUse(const QString &name) const
{
    if (indexOfParam(name) >= 0)
        return true;
    for (const Constraint &c : constraints)
        if (c.isDimension() && c.name == name)
            return true;
    return false;
}

QString param::renameIn(const QString &expr, const QString &from, const QString &to)
{
    QRegularExpression re(QStringLiteral("(?<![A-Za-z0-9_.])%1(?![A-Za-z0-9_])")
                              .arg(QRegularExpression::escape(from)));
    QString out = expr;
    out.replace(re, to);
    return out;
}

void Parametric::renameEverywhere(const QString &from, const QString &to)
{
    for (UserParameter &p : params) {
        if (p.name == from)
            p.name = to;
        p.expr = param::renameIn(p.expr, from, to);
    }
    for (Constraint &c : constraints) {
        if (c.isDimension() && c.name == from)
            c.name = to;
        c.expr = param::renameIn(c.expr, from, to);
    }
}

// ---- values ----------------------------------------------------------------------
namespace param {

QHash<QString, double> values(const Document &doc, const Parametric &p, QStringList *errors)
{
    QHash<QString, double> out;
    out.insert(QStringLiteral("stock_w"), doc.boardWidth());
    out.insert(QStringLiteral("stock_h"), doc.boardHeight());
    out.insert(QStringLiteral("stock_t"), doc.params().value(QStringLiteral("thickness"), "0").toDouble());

    QHash<QString, QString> exprs;   // every name to evaluate
    QStringList order;
    for (const UserParameter &up : p.params)
        if (!exprs.contains(up.name)) { exprs.insert(up.name, up.expr); order << up.name; }
    for (const Constraint &c : p.constraints)
        if (c.isDimension() && !c.name.isEmpty() && !exprs.contains(c.name)) {
            exprs.insert(c.name, c.expr.trimmed().isEmpty() ? formatNumber(c.value) : c.expr);
            order << c.name;
        }

    QStringList stack;               // names being evaluated, outermost first
    QSet<QString> failed, looped;
    std::function<bool(const QString &, double *)> resolve;
    resolve = [&](const QString &name, double *v) -> bool {
        if (out.contains(name)) { *v = out.value(name); return true; }
        if (failed.contains(name) || !exprs.contains(name))
            return false;
        if (stack.contains(name)) {
            // A loop: every name from here round to here again is in it.
            const QStringList loop = stack.mid(stack.indexOf(name)) << name;
            for (int k = 0; k + 1 < loop.size(); ++k)
                if (!looped.contains(loop.at(k))) {
                    looped.insert(loop.at(k));
                    if (errors)
                        *errors << QStringLiteral("%1: goes round in a loop (%2)")
                                       .arg(loop.at(k), loop.join(QStringLiteral(" \u2192 ")));
                }
            return false;
        }
        if (stack.size() > 200) {
            // A crafted chain thousands of names long would overflow the stack.
            if (errors && !failed.contains(name))
                *errors << QStringLiteral("%1: more than 200 names deep").arg(name);
            failed.insert(name);
            return false;
        }
        stack << name;
        QString why;
        double r = 0;
        const bool ok = evaluate(exprs.value(name), resolve, &r, &why);
        stack.removeLast();
        if (!ok) {
            if (!failed.contains(name) && !looped.contains(name) && errors)
                *errors << QStringLiteral("%1: %2").arg(name, why);
            failed.insert(name);
            return false;
        }
        out.insert(name, r);
        *v = r;
        return true;
    };
    for (const QString &n : order) {
        double v;
        resolve(n, &v);
    }
    return out;
}

QString badValue(const Constraint &c, double v)
{
    if ((c.type == Constraint::Radius || c.type == Constraint::Diameter
         || (c.type == Constraint::Distance && c.refs.size() == 1)) && !(v > 0))
        return QStringLiteral("%1 must be more than 0").arg(Constraint::typeName(c.type));
    // An angle is measured between 0 and 180 degrees, its direction kept by
    // `sense`: 270 would solve to 90 while the label still read 270.
    if (c.type == Constraint::Angle && !(v > 0 && v < 180))
        return QStringLiteral("Angle must be more than 0 and less than 180°");
    return QString();
}

// Dimensions whose value can't be drawn ("name: why"), by name.
static QHash<QString, QString> badDimensions(const Parametric &p, const QHash<QString, double> &vals)
{
    QHash<QString, QString> bad;
    for (const Constraint &c : p.constraints) {
        if (!c.isDimension())
            continue;
        auto it = vals.constFind(c.name);
        if (it == vals.constEnd())
            continue;
        if (const QString why = badValue(c, it.value()); !why.isEmpty())
            bad.insert(c.name, QStringLiteral("%1: %2").arg(c.name, why));
    }
    return bad;
}

QString newError(const Document &doc, const Parametric &before, const Parametric &after)
{
    QStringList was, now;
    const QHash<QString, double> valsBefore = values(doc, before, &was);
    const QHash<QString, double> valsAfter = values(doc, after, &now);
    QSet<QString> broken;
    for (const QString &e : was)
        broken.insert(e.section(QLatin1Char(':'), 0, 0));
    for (const QString &e : now)
        if (!broken.contains(e.section(QLatin1Char(':'), 0, 0)))
            return e;
    // A parameter driving a radius or a length to 0 or below (r = -5) would
    // solve to a circle the dimension then reads as broken for good.
    const QHash<QString, QString> badBefore = badDimensions(before, valsBefore);
    const QHash<QString, QString> badAfter = badDimensions(after, valsAfter);
    for (auto it = badAfter.constBegin(); it != badAfter.constEnd(); ++it)
        if (!badBefore.contains(it.key()))
            return it.value();
    return QString();
}

bool evaluateIn(const Document &doc, const QString &expr, double *value, QString *error)
{
    const QHash<QString, double> vals = values(doc, doc.parametric());
    return evaluate(expr, [&](const QString &n, double *v) {
        auto it = vals.constFind(n);
        if (it == vals.constEnd()) return false;
        *v = it.value();
        return true;
    }, value, error);
}

// ---- geometry model -------------------------------------------------------------
// One element as solver variables. A path only gets variables for the anchors
// some constraint refers to: the rest of its nodes stay where they are.
namespace {

struct Body {
    enum T { Circle, SketchPoint, Rect, Polygon, Path } t = Circle;
    QString id;
    const Element *src = nullptr;
    int off = 0;                    // first variable in the global vector
    int n = 0;                      // variables
    bool locked = false;            // Fix on the whole element
    int sides = 0;                  // polygon
    double rot = 0;                 // polygon and rectangle, radians
    PathModel model;                // path
    QHash<quint64, int> nodeVar;    // (sub,node) -> variable offset within the body
    int pathNodes = 0;              // all nodes of the path
    bool rounded = false;           // rectangle with CC corners: its size stays
};

quint64 nodeKey(int sub, int node) { return (quint64(quint32(sub)) << 32) | quint32(node); }

bool bodyFor(const Element &e, Body *b)
{
    b->id = e.id;
    b->src = &e;
    const QString &g = e.geometryType;
    if (g == QLatin1String("circle")) {
        b->t = Element::isPoint(e) ? Body::SketchPoint : Body::Circle;
        b->n = b->t == Body::SketchPoint ? 2 : 3;
    } else if (g == QLatin1String("rectangle")) {
        b->t = Body::Rect;
        b->n = 4;
        // Rebuilding a rectangle squares CC's rounded / dog-bone corners, so
        // the solver may only move one of those, never resize it.
        b->rounded = e.raw.value("corner_type").toInt() != 0;
        b->rot = qDegreesToRadians(e.raw.value("rotation").toDouble());
    } else if (g == QLatin1String("regular_polygon")) {
        b->t = Body::Polygon;
        b->n = 3;
        b->sides = qMax(3, e.raw.value("num_sides").toInt());
        b->rot = qDegreesToRadians(e.raw.value("rotation").toDouble());
    } else if (g == QLatin1String("path")) {
        b->t = Body::Path;
        b->model = Element::pathModel(e);
        for (const SubPath &s : b->model.subs)
            b->pathNodes += s.nodes.size();
        b->n = 0;   // grows as references claim anchors
    } else {
        return false;
    }
    return true;
}

// The body's variables as they are in the element.
void initial(const Body &b, double *x)
{
    const QJsonObject &r = b.src->raw;
    const QPointF c = arrXy(r.value("center"));
    switch (b.t) {
    case Body::Circle:
    case Body::Polygon:
        x[0] = c.x(); x[1] = c.y(); x[2] = r.value("radius").toDouble();
        break;
    case Body::SketchPoint:
        x[0] = c.x(); x[1] = c.y();
        break;
    case Body::Rect:
        x[0] = c.x(); x[1] = c.y();
        x[2] = r.value("width").toDouble(); x[3] = r.value("height").toDouble();
        break;
    case Body::Path:
        for (auto it = b.nodeVar.constBegin(); it != b.nodeVar.constEnd(); ++it) {
            const int sub = int(it.key() >> 32), node = int(it.key() & 0xffffffffu);
            const QPointF p = b.model.subs.at(sub).nodes.at(node).p;
            x[it.value()] = p.x();
            x[it.value() + 1] = p.y();
        }
        break;
    }
}

bool pathNode(const Body &b, int sub, int node)
{
    return sub >= 0 && sub < b.model.subs.size() && node >= 0
           && node < b.model.subs.at(sub).nodes.size();
}

// Does the reference name something this body has?
bool fits(const Body &b, const GeoRef &r)
{
    switch (r.kind) {
    case GeoRef::Point:
        switch (b.t) {
        case Body::Circle:
        case Body::SketchPoint: return r.index == -1;
        case Body::Rect:        return r.index >= -1 && r.index < 4;
        case Body::Polygon:     return r.index >= -1 && r.index < b.sides;
        case Body::Path:        return pathNode(b, r.sub, r.index);
        }
        return false;
    case GeoRef::Line:
        switch (b.t) {
        case Body::Rect:    return r.index >= 0 && r.index < 4;
        case Body::Polygon: return r.index >= 0 && r.index < b.sides;
        case Body::Path: {
            if (r.sub < 0 || r.sub >= b.model.subs.size() || r.index < 0
                || r.index >= b.model.subs.at(r.sub).segmentCount())
                return false;
            // Only a straight segment is a line; a node edit can curve it.
            const PathNode *na, *nb;
            b.model.subs.at(r.sub).segment(r.index, &na, &nb);
            return !na->hasOut() && !nb->hasIn();
        }
        default:            return false;
        }
    case GeoRef::Circle:
        return b.t == Body::Circle || b.t == Body::SketchPoint;
    }
    return false;
}

// Claim the path anchors a reference uses as variables.
void claim(Body &b, const GeoRef &r)
{
    if (b.t != Body::Path)
        return;
    auto take = [&](int sub, int node) {
        const quint64 k = nodeKey(sub, node);
        if (!b.nodeVar.contains(k)) {
            b.nodeVar.insert(k, b.n);
            b.n += 2;
        }
    };
    if (r.kind == GeoRef::Point) {
        take(r.sub, r.index);
    } else if (r.kind == GeoRef::Line) {
        const SubPath &s = b.model.subs.at(r.sub);
        take(r.sub, r.index);
        take(r.sub, (r.index + 1) % s.nodes.size());
    }
}

QPointF pathAnchor(const Body &b, const double *x, int sub, int node)
{
    const auto it = b.nodeVar.constFind(nodeKey(sub, node));
    if (it == b.nodeVar.constEnd())
        return b.model.subs.at(sub).nodes.at(node).p;
    return QPointF(x[b.off + it.value()], x[b.off + it.value() + 1]);
}

QPointF polyVertex(const Body &b, const double *v, int k)
{
    const double a = 2.0 * M_PI * k / b.sides + b.rot;
    return QPointF(v[0] + v[2] * std::cos(a), v[1] + v[2] * std::sin(a));
}

// A corner of a rectangle turned by its `rotation` about its centre, as
// Carbide Create draws it (corner 0 is the unturned lower left).
QPointF rectCorner(const Body &b, const double *v, int k)
{
    const double w = v[2] / 2, h = v[3] / 2;
    QPointF d;
    switch (((k % 4) + 4) % 4) {
    case 0:  d = QPointF(-w, -h); break;
    case 1:  d = QPointF(w, -h); break;
    case 2:  d = QPointF(w, h); break;
    default: d = QPointF(-w, h); break;
    }
    const double c = std::cos(b.rot), s = std::sin(b.rot);
    return QPointF(v[0] + d.x() * c - d.y() * s, v[1] + d.x() * s + d.y() * c);
}

bool bodyPoint(const Body &b, const double *x, const GeoRef &r, QPointF *p)
{
    const double *v = x + b.off;
    switch (b.t) {
    case Body::Circle:
    case Body::SketchPoint:
        *p = QPointF(v[0], v[1]);
        return true;
    case Body::Rect:
        *p = r.index < 0 ? QPointF(v[0], v[1]) : rectCorner(b, v, r.index);
        return true;
    case Body::Polygon:
        *p = r.index < 0 ? QPointF(v[0], v[1]) : polyVertex(b, v, r.index);
        return true;
    case Body::Path:
        *p = pathAnchor(b, x, r.sub, r.index);
        return true;
    }
    return false;
}

bool bodyLine(const Body &b, const double *x, const GeoRef &r, QPointF *a, QPointF *e)
{
    if (r.reversed)
        std::swap(a, e);    // written end first: the line runs back
    const double *v = x + b.off;
    switch (b.t) {
    case Body::Rect:
        *a = rectCorner(b, v, r.index);
        *e = rectCorner(b, v, r.index + 1);
        return true;
    case Body::Polygon:
        *a = polyVertex(b, v, r.index);
        *e = polyVertex(b, v, (r.index + 1) % b.sides);
        return true;
    case Body::Path: {
        const SubPath &s = b.model.subs.at(r.sub);
        *a = pathAnchor(b, x, r.sub, r.index);
        *e = pathAnchor(b, x, r.sub, (r.index + 1) % s.nodes.size());
        return true;
    }
    default:
        return false;
    }
}

bool bodyCircle(const Body &b, const double *x, QPointF *c, double *rad)
{
    const double *v = x + b.off;
    if (b.t == Body::Circle) {
        *c = QPointF(v[0], v[1]);
        *rad = v[2];
        return true;
    }
    if (b.t == Body::SketchPoint) {
        *c = QPointF(v[0], v[1]);
        *rad = 0;
        return true;
    }
    return false;
}

// The element with the body's variables written back.
Element written(const Body &b, const double *x)
{
    const double *v = x + b.off;
    QHash<QString, double> p;
    switch (b.t) {
    case Body::Circle:
        p.insert("cx", v[0]); p.insert("cy", v[1]); p.insert("radius", std::fabs(v[2]));
        return Element::regen(*b.src, p);
    case Body::SketchPoint:
        p.insert("cx", v[0]); p.insert("cy", v[1]);
        return Element::regen(*b.src, p);
    case Body::Polygon:
        p.insert("cx", v[0]); p.insert("cy", v[1]); p.insert("radius", std::fabs(v[2]));
        return Element::regen(*b.src, p);
    case Body::Rect:
        if (b.rounded) {
            Element e = *b.src;
            const QPointF c = arrXy(e.raw.value("center"));
            e.translate(v[0] - c.x(), v[1] - c.y());
            return e;
        }
        p.insert("cx", v[0]); p.insert("cy", v[1]);
        p.insert("width", std::fabs(v[2])); p.insert("height", std::fabs(v[3]));
        return Element::regen(*b.src, p);
    case Body::Path: {
        PathModel m = b.model;
        for (auto it = b.nodeVar.constBegin(); it != b.nodeVar.constEnd(); ++it) {
            const int sub = int(it.key() >> 32), node = int(it.key() & 0xffffffffu);
            PathNode &nd = m.subs[sub].nodes[node];
            const QPointF to(v[it.value()], v[it.value() + 1]);
            const QPointF d = to - nd.p;
            nd.p = to; nd.in += d; nd.out += d;
        }
        return Element::withPathModel(*b.src, m);
    }
    }
    return *b.src;
}

// A set of bodies and the constraints over them.
class System
{
public:
    System(const Document &doc, const QVector<Constraint> &cons) : m_doc(doc)
    {
        for (const Constraint &c : cons) {
            bool ok = !c.refs.isEmpty();
            for (const GeoRef &r : c.refs)
                if (!addRef(r)) { ok = false; break; }
            if (ok && shapeOk(c))
                m_cons.append(c);
        }
        // Lay the variables out, then lock what is fixed whole.
        int off = 0;
        for (Body &b : m_bodies) {
            b.off = off;
            off += b.n;
        }
        m_x.resize(off);
        for (const Body &b : m_bodies)
            initial(b, m_x.data() + b.off);
        for (const Constraint &c : m_cons)
            if (c.type == Constraint::Fix && c.refs.first().kind != GeoRef::Point)
                if (Body *b = body(c.refs.first().id))
                    b->locked = true;
    }

    const QVector<Constraint> &constraints() const { return m_cons; }
    QVector<Body> &bodies() { return m_bodies; }
    const QVector<Body> &bodies() const { return m_bodies; }
    QVector<double> &x() { return m_x; }
    const QVector<double> &x() const { return m_x; }

    Body *body(const QString &id)
    {
        const auto it = m_index.constFind(id);
        return it == m_index.constEnd() ? nullptr : &m_bodies[it.value()];
    }
    const Body *body(const QString &id) const
    {
        const auto it = m_index.constFind(id);
        return it == m_index.constEnd() ? nullptr : &m_bodies.at(it.value());
    }

    bool point(const double *x, const GeoRef &r, QPointF *p) const
    {
        if (r.isOrigin()) { *p = QPointF(0, 0); return true; }
        const Body *b = body(r.id);
        if (!b) return false;
        if (r.kind == GeoRef::Circle) {     // a circle used as a point: its center
            double rad;
            return bodyCircle(*b, x, p, &rad);
        }
        return bodyPoint(*b, x, r, p);
    }
    bool line(const double *x, const GeoRef &r, QPointF *a, QPointF *e) const
    {
        const Body *b = body(r.id);
        return b && bodyLine(*b, x, r, a, e);
    }
    bool circ(const double *x, const GeoRef &r, QPointF *c, double *rad) const
    {
        const Body *b = body(r.id);
        return b && bodyCircle(*b, x, c, rad);
    }

    // Residuals of one constraint (mm); returns how many it wrote (<= 3).
    int residuals(const Constraint &c, const double *x, double *out) const;
    // The dimension's measured value.
    double measured(const Constraint &c, const double *x) const;

private:
    bool addRef(const GeoRef &r)
    {
        if (r.isOrigin())
            return r.kind == GeoRef::Point;
        Body *b = body(r.id);
        if (!b) {
            const Element *e = nullptr;
            for (const Element &el : m_doc.elements())
                if (el.id == r.id) { e = &el; break; }
            if (!e)
                return false;
            Body nb;
            if (!bodyFor(*e, &nb))
                return false;
            m_index.insert(r.id, m_bodies.size());
            m_bodies.append(nb);
            b = &m_bodies.last();
        }
        if (!fits(*b, r))
            return false;
        claim(*b, r);
        return true;
    }
    // The refs are the kinds the type works on.
    bool shapeOk(const Constraint &c) const;

    const Document &m_doc;
    QVector<Body> m_bodies;
    QHash<QString, int> m_index;
    QVector<Constraint> m_cons;
    QVector<double> m_x;
};

using K = GeoRef::Kind;

bool kinds(const Constraint &c, std::initializer_list<K> want)
{
    if (c.refs.size() != int(want.size()))
        return false;
    int i = 0;
    for (K k : want)
        if (c.refs.at(i++).kind != k)
            return false;
    return true;
}

bool System::shapeOk(const Constraint &c) const
{
    using T = Constraint;
    switch (c.type) {
    case T::Coincident:
        return kinds(c, {K::Point, K::Point}) || kinds(c, {K::Point, K::Line})
               || kinds(c, {K::Point, K::Circle});
    case T::Horizontal:
    case T::Vertical:
        return kinds(c, {K::Line}) || kinds(c, {K::Point, K::Point});
    case T::Parallel:
    case T::Perpendicular:
    case T::Collinear:
        return kinds(c, {K::Line, K::Line});
    case T::Tangent:
        return kinds(c, {K::Line, K::Circle}) || kinds(c, {K::Circle, K::Circle});
    case T::Equal:
        return kinds(c, {K::Line, K::Line}) || kinds(c, {K::Circle, K::Circle});
    case T::Concentric:
        return kinds(c, {K::Circle, K::Circle});
    case T::Midpoint:
        return kinds(c, {K::Point, K::Line});
    case T::Fix:
        return c.refs.size() == 1;
    case T::Symmetric:
        return kinds(c, {K::Point, K::Point, K::Line});
    case T::Distance:
        return kinds(c, {K::Point, K::Point}) || kinds(c, {K::Point, K::Line})
               || kinds(c, {K::Line, K::Line}) || kinds(c, {K::Line});
    case T::HDistance:
    case T::VDistance:
        return kinds(c, {K::Point, K::Point}) || kinds(c, {K::Line});
    case T::Radius:
    case T::Diameter:
        return kinds(c, {K::Circle});
    case T::Angle:
        return kinds(c, {K::Line, K::Line});
    }
    return false;
}

double cross(QPointF a, QPointF b) { return a.x() * b.y() - a.y() * b.x(); }
double dot(QPointF a, QPointF b) { return a.x() * b.x() + a.y() * b.y(); }
double len(QPointF a) { return std::hypot(a.x(), a.y()); }
// Signed distance of p from the line a->b (+ on the left).
double side(QPointF p, QPointF a, QPointF b)
{
    const double l = len(b - a);
    return l < 1e-12 ? len(p - a) : cross(b - a, p - a) / l;
}
double wrapPi(double a)
{
    // remainder, not a loop: an angle of 1e300 (a crafted file) would spin
    // the loop for ever.
    if (!std::isfinite(a))
        return a;
    if (a > M_PI || a < -M_PI)
        a = std::remainder(a, 2 * M_PI);
    return a;
}

// Two lines' (or a line's) end points.
struct Seg { QPointF a, b; };

double System::measured(const Constraint &c, const double *x) const
{
    using T = Constraint;
    QPointF p, q, a, b, a2, b2;
    double r = 0;
    switch (c.type) {
    case T::Distance:
        if (kinds(c, {K::Line})) {
            line(x, c.refs[0], &a, &b);
            return len(b - a);
        }
        if (kinds(c, {K::Point, K::Point})) {
            point(x, c.refs[0], &p);
            point(x, c.refs[1], &q);
            return len(q - p);
        }
        if (kinds(c, {K::Point, K::Line})) {
            point(x, c.refs[0], &p);
            line(x, c.refs[1], &a, &b);
            return std::fabs(side(p, a, b));
        }
        line(x, c.refs[0], &a, &b);
        line(x, c.refs[1], &a2, &b2);
        return std::fabs(side((a2 + b2) / 2, a, b));
    case T::HDistance:
    case T::VDistance:
        if (kinds(c, {K::Line})) {
            line(x, c.refs[0], &p, &q);
        } else {
            point(x, c.refs[0], &p);
            point(x, c.refs[1], &q);
        }
        return c.type == T::HDistance ? std::fabs(q.x() - p.x()) : std::fabs(q.y() - p.y());
    case T::Radius:
        circ(x, c.refs[0], &p, &r);
        return std::fabs(r);
    case T::Diameter:
        circ(x, c.refs[0], &p, &r);
        return 2 * std::fabs(r);
    case T::Angle: {
        line(x, c.refs[0], &a, &b);
        line(x, c.refs[1], &a2, &b2);
        const double t = std::atan2(cross(b - a, b2 - a2), dot(b - a, b2 - a2));
        return std::fabs(qRadiansToDegrees(t));
    }
    default:
        return 0;
    }
}

int System::residuals(const Constraint &c, const double *x, double *o) const
{
    using T = Constraint;
    QPointF p, q, a, b, a2, b2, c1, c2;
    double r1 = 0, r2 = 0;
    switch (c.type) {
    case T::Coincident:
        point(x, c.refs[0], &p);
        if (c.refs[1].kind == K::Point) {
            point(x, c.refs[1], &q);
            o[0] = q.x() - p.x();
            o[1] = q.y() - p.y();
            return 2;
        }
        if (c.refs[1].kind == K::Line) {
            line(x, c.refs[1], &a, &b);
            o[0] = side(p, a, b);
            return 1;
        }
        circ(x, c.refs[1], &c1, &r1);
        o[0] = len(p - c1) - std::fabs(r1);
        return 1;
    case T::Horizontal:
    case T::Vertical:
        if (c.refs.size() == 1)
            line(x, c.refs[0], &p, &q);
        else {
            point(x, c.refs[0], &p);
            point(x, c.refs[1], &q);
        }
        o[0] = c.type == T::Horizontal ? q.y() - p.y() : q.x() - p.x();
        return 1;
    case T::Parallel:
    case T::Perpendicular: {
        line(x, c.refs[0], &a, &b);
        line(x, c.refs[1], &a2, &b2);
        const QPointF u = b - a, v = b2 - a2;
        const double lu = len(u), lv = len(v);
        if (lu < 1e-12 || lv < 1e-12) { o[0] = 0; return 1; }
        const double scale = (lu + lv) / 2;
        o[0] = (c.type == T::Parallel ? cross(u, v) : dot(u, v)) / (lu * lv) * scale;
        return 1;
    }
    case T::Collinear:
        line(x, c.refs[0], &a, &b);
        line(x, c.refs[1], &a2, &b2);
        o[0] = side(a2, a, b);
        o[1] = side(b2, a, b);
        return 2;
    case T::Tangent:
        if (c.refs[0].kind == K::Line) {
            line(x, c.refs[0], &a, &b);
            circ(x, c.refs[1], &c1, &r1);
            o[0] = c.sense * side(c1, a, b) - std::fabs(r1);
            return 1;
        }
        circ(x, c.refs[0], &c1, &r1);
        circ(x, c.refs[1], &c2, &r2);
        o[0] = c.sense > 0 ? len(c2 - c1) - (std::fabs(r1) + std::fabs(r2))
                           : len(c2 - c1) - std::fabs(std::fabs(r1) - std::fabs(r2));
        return 1;
    case T::Equal:
        if (c.refs[0].kind == K::Line) {
            line(x, c.refs[0], &a, &b);
            line(x, c.refs[1], &a2, &b2);
            o[0] = len(b2 - a2) - len(b - a);
            return 1;
        }
        circ(x, c.refs[0], &c1, &r1);
        circ(x, c.refs[1], &c2, &r2);
        o[0] = std::fabs(r2) - std::fabs(r1);
        return 1;
    case T::Concentric:
        circ(x, c.refs[0], &c1, &r1);
        circ(x, c.refs[1], &c2, &r2);
        o[0] = c2.x() - c1.x();
        o[1] = c2.y() - c1.y();
        return 2;
    case T::Midpoint:
        point(x, c.refs[0], &p);
        line(x, c.refs[1], &a, &b);
        o[0] = (a.x() + b.x()) / 2 - p.x();
        o[1] = (a.y() + b.y()) / 2 - p.y();
        return 2;
    case T::Fix:
        if (c.refs[0].kind != K::Point)
            return 0;               // the whole element: locked, no residual
        point(x, c.refs[0], &p);
        o[0] = p.x() - c.at.x();
        o[1] = p.y() - c.at.y();
        return 2;
    case T::Symmetric: {
        point(x, c.refs[0], &p);
        point(x, c.refs[1], &q);
        line(x, c.refs[2], &a, &b);
        const QPointF u = b - a;
        const double lu = len(u);
        o[0] = side((p + q) / 2, a, b);
        o[1] = lu < 1e-12 ? 0 : dot(q - p, u) / lu;
        return 2;
    }
    case T::Distance:
        if (kinds(c, {K::Line})) {
            line(x, c.refs[0], &a, &b);
            o[0] = len(b - a) - c.value;
            return 1;
        }
        if (kinds(c, {K::Point, K::Point})) {
            point(x, c.refs[0], &p);
            point(x, c.refs[1], &q);
            o[0] = len(q - p) - c.value;
            return 1;
        }
        if (kinds(c, {K::Point, K::Line})) {
            point(x, c.refs[0], &p);
            line(x, c.refs[1], &a, &b);
            o[0] = c.sense * side(p, a, b) - c.value;
            return 1;
        }
        line(x, c.refs[0], &a, &b);
        line(x, c.refs[1], &a2, &b2);
        o[0] = c.sense * side((a2 + b2) / 2, a, b) - c.value;
        return 1;
    case T::HDistance:
    case T::VDistance:
        if (kinds(c, {K::Line}))
            line(x, c.refs[0], &p, &q);
        else {
            point(x, c.refs[0], &p);
            point(x, c.refs[1], &q);
        }
        o[0] = (c.type == T::HDistance ? q.x() - p.x() : q.y() - p.y()) - c.sense * c.value;
        return 1;
    case T::Radius:
        circ(x, c.refs[0], &c1, &r1);
        o[0] = r1 - c.value;
        return 1;
    case T::Diameter:
        circ(x, c.refs[0], &c1, &r1);
        o[0] = 2 * r1 - c.value;
        return 1;
    case T::Angle: {
        line(x, c.refs[0], &a, &b);
        line(x, c.refs[1], &a2, &b2);
        const QPointF u = b - a, v = b2 - a2;
        const double t = std::atan2(cross(u, v), dot(u, v));
        const double scale = qMax(1.0, (len(u) + len(v)) / 2);
        o[0] = wrapPi(t - c.sense * qDegreesToRadians(c.value)) * scale;
        return 1;
    }
    }
    return 0;
}

// Solve A x = b in place (n x n, row-major), partial pivoting. False when singular.
bool gauss(QVector<double> &A, QVector<double> &b, int n)
{
    for (int col = 0; col < n; ++col) {
        int piv = col;
        double best = std::fabs(A[col * n + col]);
        for (int r = col + 1; r < n; ++r)
            if (std::fabs(A[r * n + col]) > best) { best = std::fabs(A[r * n + col]); piv = r; }
        if (best < 1e-300)
            return false;
        if (piv != col) {
            for (int k = 0; k < n; ++k)
                std::swap(A[col * n + k], A[piv * n + k]);
            std::swap(b[col], b[piv]);
        }
        const double d = A[col * n + col];
        for (int r = col + 1; r < n; ++r) {
            const double f = A[r * n + col] / d;
            if (f == 0) continue;
            for (int k = col; k < n; ++k)
                A[r * n + k] -= f * A[col * n + k];
            b[r] -= f * b[col];
        }
    }
    for (int r = n - 1; r >= 0; --r) {
        double s = b[r];
        for (int k = r + 1; k < n; ++k)
            s -= A[r * n + k] * b[k];
        b[r] = s / A[r * n + r];
    }
    return true;
}

// The constraints' residual rows and the free variables they depend on.
struct Problem {
    const System *sys = nullptr;
    QVector<int> freeVar;               // free index -> global variable
    QVector<int> rowStart;              // per constraint: first residual row
    QVector<int> rowCount;
    int rows = 0;
    QVector<QVector<int>> consOfVar;    // free index -> constraints that use it

    void setup(const System &s, const QVector<bool> &isFree)
    {
        sys = &s;
        for (int i = 0; i < isFree.size(); ++i)
            if (isFree[i]) freeVar << i;
        QVector<double> x = s.x();
        double buf[4];
        for (const Constraint &c : s.constraints()) {
            rowStart << rows;
            const int n = s.residuals(c, x.data(), buf);
            rowCount << n;
            rows += n;
        }
        // Which constraints feel each free variable: wiggle it and look.
        consOfVar.resize(freeVar.size());
        QVector<double> base(rows);
        evalAll(x.data(), base.data());
        for (int j = 0; j < freeVar.size(); ++j) {
            const int g = freeVar[j];
            const double keep = x[g];
            x[g] = keep + 1.2345e-3 + std::fabs(keep) * 1e-4;
            for (int ci = 0; ci < s.constraints().size(); ++ci) {
                const int n = s.residuals(s.constraints()[ci], x.data(), buf);
                for (int k = 0; k < n; ++k)
                    if (std::fabs(buf[k] - base[rowStart[ci] + k]) > 1e-15) {
                        consOfVar[j] << ci;
                        break;
                    }
            }
            x[g] = keep;
        }
    }
    void evalAll(const double *x, double *r) const
    {
        for (int ci = 0; ci < sys->constraints().size(); ++ci)
            sys->residuals(sys->constraints()[ci], x, r + rowStart[ci]);
    }
    // Column-major Jacobian (rows x free) by central differences.
    void jacobian(QVector<double> &x, QVector<double> &J) const
    {
        J.fill(0, rows * freeVar.size());
        double plus[4], minus[4];
        for (int j = 0; j < freeVar.size(); ++j) {
            const int g = freeVar[j];
            const double keep = x[g];
            const double h = 1e-6 * qMax(1.0, std::fabs(keep));
            for (int ci : consOfVar[j]) {
                const Constraint &c = sys->constraints()[ci];
                x[g] = keep + h;
                const int n = sys->residuals(c, x.data(), plus);
                x[g] = keep - h;
                sys->residuals(c, x.data(), minus);
                for (int k = 0; k < n; ++k)
                    J[j * rows + rowStart[ci] + k] = (plus[k] - minus[k]) / (2 * h);
            }
            x[g] = keep;
        }
    }
};

QVector<bool> freeMask(const System &s)
{
    QVector<bool> isFree(s.x().size(), true);
    for (const Body &b : s.bodies()) {
        if (b.locked)
            for (int k = 0; k < b.n; ++k)
                isFree[b.off + k] = false;
        if (b.rounded)
            isFree[b.off + 2] = isFree[b.off + 3] = false;
    }
    return isFree;
}

QVector<Constraint> withValues(const Document &doc, QVector<Constraint> cons)
{
    const QHash<QString, double> vals = values(doc, doc.parametric());
    for (Constraint &c : cons)
        if (c.isDimension() && vals.contains(c.name))
            c.value = vals.value(c.name);
    return cons;
}

} // namespace

// ---- public geometry ---------------------------------------------------------------

static bool oneRef(const Document &doc, const GeoRef &r, System **out)
{
    Constraint c;
    c.type = Constraint::Fix;
    c.refs = {r};
    *out = new System(doc, {c});
    return !(*out)->constraints().isEmpty();
}

bool point(const Document &doc, const GeoRef &r, QPointF *p)
{
    if (r.isOrigin()) { *p = QPointF(0, 0); return true; }
    System *s = nullptr;
    const bool ok = oneRef(doc, r, &s) && s->point(s->x().data(), r, p);
    delete s;
    return ok;
}

bool line(const Document &doc, const GeoRef &r, QPointF *a, QPointF *b)
{
    System *s = nullptr;
    const bool ok = r.kind == GeoRef::Line && oneRef(doc, r, &s) && s->line(s->x().data(), r, a, b);
    delete s;
    return ok;
}

bool circle(const Document &doc, const GeoRef &r, QPointF *c, double *radius)
{
    System *s = nullptr;
    const bool ok = r.kind == GeoRef::Circle && oneRef(doc, r, &s)
                    && s->circ(s->x().data(), r, c, radius);
    delete s;
    return ok;
}

bool resolves(const Document &doc, const GeoRef &r)
{
    if (r.isOrigin())
        return r.kind == GeoRef::Point;
    System *s = nullptr;
    const bool ok = oneRef(doc, r, &s);
    delete s;
    return ok;
}

bool resolves(const Document &doc, const Constraint &c)
{
    System s(doc, {c});
    return !s.constraints().isEmpty();
}

double measure(const Document &doc, const Constraint &c)
{
    System s(doc, {c});
    if (s.constraints().isEmpty())
        return 0;
    return s.measured(c, s.x().data());
}

void prime(const Document &doc, Constraint &c)
{
    System s(doc, {c});
    if (s.constraints().isEmpty())
        return;
    const double *x = s.x().data();
    using T = Constraint;
    QPointF p, q, a, b, a2, b2, c1, c2;
    double r1 = 0, r2 = 0;
    c.sense = 1;
    switch (c.type) {
    case T::Fix:
        if (c.refs[0].kind == K::Point && s.point(x, c.refs[0], &p))
            c.at = p;
        break;
    case T::Tangent:
        if (c.refs[0].kind == K::Line) {
            s.line(x, c.refs[0], &a, &b);
            s.circ(x, c.refs[1], &c1, &r1);
            c.sense = side(c1, a, b) < 0 ? -1 : 1;
        } else {
            s.circ(x, c.refs[0], &c1, &r1);
            s.circ(x, c.refs[1], &c2, &r2);
            const double d = len(c2 - c1);
            const double ext = std::fabs(d - (std::fabs(r1) + std::fabs(r2)));
            const double in = std::fabs(d - std::fabs(std::fabs(r1) - std::fabs(r2)));
            c.sense = ext <= in ? 1 : -1;
        }
        break;
    case T::Distance:
        if (kinds(c, {K::Point, K::Line})) {
            s.point(x, c.refs[0], &p);
            s.line(x, c.refs[1], &a, &b);
            c.sense = side(p, a, b) < 0 ? -1 : 1;
        } else if (kinds(c, {K::Line, K::Line})) {
            s.line(x, c.refs[0], &a, &b);
            s.line(x, c.refs[1], &a2, &b2);
            c.sense = side((a2 + b2) / 2, a, b) < 0 ? -1 : 1;
        }
        break;
    case T::HDistance:
    case T::VDistance:
        if (kinds(c, {K::Line}))
            s.line(x, c.refs[0], &p, &q);
        else {
            s.point(x, c.refs[0], &p);
            s.point(x, c.refs[1], &q);
        }
        c.sense = (c.type == T::HDistance ? q.x() - p.x() : q.y() - p.y()) < 0 ? -1 : 1;
        break;
    case T::Angle:
        s.line(x, c.refs[0], &a, &b);
        s.line(x, c.refs[1], &a2, &b2);
        c.sense = std::atan2(cross(b - a, b2 - a2), dot(b - a, b2 - a2)) < 0 ? -1 : 1;
        break;
    default:
        break;
    }
    if (c.isDimension()) {
        c.value = s.measured(c, x);
        if (c.expr.trimmed().isEmpty())
            c.expr = formatNumber(c.value);
    }
}

QVector<RefShape> shapesOf(const Document &doc, const QString &id)
{
    QVector<RefShape> out;
    const Element *e = nullptr;
    for (const Element &el : doc.elements())
        if (el.id == id) { e = &el; break; }
    Body b;
    if (!e || !bodyFor(*e, &b))
        return out;
    // Every anchor of a path as a variable, so the shapes read straight off x.
    if (b.t == Body::Path)
        for (int s = 0; s < b.model.subs.size(); ++s)
            for (int i = 0; i < b.model.subs.at(s).nodes.size(); ++i) {
                GeoRef r;
                r.id = id; r.sub = s; r.index = i;
                claim(b, r);
            }
    QVector<double> x(b.n);
    initial(b, x.data());
    auto add = [&](K k, int sub, int index) {
        RefShape rs;
        rs.ref.kind = k; rs.ref.id = id; rs.ref.sub = sub; rs.ref.index = index;
        if (k == K::Point)
            bodyPoint(b, x.constData(), rs.ref, &rs.a);
        else if (k == K::Line)
            bodyLine(b, x.constData(), rs.ref, &rs.a, &rs.b);
        else
            bodyCircle(b, x.constData(), &rs.a, &rs.r);
        out << rs;
    };
    switch (b.t) {
    case Body::Circle:
        add(K::Circle, 0, -1);
        add(K::Point, 0, -1);
        break;
    case Body::SketchPoint:
        add(K::Point, 0, -1);
        break;
    case Body::Rect:
        add(K::Point, 0, -1);
        for (int i = 0; i < 4; ++i) { add(K::Point, 0, i); add(K::Line, 0, i); }
        break;
    case Body::Polygon:
        add(K::Point, 0, -1);
        for (int i = 0; i < b.sides; ++i) { add(K::Point, 0, i); add(K::Line, 0, i); }
        break;
    case Body::Path:
        for (int s = 0; s < b.model.subs.size(); ++s) {
            const SubPath &sp = b.model.subs.at(s);
            for (int i = 0; i < sp.nodes.size(); ++i)
                add(K::Point, s, i);
            // Only straight segments act as lines.
            for (int i = 0; i < sp.segmentCount(); ++i) {
                const PathNode *na, *nb;
                sp.segment(i, &na, &nb);
                if (!na->hasOut() && !nb->hasIn())
                    add(K::Line, s, i);
            }
        }
        break;
    }
    return out;
}

QSet<QString> lockedElements(const Document &doc)
{
    QSet<QString> out;
    for (const Constraint &c : doc.parametric().constraints)
        if (c.type == Constraint::Fix && !c.refs.isEmpty() && c.refs.first().kind != GeoRef::Point)
            out.insert(c.refs.first().id);
    return out;
}

QVector<GeoRef> refsOf(const Document &doc, const QString &id)
{
    QVector<GeoRef> out;
    for (const RefShape &s : shapesOf(doc, id))
        out << s.ref;
    return out;
}

// ---- solve ------------------------------------------------------------------------

SolveResult solve(Document &doc, const QSet<QString> &keep, bool force)
{
    SolveResult res;
    if (doc.parametric().constraints.isEmpty())
        return res;
    // Dimensions take the values of their expressions first.
    {
        const QHash<QString, double> vals = values(doc, doc.parametric());
        for (Constraint &c : doc.parametricRef().constraints)
            if (c.isDimension() && vals.contains(c.name))
                c.value = vals.value(c.name);
    }
    System sys(doc, doc.parametric().constraints);
    if (sys.constraints().isEmpty())
        return res;
    QVector<double> &x = sys.x();
    const QVector<double> x0 = x;

    // The size variables of rectangles, polygons and circles: weighted so
    // the minimum-norm step moves a shape before it resizes it, and held
    // outright when a free solve would turn one inside out.
    auto isSize = [&](int var) {
        for (const Body &b : sys.bodies())
            if (var >= b.off && var < b.off + b.n) {
                const int k = var - b.off;
                return (b.t == Body::Rect && (k == 2 || k == 3))
                    || ((b.t == Body::Polygon || b.t == Body::Circle) && k == 2);
            }
        return false;
    };
    auto cost = [&](const QVector<double> &rv) {
        double s = 0;
        for (double v : rv) s += v * v;
        return s;
    };
    auto worst = [&](const QVector<double> &rv) {
        double s = 0;
        for (double v : rv) s = qMax(s, std::fabs(v));
        return s;
    };

    struct Run { QVector<double> x, r; int it = 0; QSet<QString> failing; bool insideOut = false; };
    auto isKept = [&](int var) {
        for (const Body &b : sys.bodies())
            if (var >= b.off && var < b.off + b.n)
                return keep.contains(b.id);
        return false;
    };
    auto run = [&](bool holdSizes, bool holdKept) {
        Run out;
        out.x = x0;
        QVector<bool> isFree = freeMask(sys);
        for (int k = 0; k < isFree.size(); ++k)
            if (isFree[k] && ((holdSizes && isSize(k)) || (holdKept && isKept(k))))
                isFree[k] = false;
        Problem pb;
        pb.setup(sys, isFree);
        const int m = pb.rows, n = pb.freeVar.size();

        // Damping weights: the minimum-norm step changes cheap variables
        // first. Sizes cost 1e2, so shapes move before they resize; what
        // the edit touched costs 1e4.
        QVector<double> w(n, 1.0);
        for (int j = 0; j < n; ++j)
            if (isSize(pb.freeVar[j]))
                w[j] = 1e2;
        for (const Body &b : sys.bodies())
            if (keep.contains(b.id))
                for (int j = 0; j < n; ++j)
                    if (pb.freeVar[j] >= b.off && pb.freeVar[j] < b.off + b.n)
                        w[j] = 1e4;

        QVector<double> &xr = out.x;
        QVector<double> &r = out.r;
        r.resize(m);
        QVector<double> rTry(m), J, A, g, xTry;
        pb.evalAll(xr.data(), r.data());
        double c = cost(r);
        double mu = 1e-3;
        int it = 0;
        if (n > 0) {
            for (; it < 300 && worst(r) > 1e-10; ++it) {
                pb.jacobian(xr, J);
                A.fill(0, n * n);
                g.fill(0, n);
                for (int i = 0; i < n; ++i) {
                    const double *ci = J.constData() + i * m;
                    for (int k = 0; k < m; ++k)
                        g[i] += ci[k] * r[k];
                    for (int j = i; j < n; ++j) {
                        const double *cj = J.constData() + j * m;
                        double s = 0;
                        for (int k = 0; k < m; ++k)
                            s += ci[k] * cj[k];
                        A[i * n + j] = A[j * n + i] = s;
                    }
                }
                bool stepped = false;
                while (mu < 1e12) {
                    QVector<double> Ad = A, d(n);
                    for (int i = 0; i < n; ++i) {
                        Ad[i * n + i] += mu * w[i];
                        d[i] = -g[i];
                    }
                    if (gauss(Ad, d, n)) {
                        xTry = xr;
                        for (int i = 0; i < n; ++i)
                            xTry[pb.freeVar[i]] += d[i];
                        pb.evalAll(xTry.data(), rTry.data());
                        const double cTry = cost(rTry);
                        if (cTry < c) {
                            xr = xTry;
                            r = rTry;
                            c = cTry;
                            mu = qMax(mu / 4, 1e-9);
                            stepped = true;
                            break;
                        }
                    }
                    mu *= 5;
                }
                if (!stepped)
                    break;
            }
        }
        out.it = it;
        for (int ci = 0; ci < sys.constraints().size(); ++ci)
            for (int k = 0; k < pb.rowCount[ci]; ++k)
                if (std::fabs(r[pb.rowStart[ci] + k]) > 1e-6) {
                    out.failing << sys.constraints()[ci].uid;
                    break;
                }
        // A rectangle or polygon driven through zero is a solution only on
        // paper: written() can only build it the right way out, where the
        // constraints that put it there no longer hold.
        for (const Body &b : sys.bodies()) {
            const double *v = xr.constData() + b.off;
            const bool insideOut = (b.t == Body::Rect && !b.rounded && (v[2] <= 0 || v[3] <= 0))
                                 || (b.t == Body::Polygon && v[2] <= 0);
            if (!insideOut)
                continue;
            out.insideOut = true;
            for (const Constraint &c : sys.constraints())
                for (const GeoRef &r : c.refs)
                    if (r.id == b.id) {
                        out.failing << c.uid;
                        break;
                    }
        }
        return out;
    };

    // How far a run moved what the edit asked to keep where it put it.
    auto keptShift = [&](const Run &rr) {
        double s = 0;
        for (const Body &b : sys.bodies())
            if (keep.contains(b.id))
                for (int k = 0; k < b.n; ++k)
                    s += std::fabs(rr.x[b.off + k] - x0[b.off + k]);
        return s;
    };
    // The weights make resizing and moving what was edited dear, not
    // impossible: a free shape on the end of something dragged still gives
    // a little (and goes through zero size when dragged far enough), and
    // the dragged thing lags a little behind a shape that has to stretch.
    // When the constraints can hold with those kept exactly as they are,
    // that is the answer: the shapes follow by moving, the dragged thing
    // stays where it was put.
    Run best = run(false, false);
    bool resized = false;
    for (int k = 0; k < x0.size() && !resized; ++k)
        resized = isSize(k) && std::fabs(best.x[k] - x0[k]) > 1e-9;
    bool exact = false;
    if (resized) {
        Run held = run(true, !keep.isEmpty());
        if (held.failing.isEmpty()) {
            best = held;
            exact = true;
        } else if (!keep.isEmpty()) {
            held = run(true, false);
            if (held.failing.isEmpty() && (best.insideOut || keptShift(held) <= 2 * keptShift(best) + 1e-6))
                best = held;
        }
    }
    if (!exact && !keep.isEmpty() && keptShift(best) > 1e-9) {
        Run held = run(false, true);
        if (held.failing.isEmpty())
            best = held;
    }
    x = best.x;
    res.iterations = best.it;
    res.residual = worst(best.r);
    // Keep the constraint order of the system for the failing list.
    for (const Constraint &c : sys.constraints())
        if (best.failing.contains(c.uid))
            res.failing << c.uid;
    // A line the constraints can only meet by shrinking it to nothing
    // (Horizontal and Vertical on the same line) is a conflict, not a
    // solution: the constraints on it fail rather than the line vanishing.
    for (const Constraint &c : sys.constraints()) {
        if (res.failing.contains(c.uid))
            continue;
        for (const GeoRef &ref : c.refs) {
            if (ref.kind != GeoRef::Line)
                continue;
            QPointF a, b, a0, b0;
            if (!sys.line(x.constData(), ref, &a, &b) || !sys.line(x0.constData(), ref, &a0, &b0))
                continue;
            if (len(b - a) < 1e-6 && len(b0 - a0) > 1e-6) {
                res.failing << c.uid;
                break;
            }
        }
    }
    res.ok = res.failing.isEmpty();
    if (!res.ok && !force)
        return res;
    // Build every changed element before replacing any: the bodies point into
    // the document's element vector, which a replace may reallocate.
    QVector<Element> out;
    for (const Body &b : sys.bodies()) {
        bool moved = false;
        for (int k = 0; k < b.n; ++k)
            if (std::fabs(x[b.off + k] - x0[b.off + k]) > 1e-9)
                moved = true;
        if (moved)
            out << written(b, x.constData());
    }
    for (const Element &e : out) {
        doc.replaceElement(e);
        res.changed.insert(e.id);
    }
    return res;
}

static QJsonValue flatValue(const QJsonObject &j, const QString &key)
{
    const int dot = key.indexOf(QLatin1Char('.'));
    if (dot < 0)
        return j.value(key);
    return j.value(key.left(dot)).toObject().value(key.mid(dot + 1));
}

static void flatPut(QJsonObject &j, const QString &key, const QJsonValue &v)
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

QStringList applyToolpathBindings(Document &doc, QStringList *problems)
{
    QStringList changed;
    const QHash<QString, double> vals = values(doc, doc.parametric());
    const Lookup look = [&](const QString &n, double *v) {
        auto it = vals.constFind(n);
        if (it == vals.constEnd()) return false;
        *v = it.value();
        return true;
    };
    auto numberOf = [](const QJsonValue &j) {
        return j.isString() ? j.toString().toDouble() : j.toDouble();
    };
    for (Toolpath &t : doc.toolpathsRef()) {
        const QJsonObject binds = t.json.value(QStringLiteral("phobic_expr")).toObject();
        if (binds.isEmpty())
            continue;
        const QString tpName = t.json.value(QStringLiteral("name")).toString();
        QJsonObject j = t.json;
        for (auto it = binds.begin(); it != binds.end(); ++it) {
            const QJsonObject b = it.value().toObject();
            const QString expr = b.value("expr").toString();
            const bool neg = b.value("neg").toBool();
            // Depths are shown and ranged as their size, the sign kept apart.
            const bool size = neg || b.value("abs").toBool();
            double v = 0;
            if (!evaluate(expr, look, &v)) {
                if (problems) {
                    const double kept = numberOf(flatValue(j, it.key()));
                    *problems << QStringLiteral("toolpath %1: %2 = %3 can't be worked out, kept %4")
                                     .arg(tpName, it.key(), expr,
                                          formatNumber(size ? std::fabs(kept) : kept));
                }
                continue;
            }
            if (size)
                v = std::fabs(v);
            // The field's range, as the form had it when it bound the field
            // (bindings made before it was stored have none).
            const double want = v;
            if (b.contains("min"))
                v = qMax(v, b.value("min").toDouble());
            if (b.contains("max"))
                v = qMin(v, b.value("max").toDouble());
            if (v != want && problems)
                *problems << QStringLiteral("toolpath %1: %2 = %3 is %4, out of range - set to %5")
                                 .arg(tpName, it.key(), expr, formatNumber(want), formatNumber(v));
            if (neg)
                v = -v;
            const QJsonValue nv = b.value("str").toBool()
                ? QJsonValue(QString::number(v, 'f', 3)) : QJsonValue(v);
            const QJsonValue old = flatValue(j, it.key());
            const bool same = old.isString() == nv.isString()
                && std::fabs(numberOf(old) - numberOf(nv)) < 1e-9;
            if (!same)
                flatPut(j, it.key(), nv);
        }
        if (j != t.json) {
            t.json = j;
            changed << t.uuid;
        }
    }
    return changed;
}

Freedom freedom(const Document &doc)
{
    Freedom f;
    const QVector<Constraint> cons = withValues(doc, doc.parametric().constraints);
    System sys(doc, cons);
    if (sys.constraints().isEmpty())
        return f;
    const QVector<bool> isFree = freeMask(sys);
    Problem pb;
    pb.setup(sys, isFree);
    const int m = pb.rows, n = pb.freeVar.size();
    QVector<double> J;
    pb.jacobian(sys.x(), J);

    // Row-reduce J (m x n) and see which variables the null space leaves alone.
    QVector<double> R(m * n);
    double big = 0;
    for (int i = 0; i < m; ++i)
        for (int j = 0; j < n; ++j) {
            R[i * n + j] = J[j * m + i];
            big = qMax(big, std::fabs(R[i * n + j]));
        }
    const double tol = 1e-8 * qMax(1.0, big);
    QVector<int> pivotCol;
    int row = 0;
    for (int col = 0; col < n && row < m; ++col) {
        int piv = row;
        double best = std::fabs(R[row * n + col]);
        for (int i = row + 1; i < m; ++i)
            if (std::fabs(R[i * n + col]) > best) { best = std::fabs(R[i * n + col]); piv = i; }
        if (best <= tol)
            continue;
        for (int k = 0; k < n; ++k)
            std::swap(R[row * n + k], R[piv * n + k]);
        const double d = R[row * n + col];
        for (int k = 0; k < n; ++k)
            R[row * n + k] /= d;
        for (int i = 0; i < m; ++i) {
            if (i == row) continue;
            const double fct = R[i * n + col];
            if (fct == 0) continue;
            for (int k = 0; k < n; ++k)
                R[i * n + k] -= fct * R[row * n + k];
        }
        pivotCol << col;
        ++row;
    }
    f.dof = n - pivotCol.size();
    QVector<bool> isPivot(n, false), determined(n, false);
    for (int c : pivotCol) isPivot[c] = true;
    for (int r = 0; r < pivotCol.size(); ++r) {
        bool alone = true;
        for (int col = 0; col < n; ++col)
            if (!isPivot[col] && std::fabs(R[r * n + col]) > 1e-7) { alone = false; break; }
        determined[pivotCol[r]] = alone;
    }
    QVector<bool> globalDet(sys.x().size(), true);
    for (int j = 0; j < n; ++j)
        globalDet[pb.freeVar[j]] = determined[j];
    for (const Body &b : sys.bodies()) {
        if (b.t == Body::Path && b.nodeVar.size() < b.pathNodes && !b.locked)
            continue;   // anchors no constraint touches can still move
        bool all = true;
        for (int k = 0; k < b.n; ++k)
            if (!globalDet[b.off + k]) { all = false; break; }
        if (all)
            f.fullyConstrained.insert(b.id);
    }
    return f;
}

QStringList unsatisfied(const Document &doc)
{
    QStringList out;
    const QVector<Constraint> cons = withValues(doc, doc.parametric().constraints);
    System sys(doc, cons);
    double buf[4];
    for (const Constraint &c : sys.constraints()) {
        const int n = sys.residuals(c, sys.x().constData(), buf);
        for (int k = 0; k < n; ++k)
            if (std::fabs(buf[k]) > 1e-6) {
                out << c.uid;
                break;
            }
    }
    return out;
}

namespace {

const Element *elementIn(const Document &doc, const QString &id)
{
    for (const Element &e : doc.elements())
        if (e.id == id)
            return &e;
    return nullptr;
}

// What a reference's numbers mean: the element kind and, for a path, how
// many nodes each subpath has. While it holds, (sub, index) still names
// the same node.
QString layoutOf(const Element &e)
{
    QString out = e.geometryType;
    if (e.geometryType == QLatin1String("path"))
        for (const SubPath &s : Element::pathModel(e).subs)
            out += QStringLiteral("|%1%2").arg(s.nodes.size()).arg(s.closed ? 'c' : 'o');
    // A polygon given another side count renumbers its edges and vertices.
    if (e.geometryType == QLatin1String("regular_polygon"))
        out += QStringLiteral("|%1").arg(e.raw.value("num_sides").toInt());
    return out;
}

bool near(QPointF a, QPointF b) { return std::hypot(a.x() - b.x(), a.y() - b.y()) < 1e-4; }

// The reference in `doc` to the same geometry `r` was in `before`: the
// point, line or circle at the same place - where `t` took it, for an
// element the edit transformed. A line found running the other way is
// taken reversed, so it keeps its direction. False when there is none.
bool follow(const Document &before, const Document &doc, const GeoRef &r, GeoRef *out,
            const QTransform &t = QTransform())
{
    QPointF a, b, c;
    double rad = 0;
    if (r.kind == GeoRef::Point && !point(before, r, &a))
        return false;
    if (r.kind == GeoRef::Line && !line(before, r, &a, &b))
        return false;
    if (r.kind == GeoRef::Circle && !circle(before, r, &c, &rad))
        return false;
    a = t.map(a);
    b = t.map(b);
    c = t.map(c);
    rad *= std::sqrt(std::fabs(t.determinant()));
    // Several can sit at the same place (a closed path's last node on its
    // first): take the one numbered closest to where it was.
    bool found = false;
    int best = 0;
    for (const RefShape &s : shapesOf(doc, r.id)) {
        if (s.ref.kind != r.kind)
            continue;
        const bool ahead = r.kind == GeoRef::Line && near(s.a, a) && near(s.b, b);
        const bool back = r.kind == GeoRef::Line && !ahead && near(s.a, b) && near(s.b, a);
        const bool same = r.kind == GeoRef::Point ? near(s.a, a)
                        : r.kind == GeoRef::Line  ? ahead || back
                                                  : near(s.a, c) && std::fabs(s.r - rad) < 1e-4;
        // A rectangle or polygon center is never one of its corners.
        if (!same || (r.kind == GeoRef::Point && (s.ref.index == -1) != (r.index == -1)))
            continue;
        const int score = std::abs(s.ref.sub - r.sub) * 100000 + std::abs(s.ref.index - r.index);
        if (!found || score < best) {
            *out = s.ref;
            out->reversed = back;
            best = score;
            found = true;
        }
    }
    return found;
}

} // namespace

QSet<QString> usedNames(const Document &doc, const Parametric &p)
{
    QSet<QString> used;
    for (const UserParameter &up : p.params)
        for (const QString &n : namesIn(up.expr)) used.insert(n);
    for (const Constraint &c : p.constraints)
        if (c.isDimension())
            for (const QString &n : namesIn(c.expr)) used.insert(n);
    for (const Toolpath &tp : doc.toolpaths()) {
        const QJsonObject binds = tp.json.value(QStringLiteral("phobic_expr")).toObject();
        for (auto it = binds.constBegin(); it != binds.constEnd(); ++it)
            for (const QString &n : namesIn(it.value().toObject().value(QStringLiteral("expr")).toString()))
                used.insert(n);
    }
    return used;
}

int prune(Document &doc)
{
    return reconcile(doc, doc);   // same numbering: only what no longer resolves goes
}

int reconcile(const Document &before, Document &doc, QStringList *dropped, bool *changed,
              const QHash<QString, QTransform> &moved)
{
    if (changed) *changed = false;
    Parametric p = doc.parametric();
    if (p.constraints.isEmpty())
        return 0;
    QHash<QString, bool> sameLayout;     // element id -> its numbering still holds
    auto keepsNumbers = [&](const QString &id) {
        auto it = sameLayout.constFind(id);
        if (it != sameLayout.constEnd())
            return it.value();
        const Element *o = elementIn(before, id), *n = elementIn(doc, id);
        const bool same = o && n && layoutOf(*o) == layoutOf(*n);
        sameLayout.insert(id, same);
        return same;
    };

    QVector<Constraint> kept;
    QVector<Constraint> gone;
    bool renumbered = false;
    for (Constraint c : p.constraints) {
        bool ok = true;
        for (GeoRef &r : c.refs) {
            if (r.isOrigin())
                continue;
            GeoRef to;
            const auto t = moved.constFind(r.id);
            if (t != moved.constEnd() && !t.value().isIdentity()) {
                // A transformed element may keep its layout and still
                // renumber (a rectangle flipped over keeps corner 0 at the
                // lower left): follow the geometry through the transform.
                // Nothing there (a transform that bent the shape) falls
                // back to the old numbers while they still hold.
                if (!follow(before, doc, r, &to, t.value())) {
                    if (keepsNumbers(r.id))
                        continue;
                    ok = false;
                    break;
                }
            } else if (keepsNumbers(r.id)) {
                continue;
            } else if (!follow(before, doc, r, &to)) {
                ok = false;
                break;
            }
            renumbered = renumbered || !(to == r) || to.reversed != r.reversed;
            r = to;
        }
        if (ok && resolves(doc, c))
            kept << c;
        else
            gone << c;
    }
    p.constraints = kept;
    if (gone.isEmpty()) {
        if (renumbered) {
            doc.setParametric(p);       // references moved to new numbers
            if (changed) *changed = true;
        }
        return 0;
    }

    // A dropped dimension other expressions still use lives on as a user
    // parameter of the same name and value, so they keep working.
    // A converted dimension can use another dropped one (d2 = d1 * 2), so
    // go round until no more are needed.
    for (const Constraint &c : gone)
        if (dropped) *dropped << c.uid;
    QSet<QString> asParams;
    for (bool more = true; more;) {
        more = false;
        const QSet<QString> used = usedNames(doc, p);
        for (const Constraint &c : gone) {
            if (!c.isDimension() || c.name.isEmpty() || asParams.contains(c.name) || !used.contains(c.name))
                continue;
            UserParameter up;
            up.name = c.name;
            up.expr = c.expr.trimmed().isEmpty() ? formatNumber(c.value) : c.expr;
            up.comment = QStringLiteral("was a dimension on deleted geometry");
            p.params << up;
            asParams.insert(c.name);
            more = true;
        }
    }
    doc.setParametric(p);
    if (changed) *changed = true;
    return gone.size();
}

} // namespace param
} // namespace c2d
