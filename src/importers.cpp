#include "importers.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QTransform>
#include <QUuid>
#include <QXmlStreamReader>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <functional>

namespace c2d {

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

static QJsonArray xy(const QPointF &p) { return QJsonArray{p.x(), p.y()}; }

static bool samePoint(const QPointF &a, const QPointF &b, double eps = 1e-6)
{
    return std::fabs(a.x() - b.x()) < eps && std::fabs(a.y() - b.y()) < eps;
}

QString ImportResult::summary() const
{
    if (!ok)
        return QStringLiteral("Import failed: %1").arg(error);
    QString s = QStringLiteral("Imported %1 element(s)").arg(elements.size());
    if (!bounds.isNull())
        s += QStringLiteral(", %1 x %2 mm at (%3, %4)")
                 .arg(bounds.width(), 0, 'f', 2).arg(bounds.height(), 0, 'f', 2)
                 .arg(bounds.left(), 0, 'f', 2).arg(bounds.top(), 0, 'f', 2);
    if (relocated)
        s += QStringLiteral(" (moved to the stock margin)");
    if (skipped > 0)
        s += QStringLiteral("; %1 skipped").arg(skipped);
    return s;
}

// One CC `path` element per subpath. The row model mirrors Element::makePath
// and the CC-853 specimens: row 0 is the moveTo (type 0, cp1 = cp2 = point);
// a line row (type 1) has cp1 = previous anchor, cp2 = itself; a cubic row
// (type 3) carries its two control points in cp1/cp2; a closed contour ends
// with a segment arriving at the start followed by a close row (type 4,
// smooth 1). Coordinates are absolute, position [0,0].
QVector<Element> elementsFromPath(const QPainterPath &p, const QJsonObject &layer)
{
    struct Row { int type; QPointF pt, c1, c2; };
    QVector<QVector<Row>> subs;
    for (int i = 0; i < p.elementCount(); ++i) {
        const QPainterPath::Element e = p.elementAt(i);
        if (e.isMoveTo()) {
            subs.append({Row{0, QPointF(e.x, e.y), QPointF(e.x, e.y), QPointF(e.x, e.y)}});
        } else if (e.isLineTo()) {
            if (subs.isEmpty()) continue;
            const QPointF prev = subs.last().last().pt;
            subs.last().append(Row{1, QPointF(e.x, e.y), prev, QPointF(e.x, e.y)});
        } else if (e.isCurveTo()) {
            if (subs.isEmpty() || i + 2 >= p.elementCount()) continue;
            const QPainterPath::Element c2 = p.elementAt(i + 1);
            const QPainterPath::Element end = p.elementAt(i + 2);
            subs.last().append(Row{3, QPointF(end.x, end.y), QPointF(e.x, e.y),
                                   QPointF(c2.x, c2.y)});
            i += 2;
        }
    }

    QVector<Element> out;
    for (QVector<Row> rows : subs) {
        if (rows.size() < 2)
            continue;
        // Drop zero-extent contours (a moveTo followed by lineTo the same spot).
        bool extent = false;
        for (const Row &r : rows) {
            extent = extent || !samePoint(r.pt, rows.first().pt, 1e-9)
                     || (r.type == 3 && (!samePoint(r.c1, r.pt, 1e-9) || !samePoint(r.c2, r.pt, 1e-9)));
        }
        if (!extent)
            continue;

        const bool closed = samePoint(rows.first().pt, rows.last().pt);
        if (closed) {
            rows.last().pt = rows.first().pt;   // exact, not just fuzzy
            const QPointF s = rows.first().pt;
            rows.append(Row{4, s, rows.last().pt, s});
        }

        QJsonArray points, cp1, cp2, ptype, smooth;
        const int n = rows.size();
        for (int i = 0; i < n; ++i) {
            const Row &r = rows.at(i);
            points.append(xy(r.pt));
            cp1.append(xy(r.type == 0 ? r.pt : r.c1));
            cp2.append(xy(r.type == 0 ? r.pt : r.c2));
            ptype.append(r.type);
            smooth.append(closed && i == n - 1 ? 1 : 0);
        }
        QJsonObject o;
        o.insert("behavior", 0);
        o.insert("geometryType", QStringLiteral("path"));
        o.insert("group_id", QJsonArray());
        o.insert("id", QUuid::createUuid().toString());
        o.insert("layer", layer);
        o.insert("position", xy(QPointF(0, 0)));
        o.insert("tabs", QJsonArray());
        o.insert("points", points);
        o.insert("cp1", cp1);
        o.insert("cp2", cp2);
        o.insert("point_type", ptype);
        o.insert("smooth", smooth);
        out.append(Element::fromJson(o));
    }
    return out;
}

// Elliptical arc E(t) = C + R(rot) * (rx cos t, ry sin t), t from startAngle
// over `sweep`, as cubic beziers of at most 90 degrees each (max radial error
// ~2.7e-4 * r).
void appendArc(QPainterPath &path, QPointF c, double rx, double ry, double rot,
               double a0, double sweep, bool startNew)
{
    const double cr = std::cos(rot), sr = std::sin(rot);
    auto E = [&](double t) {
        const double x = rx * std::cos(t), y = ry * std::sin(t);
        return QPointF(c.x() + cr * x - sr * y, c.y() + sr * x + cr * y);
    };
    auto dE = [&](double t) {
        const double x = -rx * std::sin(t), y = ry * std::cos(t);
        return QPointF(cr * x - sr * y, sr * x + cr * y);
    };
    if (startNew || path.isEmpty())
        path.moveTo(E(a0));
    if (std::fabs(sweep) < 1e-12)
        return;
    const int n = qMax(1, int(std::ceil(std::fabs(sweep) / (M_PI / 2) - 1e-9)));
    const double dt = sweep / n;
    const double k = 4.0 / 3.0 * std::tan(dt / 4.0);
    const bool fullTurn = std::fabs(std::fabs(sweep) - 2 * M_PI) < 1e-9;
    for (int i = 0; i < n; ++i) {
        const double t1 = a0 + i * dt, t2 = t1 + dt;
        const QPointF p1 = E(t1);
        // A full turn lands exactly on its start so closeSubpath() sees a
        // closed contour instead of adding a 1e-15 mm line.
        const QPointF p2 = (fullTurn && i == n - 1) ? E(a0) : E(t2);
        path.cubicTo(p1 + k * dE(t1), p2 - k * dE(t2), p2);
    }
}

static void appendCircle(QPainterPath &path, QPointF c, double r)
{
    appendArc(path, c, r, r, 0, 0, 2 * M_PI, true);
    path.closeSubpath();
}

// Bounding box, placement and element creation shared by both readers.
static void finish(ImportResult &res, QPainterPath mm, const ImportOptions &opt)
{
    if (mm.isEmpty()) {
        res.ok = true;
        return;
    }
    QRectF bb = mm.boundingRect();
    const double eps = 1e-6;
    const bool fits = opt.stockWidth > 0 && opt.stockHeight > 0
                      && bb.left() >= -eps && bb.top() >= -eps
                      && bb.right() <= opt.stockWidth + eps
                      && bb.bottom() <= opt.stockHeight + eps;
    if (opt.autoPlace && !fits) {
        QTransform t;
        t.translate(opt.margin - bb.left(), opt.margin - bb.top());
        mm = t.map(mm);
        bb = mm.boundingRect();
        res.relocated = true;
    }
    res.elements = elementsFromPath(mm, opt.layer);
    res.bounds = bb;
    res.ok = true;
}

static void addSkipped(ImportResult &res, QHash<QString, int> &counts, const QString &what)
{
    ++res.skipped;
    ++counts[what];
}

static void flushSkipped(ImportResult &res, const QHash<QString, int> &counts)
{
    QStringList keys = counts.keys();
    std::sort(keys.begin(), keys.end());
    for (const QString &k : keys)
        res.notes.append(QStringLiteral("%1 x %2 ignored").arg(counts.value(k)).arg(k));
}

// ---------------------------------------------------------------------------
// SVG
// ---------------------------------------------------------------------------

namespace {

// Scanner over SVG number lists / path data.
struct Scanner {
    QStringView s;
    int i = 0;
    void skipWs() { while (i < s.size() && (s.at(i).isSpace() || s.at(i) == QLatin1Char(','))) ++i; }
    bool atEnd() { skipWs(); return i >= s.size(); }
    bool peekNumber() {
        skipWs();
        if (i >= s.size()) return false;
        const QChar c = s.at(i);
        return c.isDigit() || c == QLatin1Char('-') || c == QLatin1Char('+') || c == QLatin1Char('.');
    }
    bool number(double *out) {
        skipWs();
        const int start = i;
        if (i < s.size() && (s.at(i) == QLatin1Char('-') || s.at(i) == QLatin1Char('+'))) ++i;
        int digits = 0;
        while (i < s.size() && s.at(i).isDigit()) { ++i; ++digits; }
        if (i < s.size() && s.at(i) == QLatin1Char('.')) {
            ++i;
            while (i < s.size() && s.at(i).isDigit()) { ++i; ++digits; }
        }
        if (digits == 0) { i = start; return false; }
        if (i < s.size() && (s.at(i) == QLatin1Char('e') || s.at(i) == QLatin1Char('E'))) {
            int j = i + 1;
            if (j < s.size() && (s.at(j) == QLatin1Char('-') || s.at(j) == QLatin1Char('+'))) ++j;
            if (j < s.size() && s.at(j).isDigit()) {
                while (j < s.size() && s.at(j).isDigit()) ++j;
                i = j;
            }
        }
        bool ok = false;
        *out = s.mid(start, i - start).toDouble(&ok);
        return ok;
    }
    // Arc flags are single characters and may be packed ("0 01 50,0").
    bool flag(bool *out) {
        skipWs();
        if (i >= s.size()) return false;
        const QChar c = s.at(i);
        if (c != QLatin1Char('0') && c != QLatin1Char('1')) return false;
        *out = c == QLatin1Char('1');
        ++i;
        return true;
    }
};

// SVG endpoint arc -> center parameterization (spec appendix F.6.5) -> beziers.
void svgArcTo(QPainterPath &path, QPointF p1, double rx, double ry, double phiDeg,
              bool largeArc, bool sweepFlag, QPointF p2)
{
    if (samePoint(p1, p2))
        return;
    rx = std::fabs(rx); ry = std::fabs(ry);
    if (rx < 1e-12 || ry < 1e-12) {
        path.lineTo(p2);
        return;
    }
    const double phi = qDegreesToRadians(phiDeg);
    const double cp = std::cos(phi), sp = std::sin(phi);
    const double dx = (p1.x() - p2.x()) / 2, dy = (p1.y() - p2.y()) / 2;
    const double x1p = cp * dx + sp * dy, y1p = -sp * dx + cp * dy;
    const double lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lambda > 1) { rx *= std::sqrt(lambda); ry *= std::sqrt(lambda); }
    const double num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    const double den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    double coef = den > 0 ? std::sqrt(qMax(0.0, num / den)) : 0;
    if (largeArc == sweepFlag) coef = -coef;
    const double cxp = coef * rx * y1p / ry, cyp = -coef * ry * x1p / rx;
    const QPointF c(cp * cxp - sp * cyp + (p1.x() + p2.x()) / 2,
                    sp * cxp + cp * cyp + (p1.y() + p2.y()) / 2);
    auto ang = [](double ux, double uy, double vx, double vy) {
        return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    };
    const double ux = (x1p - cxp) / rx, uy = (y1p - cyp) / ry;
    const double vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry;
    const double theta1 = ang(1, 0, ux, uy);
    double dtheta = ang(ux, uy, vx, vy);
    if (!sweepFlag && dtheta > 0) dtheta -= 2 * M_PI;
    if (sweepFlag && dtheta < 0) dtheta += 2 * M_PI;
    appendArc(path, c, rx, ry, phi, theta1, dtheta, false);
    // Snap the end onto the exact endpoint so closes detect cleanly.
    if (path.elementCount() > 0) {
        const auto e = path.elementAt(path.elementCount() - 1);
        if (!samePoint(QPointF(e.x, e.y), p2, 1e-9))
            path.setElementPositionAt(path.elementCount() - 1, p2.x(), p2.y());
    }
}

} // namespace

QPainterPath parseSvgPathData(const QString &d, bool *okOut)
{
    QPainterPath path;
    Scanner sc{QStringView(d)};
    QPointF cur, start, lastC2, lastQ;
    QChar prevCmd;
    QChar cmd;
    bool ok = true;

    auto rel = [&](QChar c) { return c.isLower(); };

    while (!sc.atEnd()) {
        sc.skipWs();
        const QChar ch = sc.s.at(sc.i);
        if (ch.isLetter()) {
            cmd = ch;
            ++sc.i;
        } else if (cmd.isNull() || cmd.toUpper() == QLatin1Char('Z')) {
            ok = false;   // numbers without a command letter
            break;
        } else if (cmd == QLatin1Char('M')) {
            cmd = QLatin1Char('L');   // implicit lineto after moveto
        } else if (cmd == QLatin1Char('m')) {
            cmd = QLatin1Char('l');
        }
        const QChar u = cmd.toUpper();
        const bool r = rel(cmd);
        QPointF ctrlOut;      // last cubic cp2 / quadratic cp for this command
        bool cubic = false, quad = false;

        if (u == QLatin1Char('Z')) {
            path.closeSubpath();
            cur = start;
            prevCmd = cmd;
            continue;
        }

        double a[7];
        auto need = [&](int n) {
            for (int k = 0; k < n; ++k)
                if (!sc.number(&a[k])) return false;
            return true;
        };

        if (u == QLatin1Char('M')) {
            if (!need(2)) { ok = false; break; }
            QPointF p(a[0], a[1]);
            if (r) p += cur;
            path.moveTo(p);
            cur = start = p;
        } else if (u == QLatin1Char('L')) {
            if (!need(2)) { ok = false; break; }
            QPointF p(a[0], a[1]);
            if (r) p += cur;
            path.lineTo(p);
            cur = p;
        } else if (u == QLatin1Char('H')) {
            if (!need(1)) { ok = false; break; }
            QPointF p(r ? cur.x() + a[0] : a[0], cur.y());
            path.lineTo(p);
            cur = p;
        } else if (u == QLatin1Char('V')) {
            if (!need(1)) { ok = false; break; }
            QPointF p(cur.x(), r ? cur.y() + a[0] : a[0]);
            path.lineTo(p);
            cur = p;
        } else if (u == QLatin1Char('C')) {
            if (!need(6)) { ok = false; break; }
            QPointF c1(a[0], a[1]), c2(a[2], a[3]), p(a[4], a[5]);
            if (r) { c1 += cur; c2 += cur; p += cur; }
            path.cubicTo(c1, c2, p);
            cur = p; ctrlOut = c2; cubic = true;
        } else if (u == QLatin1Char('S')) {
            if (!need(4)) { ok = false; break; }
            QPointF c2(a[0], a[1]), p(a[2], a[3]);
            if (r) { c2 += cur; p += cur; }
            const bool prevCubic = prevCmd.toUpper() == QLatin1Char('C')
                                   || prevCmd.toUpper() == QLatin1Char('S');
            const QPointF c1 = prevCubic ? 2 * cur - lastC2 : cur;
            path.cubicTo(c1, c2, p);
            cur = p; ctrlOut = c2; cubic = true;
        } else if (u == QLatin1Char('Q')) {
            if (!need(4)) { ok = false; break; }
            QPointF q(a[0], a[1]), p(a[2], a[3]);
            if (r) { q += cur; p += cur; }
            path.quadTo(q, p);
            cur = p; ctrlOut = q; quad = true;
        } else if (u == QLatin1Char('T')) {
            if (!need(2)) { ok = false; break; }
            QPointF p(a[0], a[1]);
            if (r) p += cur;
            const bool prevQuad = prevCmd.toUpper() == QLatin1Char('Q')
                                  || prevCmd.toUpper() == QLatin1Char('T');
            const QPointF q = prevQuad ? 2 * cur - lastQ : cur;
            path.quadTo(q, p);
            cur = p; ctrlOut = q; quad = true;
        } else if (u == QLatin1Char('A')) {
            bool large = false, sweep = false;
            if (!need(3) || !sc.flag(&large) || !sc.flag(&sweep)) { ok = false; break; }
            const double rx = a[0], ry = a[1], rot = a[2];
            if (!need(2)) { ok = false; break; }   // refills a[0..1]
            QPointF p(a[0], a[1]);
            if (r) p += cur;
            svgArcTo(path, cur, rx, ry, rot, large, sweep, p);
            cur = p;
        } else {
            ok = false;   // unknown command letter
            break;
        }
        if (cubic) lastC2 = ctrlOut;
        if (quad) lastQ = ctrlOut;
        prevCmd = cmd;
    }
    if (okOut) *okOut = ok;
    return path;
}

namespace {

// "translate(10 20) rotate(45) scale(2)" -> QTransform, SVG order (first
// listed is applied last to the point), i.e. M = M_n * ... * M_1 in Qt's
// row-vector convention.
QTransform parseSvgTransform(const QString &text)
{
    QTransform m;
    static const QRegularExpression re(
        QStringLiteral("([a-zA-Z]+)\\s*\\(([^)]*)\\)"));
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        const QString fn = match.captured(1).toLower();
        Scanner sc{QStringView(match.capturedView(2))};
        QVector<double> v;
        double d;
        while (sc.number(&d)) v.append(d);
        QTransform t;
        if (fn == QLatin1String("translate") && v.size() >= 1) {
            t.translate(v.at(0), v.size() > 1 ? v.at(1) : 0);
        } else if (fn == QLatin1String("scale") && v.size() >= 1) {
            t.scale(v.at(0), v.size() > 1 ? v.at(1) : v.at(0));
        } else if (fn == QLatin1String("rotate") && v.size() >= 1) {
            if (v.size() >= 3) {
                t.translate(v.at(1), v.at(2));
                t.rotate(v.at(0));
                t.translate(-v.at(1), -v.at(2));
            } else {
                t.rotate(v.at(0));
            }
        } else if (fn == QLatin1String("skewx") && v.size() >= 1) {
            t = QTransform(1, 0, std::tan(qDegreesToRadians(v.at(0))), 1, 0, 0);
        } else if (fn == QLatin1String("skewy") && v.size() >= 1) {
            t = QTransform(1, std::tan(qDegreesToRadians(v.at(0))), 0, 1, 0, 0);
        } else if (fn == QLatin1String("matrix") && v.size() >= 6) {
            t = QTransform(v.at(0), v.at(1), v.at(2), v.at(3), v.at(4), v.at(5));
        } else {
            continue;
        }
        m = t * m;
    }
    return m;
}

// CSS length -> millimetres. Unitless / px are CSS pixels at 96 dpi.
bool svgLengthMm(const QString &text, double *mm)
{
    const QString s = text.trimmed();
    if (s.isEmpty()) return false;
    Scanner sc{QStringView(s)};
    double v;
    if (!sc.number(&v)) return false;
    const QString unit = s.mid(sc.i).trimmed().toLower();
    double f;
    if (unit.isEmpty() || unit == QLatin1String("px")) f = 25.4 / 96;
    else if (unit == QLatin1String("mm")) f = 1;
    else if (unit == QLatin1String("cm")) f = 10;
    else if (unit == QLatin1String("in")) f = 25.4;
    else if (unit == QLatin1String("pt")) f = 25.4 / 72;
    else if (unit == QLatin1String("pc")) f = 25.4 / 6;
    else return false;   // %, em, ... : unusable for a physical size
    *mm = v * f;
    return true;
}

bool svgHidden(const QXmlStreamAttributes &at)
{
    if (at.value(QLatin1String("display")).trimmed() == QLatin1String("none"))
        return true;
    const QString style = at.value(QLatin1String("style")).toString();
    static const QRegularExpression re(QStringLiteral("display\\s*:\\s*none"));
    return re.match(style).hasMatch();
}

double attrD(const QXmlStreamAttributes &at, const char *name, double def = 0)
{
    const QStringView v = at.value(QLatin1String(name));
    if (v.isEmpty()) return def;
    Scanner sc{v};
    double d;
    return sc.number(&d) ? d : def;
}

QPainterPath svgShape(QStringView name, const QXmlStreamAttributes &at, bool *known,
                      bool *malformed)
{
    QPainterPath p;
    *known = true;
    *malformed = false;
    if (name == QLatin1String("path")) {
        bool ok = true;
        p = parseSvgPathData(at.value(QLatin1String("d")).toString(), &ok);
        *malformed = !ok;
    } else if (name == QLatin1String("rect")) {
        const double x = attrD(at, "x"), y = attrD(at, "y");
        const double w = attrD(at, "width"), h = attrD(at, "height");
        double rx = attrD(at, "rx", -1), ry = attrD(at, "ry", -1);
        if (w <= 0 || h <= 0) return p;
        if (rx < 0 && ry < 0) { rx = ry = 0; }
        else if (rx < 0) rx = ry;
        else if (ry < 0) ry = rx;
        rx = qMin(rx, w / 2); ry = qMin(ry, h / 2);
        if (rx > 0 && ry > 0) {
            // Corners as elliptical arcs (Y-down: start after the top-left corner, go clockwise).
            p.moveTo(x + rx, y);
            p.lineTo(x + w - rx, y);
            appendArc(p, QPointF(x + w - rx, y + ry), rx, ry, 0, -M_PI / 2, M_PI / 2, false);
            p.lineTo(x + w, y + h - ry);
            appendArc(p, QPointF(x + w - rx, y + h - ry), rx, ry, 0, 0, M_PI / 2, false);
            p.lineTo(x + rx, y + h);
            appendArc(p, QPointF(x + rx, y + h - ry), rx, ry, 0, M_PI / 2, M_PI / 2, false);
            p.lineTo(x, y + ry);
            appendArc(p, QPointF(x + rx, y + ry), rx, ry, 0, M_PI, M_PI / 2, false);
            p.closeSubpath();
        } else {
            p.moveTo(x, y); p.lineTo(x + w, y); p.lineTo(x + w, y + h); p.lineTo(x, y + h);
            p.closeSubpath();
        }
    } else if (name == QLatin1String("circle")) {
        const double r = attrD(at, "r");
        if (r > 0) appendCircle(p, QPointF(attrD(at, "cx"), attrD(at, "cy")), r);
    } else if (name == QLatin1String("ellipse")) {
        const double rx = attrD(at, "rx"), ry = attrD(at, "ry");
        if (rx > 0 && ry > 0) {
            appendArc(p, QPointF(attrD(at, "cx"), attrD(at, "cy")), rx, ry, 0, 0, 2 * M_PI, true);
            p.closeSubpath();
        }
    } else if (name == QLatin1String("line")) {
        p.moveTo(attrD(at, "x1"), attrD(at, "y1"));
        p.lineTo(attrD(at, "x2"), attrD(at, "y2"));
    } else if (name == QLatin1String("polyline") || name == QLatin1String("polygon")) {
        Scanner sc{at.value(QLatin1String("points"))};
        double x, y;
        bool first = true;
        while (sc.number(&x) && sc.number(&y)) {
            if (first) p.moveTo(x, y); else p.lineTo(x, y);
            first = false;
        }
        if (name == QLatin1String("polygon") && !first)
            p.closeSubpath();
    } else {
        *known = false;
    }
    return p;
}

} // namespace

ImportResult importSvgData(const QByteArray &data, const ImportOptions &opt,
                           const QString &sourceName)
{
    ImportResult res;
    QXmlStreamReader xml(data);
    QHash<QString, int> skippedCounts;
    QPainterPath user;        // Y-down user units, all shapes mapped by their CTM
    struct Ctx { QTransform xf; bool hidden; };
    QVector<Ctx> stack;
    bool rootSeen = false;
    double vx = 0, vy = 0, vw = 0, vh = 0;   // viewBox
    double wMm = -1, hMm = -1;
    bool aspectNone = false;
    int malformed = 0;

    static const QStringList skipContainers = {
        QStringLiteral("defs"), QStringLiteral("symbol"), QStringLiteral("clipPath"),
        QStringLiteral("mask"), QStringLiteral("pattern"), QStringLiteral("marker"),
        QStringLiteral("metadata"), QStringLiteral("title"), QStringLiteral("desc"),
        QStringLiteral("style"), QStringLiteral("linearGradient"),
        QStringLiteral("radialGradient"), QStringLiteral("filter"), QStringLiteral("script"),
        QStringLiteral("foreignObject"), QStringLiteral("switch")};
    static const QStringList reportedSkips = {
        QStringLiteral("text"), QStringLiteral("image"), QStringLiteral("use"),
        QStringLiteral("linearGradient"), QStringLiteral("radialGradient")};

    while (!xml.atEnd()) {
        const auto tok = xml.readNext();
        if (tok == QXmlStreamReader::StartElement) {
            const QString name = xml.name().toString();
            const QXmlStreamAttributes at = xml.attributes();

            if (!rootSeen) {
                if (name != QLatin1String("svg")) {
                    res.error = QStringLiteral("%1: not an SVG document (root <%2>)")
                                    .arg(sourceName, name);
                    return res;
                }
                rootSeen = true;
                Scanner sc{at.value(QLatin1String("viewBox"))};
                double a[4];
                bool vb = true;
                for (double &x : a) vb = vb && sc.number(&x);
                if (vb && a[2] > 0 && a[3] > 0) { vx = a[0]; vy = a[1]; vw = a[2]; vh = a[3]; }
                svgLengthMm(at.value(QLatin1String("width")).toString(), &wMm);
                svgLengthMm(at.value(QLatin1String("height")).toString(), &hMm);
                aspectNone = at.value(QLatin1String("preserveAspectRatio")).trimmed()
                                 .startsWith(QLatin1String("none"));
                stack.append(Ctx{QTransform(), false});
                continue;
            }

            if (reportedSkips.contains(name)) {
                addSkipped(res, skippedCounts, name);
                xml.skipCurrentElement();   // consumes through the matching end tag
                continue;
            }

            // Definition containers are walked (so gradients/text inside them
            // are still counted) but nothing in them is drawn.
            const Ctx &parent = stack.last();
            Ctx ctx{parseSvgTransform(at.value(QLatin1String("transform")).toString()) * parent.xf,
                    parent.hidden || svgHidden(at) || skipContainers.contains(name)};
            stack.append(ctx);
            if (ctx.hidden)
                continue;

            bool known = false, bad = false;
            const QPainterPath local = svgShape(xml.name(), at, &known, &bad);
            if (known) {
                if (bad) ++malformed;
                if (!local.isEmpty())
                    user.addPath(ctx.xf.map(local));
            } else if (name == QLatin1String("g") || name == QLatin1String("svg")
                       || name == QLatin1String("a")) {
                // containers: nothing to draw
            } else {
                addSkipped(res, skippedCounts, name);
            }
        } else if (tok == QXmlStreamReader::EndElement) {
            if (!stack.isEmpty())
                stack.removeLast();
        }
    }
    if (xml.hasError()) {
        res.error = QStringLiteral("%1: XML error at line %2: %3")
                        .arg(sourceName).arg(xml.lineNumber()).arg(xml.errorString());
        return res;
    }
    if (!rootSeen) {
        res.error = QStringLiteral("%1: no <svg> root element").arg(sourceName);
        return res;
    }

    // User units -> mm. No viewBox: the user unit is a CSS px and the page is
    // width x height. No width/height: the viewBox is measured in px.
    const double pxMm = 25.4 / 96;
    if (vw <= 0 || vh <= 0) {
        vx = vy = 0;
        if (wMm > 0 && hMm > 0) { vw = wMm / pxMm; vh = hMm / pxMm; }
        else {
            const QRectF ub = user.boundingRect();
            vw = qMax(1.0, ub.right()); vh = qMax(1.0, ub.bottom());
        }
    }
    if (wMm <= 0 || hMm <= 0) {
        if (wMm > 0)      { hMm = wMm * vh / vw; }
        else if (hMm > 0) { wMm = hMm * vw / vh; }
        else              { wMm = vw * pxMm; hMm = vh * pxMm; }
    }
    double sx = wMm / vw, sy = hMm / vh, tx = 0, ty = 0;
    if (!aspectNone) {   // xMidYMid meet
        const double s = qMin(sx, sy);
        tx = (wMm - vw * s) / 2; ty = (hMm - vh * s) / 2;
        sx = sy = s;
    }
    QTransform toMm;
    toMm.translate(-vx, -vy);
    toMm = toMm * QTransform::fromScale(sx, sy) * QTransform::fromTranslate(tx, ty);
    toMm = toMm * QTransform(1, 0, 0, -1, 0, hMm);   // Y-down page -> Y-up CC space
    const QPainterPath mm = toMm.map(user);

    if (malformed > 0)
        res.notes.append(QStringLiteral("%1 path(s) had malformed data (partially imported)")
                             .arg(malformed));
    flushSkipped(res, skippedCounts);
    finish(res, mm, opt);
    return res;
}

// ---------------------------------------------------------------------------
// DXF
// ---------------------------------------------------------------------------

namespace {

struct Pair { int code; QString value; };
struct Entity { QString type; QVector<Pair> pairs; QVector<QVector<Pair>> vertices; };
struct Block { QPointF base; QVector<Entity> entities; };

struct DxfCtx {
    ImportResult *res;
    QHash<QString, int> *skipped;
    QHash<QString, Block> blocks;
    double unit = 1;        // mm per drawing unit
    double tol = 0.02;
    QPainterPath out;       // mm, Y-up
};

double pairD(const QVector<Pair> &ps, int code, double def = 0)
{
    for (const Pair &p : ps)
        if (p.code == code) return p.value.toDouble();
    return def;
}
int pairI(const QVector<Pair> &ps, int code, int def = 0)
{
    for (const Pair &p : ps)
        if (p.code == code) return p.value.toInt();
    return def;
}
QString pairS(const QVector<Pair> &ps, int code)
{
    for (const Pair &p : ps)
        if (p.code == code) return p.value;
    return QString();
}
QVector<double> pairAll(const QVector<Pair> &ps, int code)
{
    QVector<double> v;
    for (const Pair &p : ps)
        if (p.code == code) v.append(p.value.toDouble());
    return v;
}

// Object coordinate system: only the mirrored case (extrusion 0,0,-1) is
// common in 2D files; it flips X.
QTransform ocs(const QVector<Pair> &ps)
{
    return pairD(ps, 230, 1) < 0 ? QTransform(-1, 0, 0, 1, 0, 0) : QTransform();
}

double insUnitsToMm(int code)
{
    switch (code) {
    case 1: return 25.4;            // inches
    case 2: return 304.8;           // feet
    case 3: return 1609344.0;       // miles
    case 4: return 1.0;             // mm
    case 5: return 10.0;            // cm
    case 6: return 1000.0;          // m
    case 7: return 1e6;             // km
    case 8: return 25.4e-6;         // microinches
    case 9: return 0.0254;          // mils
    case 10: return 914.4;          // yards
    case 11: return 1e-7;           // angstroms
    case 12: return 1e-6;           // nanometers
    case 13: return 1e-3;           // microns
    case 14: return 100.0;          // decimeters
    case 15: return 10000.0;        // decameters
    case 16: return 100000.0;       // hectometers
    default: return 1.0;            // 0 = unitless -> mm
    }
}

// Polyline vertices with bulge arcs: bulge = tan(theta/4), positive = CCW.
void polylineToPath(QPainterPath &p, const QVector<QPointF> &pts, const QVector<double> &bulge,
                    bool closed)
{
    if (pts.size() < 2)
        return;
    p.moveTo(pts.first());
    const int n = pts.size();
    const int segs = closed ? n : n - 1;
    for (int i = 0; i < segs; ++i) {
        const QPointF a = pts.at(i), b = pts.at((i + 1) % n);
        const double bl = bulge.value(i, 0);
        if (std::fabs(bl) < 1e-12 || samePoint(a, b)) {
            p.lineTo(b);
            continue;
        }
        const QPointF ch = b - a;
        const double d = std::hypot(ch.x(), ch.y());
        const QPointF mid = (a + b) / 2;
        const QPointF left(-ch.y() / d, ch.x() / d);
        const QPointF c = mid + left * (d / 2) * ((1.0 / bl) - bl) / 2;
        const double r = (d / 2) * (1 + bl * bl) / (2 * std::fabs(bl));
        const double a0 = std::atan2(a.y() - c.y(), a.x() - c.x());
        const double sweep = 4 * std::atan(bl);
        appendArc(p, c, r, r, 0, a0, sweep, false);
        // land exactly on the vertex
        p.setElementPositionAt(p.elementCount() - 1, b.x(), b.y());
    }
    if (closed)
        p.closeSubpath();
}

// NURBS point by de Boor (homogeneous coordinates).
struct Nurbs {
    int degree = 3;
    QVector<double> knots, weights;
    QVector<QPointF> ctrl;
    int findSpan(double t) const {
        const int n = ctrl.size() - 1;
        if (t >= knots.at(n + 1)) return n;
        if (t <= knots.at(degree)) return degree;
        int lo = degree, hi = n + 1;
        int mid = (lo + hi) / 2;
        while (t < knots.at(mid) || t >= knots.at(mid + 1)) {
            if (t < knots.at(mid)) hi = mid; else lo = mid;
            mid = (lo + hi) / 2;
        }
        return mid;
    }
    QPointF eval(double t) const {
        const int p = degree;
        const int k = findSpan(t);
        QVector<double> x(p + 1), y(p + 1), w(p + 1);
        for (int j = 0; j <= p; ++j) {
            const int idx = k - p + j;
            const double wj = weights.value(idx, 1.0);
            x[j] = ctrl.at(idx).x() * wj; y[j] = ctrl.at(idx).y() * wj; w[j] = wj;
        }
        for (int r = 1; r <= p; ++r) {
            for (int j = p; j >= r; --j) {
                const int i = k - p + j;
                const double den = knots.at(i + p - r + 1) - knots.at(i);
                const double alpha = den > 0 ? (t - knots.at(i)) / den : 0;
                x[j] = (1 - alpha) * x[j - 1] + alpha * x[j];
                y[j] = (1 - alpha) * y[j - 1] + alpha * y[j];
                w[j] = (1 - alpha) * w[j - 1] + alpha * w[j];
            }
        }
        return w[p] != 0 ? QPointF(x[p] / w[p], y[p] / w[p]) : QPointF(x[p], y[p]);
    }
};

// Flatten f(t) over [t0,t1] into `p` (which already has its start point) with
// chord deviation below `tol`, measured in output (mm) space.
void flattenParam(QPainterPath &p, const std::function<QPointF(double)> &f, double t0,
                  double t1, double tol, int depth = 0)
{
    const QPointF a = f(t0), b = f(t1);
    auto dev = [&](double t) {
        const QPointF m = f(t);
        const QPointF ab = b - a;
        const double len2 = ab.x() * ab.x() + ab.y() * ab.y();
        if (len2 < 1e-18) {
            const QPointF d = m - a;
            return std::hypot(d.x(), d.y());
        }
        const double cross = ab.x() * (m.y() - a.y()) - ab.y() * (m.x() - a.x());
        return std::fabs(cross) / std::sqrt(len2);
    };
    const double tm = (t0 + t1) / 2;
    if (depth >= 14 || (dev(tm) <= tol && dev((t0 + tm) / 2) <= tol && dev((tm + t1) / 2) <= tol)) {
        p.lineTo(b);
        return;
    }
    flattenParam(p, f, t0, tm, tol, depth + 1);
    flattenParam(p, f, tm, t1, tol, depth + 1);
}

void splineToPath(QPainterPath &out, const Entity &e, const QTransform &xf, double tol)
{
    Nurbs n;
    n.degree = qMax(1, pairI(e.pairs, 71, 3));
    n.knots = pairAll(e.pairs, 40);
    n.weights = pairAll(e.pairs, 41);
    const QVector<double> cx = pairAll(e.pairs, 10), cy = pairAll(e.pairs, 20);
    const QVector<double> fx = pairAll(e.pairs, 11), fy = pairAll(e.pairs, 21);
    for (int i = 0; i < qMin(cx.size(), cy.size()); ++i)
        n.ctrl.append(QPointF(cx.at(i), cy.at(i)));
    const int flags = pairI(e.pairs, 70, 0);
    const bool closedFlag = flags & 1;

    if (n.ctrl.size() < 2 && fx.size() >= 2) {
        // Fit points only: Catmull-Rom cubics through the points (exact interpolation).
        QVector<QPointF> f;
        for (int i = 0; i < qMin(fx.size(), fy.size()); ++i)
            f.append(xf.map(QPointF(fx.at(i), fy.at(i))));
        QPainterPath p;
        p.moveTo(f.first());
        const int m = f.size();
        for (int i = 0; i + 1 < m; ++i) {
            const QPointF p0 = f.at(qMax(0, i - 1)), p1 = f.at(i), p2 = f.at(i + 1),
                          p3 = f.at(qMin(m - 1, i + 2));
            p.cubicTo(p1 + (p2 - p0) / 6, p2 - (p3 - p1) / 6, p2);
        }
        if (closedFlag) p.closeSubpath();
        out.addPath(p);
        return;
    }
    if (n.ctrl.size() < 2)
        return;
    if (n.degree >= n.ctrl.size())
        n.degree = n.ctrl.size() - 1;
    const int needKnots = n.ctrl.size() + n.degree + 1;
    if (n.knots.size() != needKnots) {
        // Malformed / missing knot vector: assume clamped uniform.
        n.knots.clear();
        const int inner = n.ctrl.size() - n.degree - 1;
        for (int i = 0; i <= n.degree; ++i) n.knots.append(0);
        for (int i = 1; i <= inner; ++i) n.knots.append(double(i) / (inner + 1));
        for (int i = 0; i <= n.degree; ++i) n.knots.append(1);
    }
    const double t0 = n.knots.at(n.degree), t1 = n.knots.at(n.ctrl.size());
    if (!(t1 > t0))
        return;
    auto f = [&](double t) { return xf.map(n.eval(t)); };
    QPainterPath p;
    p.moveTo(f(t0));
    const int pieces = qMax(1, n.ctrl.size() - n.degree);
    for (int i = 0; i < pieces; ++i)
        flattenParam(p, f, t0 + (t1 - t0) * i / pieces, t0 + (t1 - t0) * (i + 1) / pieces, tol);
    if (closedFlag)
        p.closeSubpath();
    out.addPath(p);
}

void emitEntity(DxfCtx &ctx, const Entity &e, const QTransform &xfIn, int depth)
{
    const QVector<Pair> &ps = e.pairs;
    const QTransform xf = ocs(ps) * xfIn;
    const QString &t = e.type;
    QPainterPath p;   // drawing units (before xf)

    if (t == QLatin1String("LINE")) {
        p.moveTo(pairD(ps, 10), pairD(ps, 20));
        p.lineTo(pairD(ps, 11), pairD(ps, 21));
    } else if (t == QLatin1String("LWPOLYLINE")) {
        QVector<QPointF> pts;
        QVector<double> bulge;
        double x = 0;
        bool haveX = false;
        for (const Pair &pr : ps) {
            if (pr.code == 10) { x = pr.value.toDouble(); haveX = true; }
            else if (pr.code == 20 && haveX) { pts.append(QPointF(x, pr.value.toDouble())); bulge.append(0); haveX = false; }
            else if (pr.code == 42 && !bulge.isEmpty()) bulge.last() = pr.value.toDouble();
        }
        polylineToPath(p, pts, bulge, pairI(ps, 70) & 1);
    } else if (t == QLatin1String("POLYLINE")) {
        const int flags = pairI(ps, 70);
        if (flags & (8 | 16 | 64)) {   // 3D polyline / mesh / polyface: not 2D geometry
            addSkipped(*ctx.res, *ctx.skipped, QStringLiteral("POLYLINE (3D/mesh)"));
            return;
        }
        QVector<QPointF> pts;
        QVector<double> bulge;
        for (const QVector<Pair> &v : e.vertices) {
            const int vf = pairI(v, 70);
            if (vf & (16 | 8)) continue;   // spline frame control point / 3D mesh vertex
            pts.append(QPointF(pairD(v, 10), pairD(v, 20)));
            bulge.append(pairD(v, 42));
        }
        polylineToPath(p, pts, bulge, flags & 1);
    } else if (t == QLatin1String("CIRCLE")) {
        const double r = pairD(ps, 40);
        if (r > 0) appendCircle(p, QPointF(pairD(ps, 10), pairD(ps, 20)), r);
    } else if (t == QLatin1String("ARC")) {
        const double r = pairD(ps, 40);
        double a0 = qDegreesToRadians(pairD(ps, 50)), a1 = qDegreesToRadians(pairD(ps, 51));
        while (a1 <= a0 + 1e-12) a1 += 2 * M_PI;
        if (r > 0) appendArc(p, QPointF(pairD(ps, 10), pairD(ps, 20)), r, r, 0, a0, a1 - a0, true);
    } else if (t == QLatin1String("ELLIPSE")) {
        const QPointF c(pairD(ps, 10), pairD(ps, 20));
        const QPointF maj(pairD(ps, 11), pairD(ps, 21));
        const double ratio = pairD(ps, 40, 1);
        const double rx = std::hypot(maj.x(), maj.y());
        const double rot = std::atan2(maj.y(), maj.x());
        double a0 = pairD(ps, 41, 0), a1 = pairD(ps, 42, 2 * M_PI);
        while (a1 <= a0 + 1e-12) a1 += 2 * M_PI;
        if (rx > 0 && ratio > 0) {
            appendArc(p, c, rx, rx * ratio, rot, a0, a1 - a0, true);
            if (std::fabs((a1 - a0) - 2 * M_PI) < 1e-9)
                p.closeSubpath();
        }
    } else if (t == QLatin1String("SPLINE")) {
        splineToPath(ctx.out, e, xf * QTransform::fromScale(ctx.unit, ctx.unit), ctx.tol);
        return;
    } else if (t == QLatin1String("INSERT")) {
        const QString name = pairS(ps, 2);
        if (!ctx.blocks.contains(name) || depth > 8) {
            addSkipped(*ctx.res, *ctx.skipped, QStringLiteral("INSERT (%1)").arg(
                depth > 8 ? QStringLiteral("nested too deep") : QStringLiteral("unknown block ") + name));
            return;
        }
        const Block &b = ctx.blocks.value(name);
        const double sx = pairD(ps, 41, 1), sy = pairD(ps, 42, 1);
        const double rot = pairD(ps, 50, 0);
        const QPointF ins(pairD(ps, 10), pairD(ps, 20));
        const int cols = qMax(1, pairI(ps, 70, 1)), rows = qMax(1, pairI(ps, 71, 1));
        const double colSp = pairD(ps, 44, 0), rowSp = pairD(ps, 45, 0);
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                QTransform local;
                local.translate(-b.base.x(), -b.base.y());
                local = local * QTransform::fromScale(sx, sy);
                QTransform rt; rt.rotate(rot);
                local = local * rt * QTransform::fromTranslate(ins.x() + c * colSp, ins.y() + r * rowSp);
                for (const Entity &sub : b.entities)
                    emitEntity(ctx, sub, local * xf, depth + 1);
            }
        }
        return;
    } else if (t == QLatin1String("POINT") || t == QLatin1String("SEQEND")
               || t == QLatin1String("VERTEX") || t == QLatin1String("VIEWPORT")) {
        return;   // silently ignored
    } else {
        addSkipped(*ctx.res, *ctx.skipped, t);
        return;
    }
    if (!p.isEmpty())
        ctx.out.addPath((xf * QTransform::fromScale(ctx.unit, ctx.unit)).map(p));
}

} // namespace

ImportResult importDxfData(const QByteArray &data, const ImportOptions &opt,
                           const QString &sourceName)
{
    ImportResult res;
    if (data.startsWith("AutoCAD Binary DXF")) {
        res.error = QStringLiteral("%1: binary DXF is not supported (save as ASCII DXF)").arg(sourceName);
        return res;
    }
    // Group code / value pairs. Codes are on odd lines, values on even ones.
    QVector<Pair> pairs;
    {
        const QString text = QString::fromLatin1(data);
        const QStringList lines = text.split(QLatin1Char('\n'));
        for (int i = 0; i + 1 < lines.size(); i += 2) {
            bool ok = false;
            const int code = lines.at(i).trimmed().toInt(&ok);
            if (!ok) {
                res.error = QStringLiteral("%1: bad group code at line %2").arg(sourceName).arg(i + 1);
                return res;
            }
            QString v = lines.at(i + 1);
            if (v.endsWith(QLatin1Char('\r'))) v.chop(1);
            pairs.append(Pair{code, v.trimmed()});
        }
    }
    if (pairs.isEmpty()) {
        res.error = QStringLiteral("%1: empty DXF").arg(sourceName);
        return res;
    }

    QHash<QString, int> skippedCounts;
    DxfCtx ctx;
    ctx.res = &res;
    ctx.skipped = &skippedCounts;
    ctx.tol = opt.tolerance;
    QVector<Entity> entities;

    // Read one entity starting at pairs[i] (a code-0 row); i is left on the
    // next code-0 row. POLYLINE swallows its VERTEX..SEQEND children.
    auto readEntity = [&](int &i) {
        Entity e;
        e.type = pairs.at(i).value.toUpper();
        ++i;
        while (i < pairs.size() && pairs.at(i).code != 0) e.pairs.append(pairs.at(i++));
        if (e.type == QLatin1String("POLYLINE")) {
            while (i < pairs.size() && pairs.at(i).code == 0) {
                const QString sub = pairs.at(i).value.toUpper();
                if (sub == QLatin1String("VERTEX")) {
                    ++i;
                    QVector<Pair> v;
                    while (i < pairs.size() && pairs.at(i).code != 0) v.append(pairs.at(i++));
                    e.vertices.append(v);
                } else if (sub == QLatin1String("SEQEND")) {
                    ++i;
                    while (i < pairs.size() && pairs.at(i).code != 0) ++i;
                    break;
                } else {
                    break;
                }
            }
        }
        return e;
    };

    int i = 0;
    bool sawEntities = false;
    while (i < pairs.size()) {
        const Pair &p = pairs.at(i);
        if (p.code == 0 && p.value == QLatin1String("SECTION")) {
            ++i;
            const QString sec = (i < pairs.size() && pairs.at(i).code == 2) ? pairs.at(i).value.toUpper() : QString();
            ++i;
            if (sec == QLatin1String("HEADER")) {
                while (i < pairs.size() && !(pairs.at(i).code == 0 && pairs.at(i).value == QLatin1String("ENDSEC"))) {
                    if (pairs.at(i).code == 9 && pairs.at(i).value == QLatin1String("$INSUNITS")
                        && i + 1 < pairs.size() && pairs.at(i + 1).code == 70)
                        ctx.unit = insUnitsToMm(pairs.at(i + 1).value.toInt());
                    ++i;
                }
            } else if (sec == QLatin1String("BLOCKS")) {
                while (i < pairs.size() && !(pairs.at(i).code == 0 && pairs.at(i).value == QLatin1String("ENDSEC"))) {
                    if (pairs.at(i).code == 0 && pairs.at(i).value == QLatin1String("BLOCK")) {
                        Entity hdr = readEntity(i);
                        Block b;
                        b.base = QPointF(pairD(hdr.pairs, 10), pairD(hdr.pairs, 20));
                        const QString name = pairS(hdr.pairs, 2);
                        while (i < pairs.size() && pairs.at(i).code == 0
                               && pairs.at(i).value != QLatin1String("ENDBLK")
                               && pairs.at(i).value != QLatin1String("ENDSEC"))
                            b.entities.append(readEntity(i));
                        if (!name.isEmpty()) ctx.blocks.insert(name, b);
                    } else {
                        ++i;
                    }
                }
            } else if (sec == QLatin1String("ENTITIES")) {
                sawEntities = true;
                while (i < pairs.size() && !(pairs.at(i).code == 0 && pairs.at(i).value == QLatin1String("ENDSEC"))) {
                    if (pairs.at(i).code == 0) entities.append(readEntity(i));
                    else ++i;
                }
            }
            // skip to ENDSEC
            while (i < pairs.size() && !(pairs.at(i).code == 0 && pairs.at(i).value == QLatin1String("ENDSEC"))) ++i;
            ++i;
        } else if (p.code == 0 && p.value == QLatin1String("EOF")) {
            break;
        } else if (p.code == 0 && !sawEntities && p.value != QLatin1String("ENDSEC")) {
            // Bare entity stream without SECTION wrappers (some minimal exporters).
            entities.append(readEntity(i));
        } else {
            ++i;
        }
    }

    for (const Entity &e : entities)
        emitEntity(ctx, e, QTransform(), 0);

    flushSkipped(res, skippedCounts);
    if (ctx.unit != 1.0)
        res.notes.append(QStringLiteral("$INSUNITS scale: 1 unit = %1 mm").arg(ctx.unit));
    finish(res, ctx.out, opt);
    return res;
}

// ---------------------------------------------------------------------------
// File dispatch
// ---------------------------------------------------------------------------

bool isImportableFile(const QString &path)
{
    const QString s = QFileInfo(path).suffix().toLower();
    return s == QLatin1String("svg") || s == QLatin1String("dxf");
}

ImportResult importFile(const QString &path, const ImportOptions &opt)
{
    ImportResult res;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        res.error = QStringLiteral("Cannot read %1: %2").arg(path, f.errorString());
        return res;
    }
    const QByteArray data = f.readAll();
    const QString name = QFileInfo(path).fileName();
    const QString s = QFileInfo(path).suffix().toLower();
    if (s == QLatin1String("svg"))
        return importSvgData(data, opt, name);
    if (s == QLatin1String("dxf"))
        return importDxfData(data, opt, name);
    res.error = QStringLiteral("%1: unsupported file type (expected .svg or .dxf)").arg(name);
    return res;
}

} // namespace c2d
