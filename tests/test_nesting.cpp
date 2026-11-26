// Nesting parts on the stock: bottom-left fill with spacing, margin and
// quarter turns. Plain asserts, no test framework; exits 0 on success.

#include "../src/nesting.h"

#include <QPainterPath>
#include <QPainterPathStroker>

#ifdef HAVE_CLIPPER2
#include <clipper2/clipper.h>
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace c2d;

static int g_checks = 0;

static void check(bool cond, const char *what)
{
    ++g_checks;
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

static QPolygonF rect(double x, double y, double w, double h)
{
    return QPolygonF({{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}});
}

// An L: a 40 x 40 corner with 10 mm wide arms.
static QPolygonF ell(double x, double y)
{
    return QPolygonF({{x, y}, {x + 40, y}, {x + 40, y + 10}, {x + 10, y + 10},
                      {x + 10, y + 40}, {x, y + 40}});
}

static QVector<QPolygonF> placedOutlines(const QVector<QPolygonF> &in,
                                         const QVector<nest::Placement> &pl)
{
    QVector<QPolygonF> out;
    for (int i = 0; i < in.size(); ++i)
        if (pl.at(i).placed)
            out.append(pl.at(i).transform.map(in.at(i)));
    return out;
}

static double gapBetween(const QPolygonF &a, const QPolygonF &b)
{
    // Overlap area of the two shapes grown by 0.99 mm (just under 2 mm apart).
#ifndef HAVE_CLIPPER2
    auto grown = [](const QPolygonF &poly) {
        QPainterPath s;
        s.addPolygon(poly);
        s.closeSubpath();
        QPainterPathStroker st;
        st.setWidth(1.98);
        st.setJoinStyle(Qt::MiterJoin);
        return st.createStroke(s).united(s);
    };
    double area = 0;
    for (const QPolygonF &p : grown(a).intersected(grown(b)).toFillPolygons())
        for (int i = 0; i < p.size(); ++i)
            area += (p.at(i).x() * p.at((i + 1) % p.size()).y()
                     - p.at((i + 1) % p.size()).x() * p.at(i).y()) / 2;
    return std::fabs(area);
#else
    Clipper2Lib::PathsD pa{{}}, pb{{}};
    for (const QPointF &p : a) pa[0].emplace_back(p.x(), p.y());
    for (const QPointF &p : b) pb[0].emplace_back(p.x(), p.y());
    pa = Clipper2Lib::InflatePaths(pa, 0.99, Clipper2Lib::JoinType::Miter,
                                   Clipper2Lib::EndType::Polygon);
    pb = Clipper2Lib::InflatePaths(pb, 0.99, Clipper2Lib::JoinType::Miter,
                                   Clipper2Lib::EndType::Polygon);
    return std::fabs(Clipper2Lib::Area(
        Clipper2Lib::Intersect(pa, pb, Clipper2Lib::FillRule::NonZero)));
#endif
}

static double boundsArea(const QVector<QPolygonF> &polys)
{
    QRectF box;
    for (const QPolygonF &p : polys)
        box = box.isNull() ? p.boundingRect() : box.united(p.boundingRect());
    return box.width() * box.height();
}

int main()
{
    const QRectF stock(0, 0, 100, 100);

    // Six 40 x 30 rectangles, all stacked at one spot, 2 mm apart.
    {
        QVector<QPolygonF> parts(6, rect(200, 200, 40, 30));
        nest::Options o;
        o.spacing = 2;
        o.margin = 0;
        const auto pl = nest::nestParts(parts, stock, o);
        int n = 0;
        for (const auto &p : pl) n += p.placed;
        check(n == 6, "all six 40 x 30 rectangles fit on 100 x 100");
        const auto out = placedOutlines(parts, pl);
        for (const QPolygonF &p : out) {
            const QRectF b = p.boundingRect();
            check(b.left() >= -1e-6 && b.top() >= -1e-6 && b.right() <= 100 + 1e-6
                      && b.bottom() <= 100 + 1e-6,
                  "every rectangle lies inside the stock");
            check(std::fabs(b.width() * b.height() - 1200) < 1e-6, "a rectangle keeps its size");
        }
        for (int i = 0; i < out.size(); ++i)
            for (int j = i + 1; j < out.size(); ++j)
                check(gapBetween(out.at(i), out.at(j)) < 1e-6, "rectangles stay 2 mm apart");
    }

    // With a 5 mm margin the parts keep off the edge.
    {
        QVector<QPolygonF> parts(2, rect(0, 0, 20, 20));
        nest::Options o;
        o.spacing = 2;
        o.margin = 5;
        const auto pl = nest::nestParts(parts, stock, o);
        check(pl.at(0).placed && pl.at(1).placed, "two small squares fit");
        for (const QPolygonF &p : placedOutlines(parts, pl))
            check(p.boundingRect().left() >= 5 - 1e-6 && p.boundingRect().top() >= 5 - 1e-6,
                  "parts keep the margin from the stock edge");
    }

    // Work already on the stock is kept clear of.
    {
        QVector<QPolygonF> parts{rect(0, 0, 20, 20)};
        nest::Options o;
        o.spacing = 2;
        o.margin = 0;
        const auto pl = nest::nestParts(parts, stock, o, {rect(0, 0, 50, 50)});
        check(pl.at(0).placed, "a part fits beside the work already there");
        check(gapBetween(pl.at(0).transform.map(parts.at(0)), rect(0, 0, 50, 50)) < 1e-6,
              "the part keeps the spacing from the work already there");
    }

    // A part bigger than the stock does not fit and stays where it is.
    {
        QVector<QPolygonF> parts{rect(0, 0, 150, 20), rect(0, 0, 20, 20)};
        nest::Options o;
        o.spacing = 2;
        const auto pl = nest::nestParts(parts, stock, o);
        check(!pl.at(0).placed, "a part bigger than the stock is reported as not fitting");
        check(pl.at(0).transform.isIdentity(), "the part that does not fit is not moved");
        check(pl.at(1).placed, "the smaller part still goes on");
    }

    // Two L shapes on a narrow strip: turning one lets them hook together.
    {
        QVector<QPolygonF> parts{ell(0, 0), ell(0, 0)};
        const QRectF strip(0, 0, 60, 200);
        nest::Options o;
        o.spacing = 2;
        o.margin = 0;
        o.allowRotation = false;
        const auto fixed = nest::nestParts(parts, strip, o);
        o.allowRotation = true;
        const auto turned = nest::nestParts(parts, strip, o);
        check(fixed.at(0).placed && fixed.at(1).placed && turned.at(0).placed
                  && turned.at(1).placed, "both Ls fit either way");
        const double a0 = boundsArea(placedOutlines(parts, fixed));
        const double a1 = boundsArea(placedOutlines(parts, turned));
        std::printf("L bounds: fixed %.0f, turned %.0f mm2\n", a0, a1);
        check(a1 < a0, "with rotation the Ls pack into a smaller area");
        const auto out = placedOutlines(parts, turned);
        check(gapBetween(out.at(0), out.at(1)) < 1e-6, "turned Ls stay 2 mm apart");
    }

    std::printf("nesting: %d checks passed\n", g_checks);
    return 0;
}
