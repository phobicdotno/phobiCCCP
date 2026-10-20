#pragma once
#include <QHash>
#include <QJsonObject>
#include <QPointF>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

// Fusion's parametric sketching: user parameters, geometric constraints and
// driving dimensions, and the solver that keeps the drawing true to them.
//
// Everything here is plain data plus pure functions over a Document, so the
// canvas, the panels and the tests share it. The whole set is stored in the
// .c2d as one JSON string under params.phobic_parametric (Carbide Create
// ignores keys it does not know); toolpath fields bound to an expression
// carry it in the toolpath's own `phobic_expr` object.
//
// Lengths are millimetres, angles degrees.
namespace c2d {

class Document;
class Element;

namespace param {

// ---- expressions ---------------------------------------------------------
// + - * / ^, parentheses, unary minus, numbers with an optional unit
// (mm cm m in ft deg rad), names, pi, and the functions sin cos tan (degrees),
// asin acos atan atan2 (to degrees), sqrt abs min max round floor ceil.
// `lookup` resolves a name to its value; false = unknown name.
using Lookup = std::function<bool(const QString &name, double *value)>;
bool evaluate(const QString &expr, const Lookup &lookup, double *value, QString *error = nullptr);
// The names an expression refers to (functions, units and pi left out).
QStringList namesIn(const QString &expr);
// A usable parameter name: a letter or _, then letters, digits or _, and not
// a function, a unit, pi or one of the stock built-ins.
bool isValidName(const QString &name);
// Stock built-ins every expression can use: stock_w, stock_h, stock_t.
QStringList builtinNames();
// A number as an expression would show it: up to 3 decimals, no trailing 0s.
QString formatNumber(double v);
// `expr` with the name `from` replaced by `to` wherever it stands alone.
QString renameIn(const QString &expr, const QString &from, const QString &to);

} // namespace param

struct UserParameter {
    QString name;
    QString expr;       // "120", "plate_w - 2 * margin", "1/4 in"
    QString comment;
};

// A piece of geometry a constraint refers to.
//   Point:  a path anchor (sub, index); a circle / polygon / rectangle center
//           (index -1); a rectangle corner (0 BL, 1 BR, 2 TR, 3 TL) or a
//           polygon vertex (index >= 0). id "origin" is the board's 0,0.
//   Line:   a straight path segment (sub, index = segment), a rectangle edge
//           (0 bottom, 1 right, 2 top, 3 left) or a polygon edge.
//   Circle: a circle element (sketch points too: their center).
struct GeoRef {
    enum Kind { Point = 0, Line = 1, Circle = 2 };
    Kind kind = Point;
    QString id;
    int sub = 0;
    int index = -1;

    bool operator==(const GeoRef &o) const
    {
        return kind == o.kind && id == o.id && sub == o.sub && index == o.index;
    }
    bool isOrigin() const { return id == QLatin1String("origin"); }
    static GeoRef origin() { GeoRef r; r.id = QStringLiteral("origin"); return r; }
};

struct Constraint {
    // Geometric constraints, then driving dimensions (isDimension()).
    enum Type {
        Coincident, Horizontal, Vertical, Parallel, Perpendicular, Tangent, Equal,
        Concentric, Midpoint, Collinear, Fix, Symmetric,
        Distance, HDistance, VDistance, Radius, Diameter, Angle
    };
    Type type = Coincident;
    QVector<GeoRef> refs;
    QString uid;            // stable id within the document
    QString name;           // dimensions: d1, d2 ... (usable in expressions)
    QString expr;           // dimensions: the driving value as typed
    double value = 0;       // dimensions: the evaluated value
    int sense = 1;          // which side / branch the constraint holds on (+1/-1)
    QPointF at;             // Fix on a point: where it is pinned
    QPointF label;          // dimensions: text offset from the middle of what it measures (mm)

    bool isDimension() const { return type >= Distance; }
    static QString typeName(Type t);        // "Coincident", "Distance" ...
    static QString symbol(Type t);          // the glyph drawn on the canvas
};

class Parametric
{
public:
    QVector<UserParameter> params;
    QVector<Constraint> constraints;

    bool isEmpty() const { return params.isEmpty() && constraints.isEmpty(); }
    QJsonObject toJson() const;
    static Parametric fromJson(const QJsonObject &o);
    // The .c2d params key the set is stored under.
    static QString paramKey() { return QStringLiteral("phobic_parametric"); }

    QString nextDimensionName() const;      // d1, d2, ... not yet in use
    QString nextUid() const;
    int indexOfUid(const QString &uid) const;
    int indexOfParam(const QString &name) const;
    bool nameInUse(const QString &name) const;   // parameter or dimension
    // Rename a parameter or dimension everywhere it appears in an expression.
    void renameEverywhere(const QString &from, const QString &to);
};

namespace param {

// Values of every name an expression in `doc` can use: the stock built-ins,
// the user parameters and the dimensions (by their names), each evaluated in
// dependency order. Names that fail (unknown name, cycle, bad syntax) are
// left out and reported in *errors as "name: why".
QHash<QString, double> values(const Document &doc, const Parametric &p,
                              QStringList *errors = nullptr);
// The first error ("name: why") that `after` has and `before` hadn't, or
// empty. Catches loops through other names ("p = d1 + 1", then d1 = p),
// which checking the edited expression against the old values misses, and
// dimensions driven to a value they can't have (see badValue).
QString newError(const Document &doc, const Parametric &before, const Parametric &after);
// Why `v` can't be the value of dimension `c` (a radius, diameter or line
// length of 0 or below, an angle outside 0..180 degrees), or empty. newError() checks every dimension too.
QString badValue(const Constraint &c, double v);
// Evaluate `expr` against the document's current parameters.
bool evaluateIn(const Document &doc, const QString &expr, double *value,
                QString *error = nullptr);

// ---- geometry -------------------------------------------------------------
// Where a reference sits right now; false when it does not resolve (element
// gone, index out of range, wrong kind of element).
bool point(const Document &doc, const GeoRef &r, QPointF *p);
bool line(const Document &doc, const GeoRef &r, QPointF *a, QPointF *b);
bool circle(const Document &doc, const GeoRef &r, QPointF *c, double *radius);
bool resolves(const Document &doc, const GeoRef &r);
bool resolves(const Document &doc, const Constraint &c);
// What the constraint measures right now (dimensions: distance, radius,
// diameter or angle; others 0) - the value a new dimension starts at.
double measure(const Document &doc, const Constraint &c);
// Fill in sense / at / value for a constraint about to be added, from how
// the geometry lies now (which side, which tangency, the current measure).
void prime(const Document &doc, Constraint &c);
// Every pickable reference of one element (points, lines, circles) and
// where it is: a point at `a`, a line from `a` to `b`, a circle about `a`
// with radius `r`. One decode of the element for all of them.
struct RefShape {
    GeoRef ref;
    QPointF a, b;
    double r = 0;
};
QVector<RefShape> shapesOf(const Document &doc, const QString &id);
QVector<GeoRef> refsOf(const Document &doc, const QString &id);

// ---- solving --------------------------------------------------------------
struct SolveResult {
    bool ok = true;                 // every constraint holds (to 1e-6 mm)
    double residual = 0;            // the worst one left over
    int iterations = 0;
    QStringList failing;            // uids of constraints that do not hold
    QSet<QString> changed;          // element ids the solver moved
};

// Re-evaluate the dimensions from their expressions, then move the geometry
// until every constraint holds, changing as little as possible - and least
// of all the elements in `keep` (the ones the user just edited). Elements
// with a Fix constraint on the whole element never move. The document's
// elements are only touched when the solve succeeds, unless `force`.
SolveResult solve(Document &doc, const QSet<QString> &keep = {}, bool force = false);

// Re-evaluate every toolpath field bound to an expression (`phobic_expr`),
// clamped to the field's range when the binding carries one (min / max, a
// depth's "abs" or "neg" ranging its size). Returns the uuids that changed.
// *problems gets one line for each binding that can't be worked out (it
// keeps its value) or was clamped.
QStringList applyToolpathBindings(Document &doc, QStringList *problems = nullptr);

struct Freedom {
    int dof = 0;                    // degrees of freedom the constrained part has left
    QSet<QString> fullyConstrained; // element ids that cannot move any more
};
Freedom freedom(const Document &doc);

// uids of the constraints that do not hold right now (worse than 1e-6 mm).
QStringList unsatisfied(const Document &doc);

// After an edit: keep every constraint on the geometry it was put on. An
// element whose node numbering changed (a node deleted or inserted, a
// fillet, a rectangle turned into a path) has its references moved to the
// point, line or circle at the same place in `doc`; constraints whose
// geometry is gone, or no longer a line, are dropped. A dropped dimension
// that expressions or toolpath fields still use becomes a user parameter of
// the same name. Returns how many were dropped (their uids in *dropped);
// *changed says whether the constraints changed at all.
int reconcile(const Document &before, Document &doc, QStringList *dropped = nullptr,
              bool *changed = nullptr);
// Every name the expressions in `p` and the document's toolpath fields use.
QSet<QString> usedNames(const Document &doc, const Parametric &p);
// reconcile() against the document itself: drops what no longer resolves.
int prune(Document &doc);

} // namespace param
} // namespace c2d
