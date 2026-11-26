#include "surfacing.h"

#include <QStringList>
#include <QtMath>
#include <cmath>

namespace c2d {

// Positions from a to b (a < b) at most `step` apart, both ends included:
// the last one is pulled back to b rather than stepping past it.
static QVector<double> stations(double a, double b, double step)
{
    QVector<double> v;
    const int n = qMax(1, int(std::ceil((b - a) / step - 1e-9)));
    for (int k = 0; k < n; ++k)
        v << a + step * k;
    v << b;
    return v;
}

SurfacingPlan planSurfacing(const SurfacingParams &p)
{
    SurfacingPlan s;
    if (!(p.width > 0) || !(p.height > 0))
        s.error = QStringLiteral("The area needs a width and a length.");
    else if (!(p.toolDia > 0))
        s.error = QStringLiteral("The bit needs a diameter.");
    else if (!(p.stepoverPct >= 5 && p.stepoverPct <= 95))
        s.error = QStringLiteral("Stepover must be between 5 and 95 %.");
    else if (!(p.depth > 0) || !(p.stepdown > 0))
        s.error = QStringLiteral("Depth per pass and total depth must be more than 0.");
    else if (!(p.feed > 0) || !(p.plunge > 0) || p.rpm <= 0)
        s.error = QStringLiteral("Feed, plunge and rpm must be more than 0.");
    if (!s.ok())
        return s;

    const double r = p.toolDia / 2.0;
    const double step = p.toolDia * p.stepoverPct / 100.0;
    const double xa = p.x0 - r, xb = p.x0 + p.width + r;
    const double ya = p.y0 - r, yb = p.y0 + p.height + r;
    s.minX = xa; s.maxX = xb; s.minY = ya; s.maxY = yb;
    // Along X: each line runs xa..xb, the lines step across Y.
    const QVector<double> across = p.alongX ? stations(ya, yb, step) : stations(xa, xb, step);
    const double la = p.alongX ? xa : ya, lb = p.alongX ? xb : yb;
    s.lines = across.size();
    s.passes = qMax(1, int(std::ceil(p.depth / p.stepdown - 1e-9)));

    auto pt = [&](double along, double cross, double &x, double &y) {
        x = p.alongX ? along : cross;
        y = p.alongX ? cross : along;
    };
    QVector<Op> &o = s.ops;
    o << Op::comment(QStringLiteral("Surfacing"));
    double x = 0, y = 0;
    pt(la, across.first(), x, y);
    o << Op::rapid(x, y, p.safeZ);
    o << Op::spindle(p.rpm);
    double z = 0;
    for (int k = 1; k <= s.passes; ++k) {
        z = -p.depth * k / s.passes;
        pt(la, across.first(), x, y);
        o << Op::rapid(x, y, p.safeZ);
        o << Op::feedTo(x, y, z, p.plunge);
        bool fwd = true;
        for (int i = 0; i < across.size(); ++i) {
            if (i > 0) {
                pt(fwd ? la : lb, across.at(i), x, y);
                o << Op::feedTo(x, y, z, p.feed);
            }
            pt(fwd ? lb : la, across.at(i), x, y);
            o << Op::feedTo(x, y, z, p.feed);
            fwd = !fwd;
        }
        o << Op::rapid(x, y, p.safeZ);
    }
    if (p.finishClimb) {
        // The wall round the surfaced area has the stock outside it: with
        // the spindle turning clockwise, going round anticlockwise is climb.
        o << Op::rapid(xa, ya, p.safeZ);
        o << Op::feedTo(xa, ya, z, p.plunge);
        o << Op::feedTo(xb, ya, z, p.feed);
        o << Op::feedTo(xb, yb, z, p.feed);
        o << Op::feedTo(xa, yb, z, p.feed);
        o << Op::feedTo(xa, ya, z, p.feed);
        o << Op::rapid(xa, ya, p.safeZ);
    }
    o << Op::spindle(0);
    return s;
}

QString surfacingFitProblem(const SurfacingPlan &plan, double wcoX, double wcoY,
                            double left, double right, double front, double back)
{
    const double x1 = plan.minX + wcoX, x2 = plan.maxX + wcoX;
    const double y1 = plan.minY + wcoY, y2 = plan.maxY + wcoY;
    constexpr double eps = 1e-6;
    QStringList out;
    if (x1 < left - eps)
        out << QStringLiteral("%1 mm past the left end").arg(left - x1, 0, 'f', 1);
    if (x2 > right + eps)
        out << QStringLiteral("%1 mm past the right end").arg(x2 - right, 0, 'f', 1);
    if (y1 < front - eps)
        out << QStringLiteral("%1 mm past the front").arg(front - y1, 0, 'f', 1);
    if (y2 > back + eps)
        out << QStringLiteral("%1 mm past the back").arg(y2 - back, 0, 'f', 1);
    if (out.isEmpty())
        return QString();
    return QStringLiteral("The bit would go %1 of the travel.").arg(out.join(QStringLiteral(", ")));
}

} // namespace c2d
