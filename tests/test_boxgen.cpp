// Finger-joint box generator: part counts, outer sizes, mating fingers are
// each other's gaps, and the parts fill the box walls exactly once.
#include "../src/boxgen.h"
#include "../src/canvas.h"
#include "../src/c2ddocument.h"
#include "../src/element.h"
#include <QApplication>
#include <QUndoStack>
#include <QRectF>
#include <cmath>
#include <cstdio>
using namespace c2d;
static int fails = 0;
static void check(bool ok, const char *what) { std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++fails; }
static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

static double area(const QVector<QPointF> &p)
{
    double a = 0;
    for (int i = 0; i < p.size(); ++i) {
        const QPointF &u = p[i], &v = p[(i + 1) % p.size()];
        a += u.x() * v.y() - v.x() * u.y();
    }
    return a / 2;
}
static QRectF bounds(const QVector<QPointF> &p)
{
    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
    for (const QPointF &q : p) { x0 = std::min(x0, q.x()); y0 = std::min(y0, q.y()); x1 = std::max(x1, q.x()); y1 = std::max(y1, q.y()); }
    return QRectF(QPointF(x0, y0), QPointF(x1, y1));
}
static bool orthogonal(const QVector<QPointF> &p)
{
    for (int i = 0; i < p.size(); ++i) {
        const QPointF &u = p[i], &v = p[(i + 1) % p.size()];
        if (!(near(u.x(), v.x()) || near(u.y(), v.y())) || (near(u.x(), v.x()) && near(u.y(), v.y())))
            return false;
    }
    return true;
}
static bool matesOk(const BoxResult &r)
{
    for (int i = 0; i < r.panels.size(); ++i)
        for (const BoxEdge &e : r.panels[i].edges) {
            if (e.matePanel < 0) continue;
            const BoxEdge &m = r.panels[e.matePanel].edges[e.mateEdge];
            if (m.matePanel != i || m.fingers.size() != e.fingers.size() || e.fingers.size() % 2 == 0)
                return false;
            const int n = e.fingers.size();
            for (int k = 0; k < n; ++k)   // both directions: the pattern is symmetric
                if (e.fingers[k] == m.fingers[k] || e.fingers[k] == m.fingers[n - 1 - k])
                    return false;
        }
    return true;
}
static double wallVolume(const BoxResult &r, double t, bool lid)
{
    const double W = r.outerW, D = r.outerD, H = r.outerH;
    return W * D * H - (W - 2 * t) * (D - 2 * t) * (H - t - (lid ? t : 0));
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    check(boxFingerCount(100, 18) % 2 == 1, "finger count is odd");
    check(boxFingerCount(100, 20) == 5, "100 mm at 20 mm fingers is 5");
    check(boxFingerCount(5, 50) == 1, "a short edge gets one finger");
    check(near(boxDefaultFinger(6), 18), "default finger is 3 x thickness");

    for (bool lid : {false, true}) {
        BoxSpec s;
        s.width = 150; s.depth = 100; s.height = 60; s.thickness = 6; s.fingerWidth = 18;
        s.clearance = 0; s.lid = lid; s.bitDiameter = 3.175;
        const BoxResult r = makeFingerBox(s);
        check(r.error.isEmpty(), "box made");
        check(r.panels.size() == (lid ? 6 : 5), lid ? "six parts with a lid" : "five parts without a lid");
        bool sizes = true, ortho = true;
        double vol = 0;
        for (const BoxPanel &p : r.panels) {
            const QRectF b = bounds(p.outline);
            sizes = sizes && near(b.width(), p.width) && near(b.height(), p.height);
            ortho = ortho && orthogonal(p.outline);
            vol += std::fabs(area(p.outline)) * s.thickness;
        }
        check(sizes, "every part spans its outer size");
        check(ortho, "outlines are square-cornered and closed");
        check(near(r.panels[0].width, 150) && near(r.panels[0].height, 100), "bottom is W x D");
        check(near(r.panels[1].width, 150) && near(r.panels[1].height, 60), "front is W x H");
        check(near(r.panels[3].width, 100) && near(r.panels[3].height, 60), "ends are D x H");
        bool attached = true;   // a corner block never hangs on by its tip
        for (const BoxPanel &p : r.panels)
            for (int c = 0; c < 4; ++c) {
                if (!p.corners[c]) continue;
                const BoxEdge &a = p.edges[c], &b = p.edges[(c + 1) % 4];
                const bool fa = a.fingers.isEmpty() || a.fingers.last();
                const bool fb = b.fingers.isEmpty() || b.fingers.first();
                attached = attached && (fa || fb);
            }
        check(attached, "corner blocks join a finger on their part");
        check(matesOk(r), "each joint: one part's fingers are the other's gaps");
        check(near(vol, wallVolume(r, s.thickness, lid), 1e-3), "parts fill the box walls exactly once");
        // Spacing between neighbours at least twice the bit.
        bool spaced = true;
        for (int i = 0; i + 1 < r.panels.size(); ++i) {
            const QRectF a = bounds(r.panels[i].outline), b = bounds(r.panels[i + 1].outline);
            if (near(a.top(), b.top()) && b.left() - a.right() < 2 * s.bitDiameter - 1e-9)
                spaced = false;
        }
        check(spaced, "parts at least two bit widths apart");
        check(bounds(r.panels[0].outline).left() >= 2 * s.bitDiameter - 1e-9, "first part clear of the stock edge");

        // A fit gap takes material off the fingers only.
        s.clearance = 0.2;
        const BoxResult g = makeFingerBox(s);
        double vg = 0; bool ok = true;
        for (const BoxPanel &p : g.panels) { vg += std::fabs(area(p.outline)) * s.thickness; ok = ok && orthogonal(p.outline); }
        check(ok && vg < vol && vg > vol * 0.97, "fit gap trims the fingers a little");
    }

    BoxSpec in;
    in.width = 100; in.depth = 80; in.height = 50; in.thickness = 6; in.insideSize = true; in.lid = true;
    const BoxResult ri = makeFingerBox(in);
    check(near(ri.outerW, 112) && near(ri.outerD, 92) && near(ri.outerH, 62), "inside size adds the walls");

    BoxSpec row;
    row.stockWidth = 400; row.lid = true;
    const BoxResult rr = makeFingerBox(row);
    bool inside = true;
    for (const BoxPanel &p : rr.panels) inside = inside && bounds(p.outline).right() <= 400;
    check(inside, "parts wrap to a new row inside the stock width");

    BoxSpec tiny;
    tiny.width = 10; tiny.thickness = 6;
    check(!makeFingerBox(tiny).error.isEmpty(), "too small for the material is refused");

    // Drawn on the canvas as closed paths, one undo step.
    {
        Document doc;
        Canvas canvas;
        canvas.setDocument(&doc);
        BoxSpec s; s.lid = true;
        const BoxResult r = makeFingerBox(s);
        QVector<Element> els;
        for (const BoxPanel &p : r.panels)
            els << Element::makePath(p.outline, true, doc.defaultLayer());
        canvas.addElements(els, QStringLiteral("box with finger joints"));
        check(doc.elements().size() == 6, "six closed vectors on the canvas");
        check(canvas.undoStack()->count() == 1, "one undo step");
        canvas.undoStack()->undo();
        check(doc.elements().isEmpty(), "one undo takes the whole box away");
    }

    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
