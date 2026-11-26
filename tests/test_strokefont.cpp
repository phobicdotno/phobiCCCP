// Single-line fonts: a text in "Single line (Roman)" is drawn as open
// strokes (cut on the line, left out by pockets), sized by its height, keeps
// its font name through a save and load, stays open on an arc, and Convert
// to path gives open paths.
#include "../src/c2ddocument.h"
#include "../src/element.h"
#include "../src/strokefont.h"
#include "../src/vectorops.h"
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <cmath>
#include <cstdio>

using namespace c2d;

static int failures = 0;
static void check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++failures;
}

static bool allOpen(const QPainterPath &p, int *count)
{
    *count = 0;
    for (int i = 0; i < p.elementCount(); ++i)
        if (p.elementAt(i).isMoveTo())
            ++*count;
    for (const QPolygonF &poly : p.toSubpathPolygons())
        if (poly.size() > 2 && poly.first() == poly.last())
            return false;
    return *count > 0;
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QTemporaryDir tmp;
    QString err;
    const QString blank = tmp.filePath("a.c2d");
    Document::createBlank(blank, 300, 300, 19, &err);
    Document d;
    d.load(blank, &err);

    // Every printable character but the space has strokes, all on the grid.
    bool glyphsOk = true;
    for (int c = 33; c < 127; ++c)
        for (const QString &fam : strokefont::families()) {
            double adv = 0;
            const QPainterPath g = strokefont::glyph(fam, QChar(c), &adv);
            const QRectF r = g.boundingRect();
            if (g.isEmpty() || adv <= 0 || r.top() < -4 || r.bottom() > 12 || r.left() < -2
                || r.right() > adv + 2) {
                std::printf("  glyph %c (%s) off the grid\n", char(c), qPrintable(fam));
                glyphsOk = false;
            }
        }
    const QString extra = QString::fromUtf8("ÆØÅæøåÄÖÜäöüéèß");
    for (const QChar c : extra)
        for (const QString &fam : strokefont::families()) {
            double adv = 0;
            const QPainterPath g = strokefont::glyph(fam, c, &adv);
            const QRectF r = g.boundingRect();
            if (g.isEmpty() || adv <= 0 || r.top() < -4 || r.bottom() > 14 || r.right() > adv + 2) {
                std::printf("  glyph U+%04X (%s) missing or off the grid\n", c.unicode(), qPrintable(fam));
                glyphsOk = false;
            }
        }
    check(glyphsOk, "all ASCII and Nordic/German glyphs have strokes inside their cell");

    const Element plain = Element::makeText(QStringLiteral("HI"), {50, 50}, 20,
                                            QStringLiteral("Helvetica"), d.defaultLayer());
    check(!Element::isStrokeText(plain.raw) && vec::isClosed(plain), "outline text stays closed");
    const Element hi = Element::regenText(plain, {{"family", strokefont::romanFamily()}});
    check(Element::isStrokeText(hi.raw), "Roman marks the text as single line");
    check(hi.raw.value("font").toString() == QLatin1String("Single line (Roman)-Regular"),
          "font key names the single-line font");
    int n = 0;
    check(allOpen(hi.painterPath, &n) && n == 4, "HI is four open strokes (H: 3, I: 1)");
    check(!vec::isClosed(hi), "single-line text counts as an open vector");
    const QRectF r = hi.painterPath.boundingRect();
    check(std::abs(r.height() - 20) < 0.01, "cap height = font height");
    check(r.width() > 0.8 * r.height() && r.width() < 1.1 * r.height(),
          "HI width in proportion to its height");
    check(std::abs(r.bottom() - 70) < 0.01 && std::abs(r.top() - 50) < 0.01, "sits on its baseline");
    for (const QJsonValue &c : hi.raw.value("rendered").toArray())
        if (c.toArray().size() < 2)
            check(false, "every stroke has two or more points");

    // An "O" drawn round is one stroke that comes back to its start.
    const Element o = Element::regenText(hi, {{"text", "O"}});
    check(o.painterPath.toSubpathPolygons().size() == 1, "O is one stroke");

    // Norwegian letters are drawn, not left blank: one glyph per letter,
    // each with strokes.
    const Element nor = Element::regenText(hi, {{"text", QString::fromUtf8("BLÅBÆR Ø")}});
    double nbw = 0;
    const QVector<TextGlyph> nbg = Element::textGlyphs(nor.raw, &nbw);
    bool noBlank = nbg.size() == 8;
    for (int i = 0; i < nbg.size(); ++i)
        if (i != 6)
            noBlank = noBlank && !nbg.at(i).path.isEmpty();
    check(noBlank, "BLÅBÆR Ø has no blank letters");
    check(nbg.size() == 8 && nbg.at(2).path.boundingRect().height() > nbg.at(0).path.boundingRect().height(),
          "Å carries its ring above the A");

    // Round trip of the font name through a save and load.
    d.addElement(hi);
    const QString out = tmp.filePath("b.c2d");
    check(d.save(out, &err), "save");
    Document back;
    check(back.load(out, &err), "load");
    const Element *rb = back.elementById(hi.id);
    check(rb && Element::isStrokeText(rb->raw), "font name comes back from the file");
    int nb = 0;
    check(rb && allOpen(rb->painterPath, &nb) && nb == 4, "reloaded text is still open strokes");
    // A file with no outlines stored lays the strokes out again.
    QJsonObject bare = hi.raw;
    bare.remove("rendered");
    int nr = 0;
    check(allOpen(Element::fromJson(bare).painterPath, &nr) && nr == 4, "re-laid out from the font name");

    // On an arc the strokes stay open.
    const Element arc = Element::regenText(hi, {{"arc_enabled", true}, {"arc_radius", 40.0}});
    int na = 0;
    check(allOpen(arc.painterPath, &na) && na == 4, "text on an arc stays open strokes");

    // Italic: same strokes, slanted.
    const Element it = Element::regenText(hi, {{"family", strokefont::italicFamily()}});
    int ni = 0;
    check(Element::isStrokeText(it.raw) && allOpen(it.painterPath, &ni) && ni == 4,
          "Italic is single line too");
    check(it.painterPath.boundingRect().width() > r.width(), "Italic leans");

    // Convert to path: open paths.
    const QVector<Element> paths = Element::toPaths(hi);
    bool open = paths.size() == 4;
    for (const Element &p : paths)
        open = open && !vec::isClosed(p);
    check(open, "Convert to path gives four open paths");
    const QVector<Element> oPaths = Element::toPaths(o);
    check(oPaths.size() == 1 && vec::isClosed(oPaths.first()), "a round stroke converts to a closed path");

    // Back to an outline font: closed again.
    const Element again = Element::regenText(hi, {{"family", QStringLiteral("Helvetica")}});
    check(!Element::isStrokeText(again.raw) && vec::isClosed(again), "back to an outline font");

    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
