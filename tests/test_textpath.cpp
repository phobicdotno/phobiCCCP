// Text on path: letters laid along a line sit at their straight positions
// with no turn, round a circle they turn with the tangent, and the result
// is plain closed vectors. Plain asserts, no test framework; exits 0 on success.

#include "../src/element.h"
#include "../src/vectorops.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineF>

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

static bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

// Angle difference folded into (-180, 180].
static double angDiff(double a, double b)
{
    double d = std::fmod(a - b, 360.0);
    if (d > 180) d -= 360;
    if (d <= -180) d += 360;
    return d;
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    const QJsonObject layer;
    const Element text = Element::makeText(QStringLiteral("AB"), QPointF(30, 40), 10.0,
                                           QStringLiteral("Helvetica"), layer);
    double width = 0;
    const QVector<TextGlyph> glyphs = Element::textGlyphs(text.raw, &width);
    check(glyphs.size() == 2, "AB has two glyphs");
    check(width > 5 && width < 20, "AB is a sensible width at 10 mm");

    // ---- straight 100 mm line, start at 0 % ----
    const Element line = Element::makePath({QPointF(0, 0), QPointF(100, 0)}, false, layer);
    vec::TextOnPathOptions opt;
    opt.startPercent = 0;
    QVector<vec::GlyphPlace> places;
    QPainterPath out = vec::textOnPathOutline(text.raw, line.painterPath, opt, &places);
    check(places.size() == 2, "two placements on the line");
    for (int i = 0; i < 2; ++i) {
        check(near(places.at(i).arcPos, glyphs.at(i).cluster, 1e-9), "arc position = straight position");
        check(near(places.at(i).at.x(), glyphs.at(i).cluster, 1e-3), "origin x on the line");
        check(near(places.at(i).at.y(), 0, 1e-3), "origin y on the line");
        check(near(places.at(i).angleDeg, 0, 1e-6), "no rotation on a straight line");
    }
    check(places.at(1).at.x() > places.at(0).at.x() + 2, "B after A");
    check(out.boundingRect().top() > -0.5 && out.boundingRect().top() < 0.5,
          "letters stand on the line");   // Y-up: the box's top() is its lowest y

    // Start 50 %: shifted by 50 mm.
    opt.startPercent = 50;
    vec::textOnPathOutline(text.raw, line.painterPath, opt, &places);
    check(near(places.at(0).at.x(), 50 + glyphs.at(0).cluster, 1e-3), "start 50 % begins at 50 mm");
    // Centred.
    opt.startPercent = -1;
    vec::textOnPathOutline(text.raw, line.painterPath, opt, &places);
    check(near(places.at(0).arcPos, (100 - width) / 2 + glyphs.at(0).cluster, 1e-6), "centred on the line");
    // Offset lifts the baseline.
    opt.offset = 3;
    vec::textOnPathOutline(text.raw, line.painterPath, opt, &places);
    check(near(places.at(0).at.y(), 3, 1e-3), "offset 3 mm lifts the baseline");
    // Flip: runs right to left, upside down, under the line.
    opt.offset = 0;
    opt.flip = true;
    out = vec::textOnPathOutline(text.raw, line.painterPath, opt, &places);
    check(near(std::abs(places.at(0).angleDeg), 180, 1e-6), "flipped text turned 180");
    check(places.at(1).at.x() < places.at(0).at.x(), "flipped text runs the other way");
    check(out.boundingRect().center().y() < 0, "flipped text sits under the line");
    opt.flip = false;

    // ---- circle (closed bezier), radius 50 about the origin ----
    const Element circle = Element::makeCircle(QPointF(0, 0), 50, layer);
    opt.startPercent = 10;
    out = vec::textOnPathOutline(text.raw, circle.painterPath, opt, &places);
    check(places.size() == 2, "two placements on the circle");
    for (int i = 0; i < 2; ++i) {
        const TextGlyph &g = glyphs.at(i);
        // Where the cluster middle went: the start of the left edge plus
        // half the advance along the glyph's own direction.
        const double a = places.at(i).angleDeg * M_PI / 180;
        const QPointF m = places.at(i).at + QPointF(std::cos(a), std::sin(a)) * (g.advance / 2);
        check(near(std::hypot(m.x(), m.y()), 50, 0.01), "glyph middle on the circle");
        // The tangent there is perpendicular to the radius.
        const double radial = std::atan2(m.y(), m.x()) * 180 / M_PI;
        const double turn = angDiff(places.at(i).angleDeg, radial);
        check(near(std::abs(turn), 90, 0.2), "rotation follows the circle tangent");
    }
    check(std::abs(angDiff(places.at(1).angleDeg, places.at(0).angleDeg)) > 1,
          "second letter turned further round the circle");
    // Arc length between the two middles matches the straight spacing.
    {
        const double a0 = places.at(0).arcPos + glyphs.at(0).advance / 2;
        const double a1 = places.at(1).arcPos + glyphs.at(1).advance / 2;
        check(near(a1 - a0, glyphs.at(1).cluster + glyphs.at(1).advance / 2
                                 - glyphs.at(0).cluster - glyphs.at(0).advance / 2, 1e-9),
              "letter spacing kept along the arc");
    }

    // ---- elements: plain closed paths, the first keeps the text id ----
    const QVector<Element> els = vec::textOnPathElements(text, circle, opt);
    check(els.size() >= 3, "A (outline + counter) and B (outline + 2 counters) as paths");
    check(els.first().id == text.id, "first piece keeps the text's id");
    for (const Element &e : els) {
        check(e.geometryType == QLatin1String("path"), "pieces are plain paths");
        check(vec::isClosed(e), "pieces are closed");
    }
    // The pieces land where the outline was laid.
    QRectF box;
    for (const Element &e : els)
        box = box.united(e.painterPath.boundingRect());
    check(QLineF(box.center(), out.boundingRect().center()).length() < 0.1, "pieces where the outline is");
    check(vec::textOnPathElements(circle, line, opt).isEmpty(), "a non-text gives nothing");

    std::printf("textpath: %d checks passed\n", g_checks);
    return 0;
}
