// Headless checks for the image tracer (src/imagetrace.*) and the background
// image container round trip (src/backgroundimage.*). Plain asserts, no
// framework; exits 0 on success.

#include "../src/backgroundimage.h"
#include "../src/element.h"
#include "../src/imagetrace.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QPainter>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtMath>

#include <cmath>
#include <cstdio>
#include <cstdlib>

static int g_checks = 0;

static void check(bool cond, const char *what)
{
    ++g_checks;
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

static bool near(double a, double b, double tol)
{
    return std::fabs(a - b) <= tol;
}

static QImage blank(int w, int h)
{
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(Qt::white);
    return img;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // ---- 1. filled circle: one outer contour, area ≈ πr², bbox = the circle
    {
        const int W = 400, H = 400, R = 100;
        QImage img = blank(W, H);
        {
            QPainter p(&img);
            p.setRenderHint(QPainter::Antialiasing, false);
            p.setPen(Qt::NoPen);
            p.setBrush(Qt::black);
            p.drawEllipse(QPoint(200, 200), R, R);
        }
        c2d::TraceOptions o;
        o.mmPerPixel = 0.1;               // 400 px -> 40 mm
        o.simplifyTolMm = 0.08;
        o.origin = QPointF(10, 20);
        const c2d::TraceResult r = c2d::traceImage(img, o);
        check(r.contours.size() == 1, "circle: exactly one contour");
        const c2d::TraceContour &c = r.contours.first();
        check(!c.hole, "circle: contour is an outer boundary");
        const double expected = M_PI * 10.0 * 10.0;   // r = 10 mm
        std::printf("circle: area %.3f mm² (expected %.3f), %d vertices, %lld ms\n",
                    c.area, expected, int(c.pts.size()), (long long)r.elapsedMs);
        check(near(c.area, expected, expected * 0.02), "circle: area within 2% of πr²");
        const QRectF bb = c.bounds();
        // centre (200,200) px -> (10 + 20, 20 + (400-200)*0.1) = (30, 40) mm
        check(near(bb.left(), 20.0, 0.25) && near(bb.right(), 40.0, 0.25), "circle: bbox x");
        check(near(bb.top(), 30.0, 0.25) && near(bb.bottom(), 50.0, 0.25), "circle: bbox y");
        check(c.pts.size() < 200, "circle: simplification removed the staircase");
        // ring orientation: outer contours are CCW in Y-up space
        double a2 = 0;
        for (int i = 0; i < c.pts.size(); ++i) {
            const QPointF &p = c.pts[i], &q = c.pts[(i + 1) % c.pts.size()];
            a2 += p.x() * q.y() - q.x() * p.y();
        }
        check(a2 > 0, "circle: outer contour is counter-clockwise");

        // element schema: closed path, first row type 0, last row type 4
        const c2d::Element e = c2d::traceContourElement(c, false, QJsonObject());
        check(e.geometryType == QLatin1String("path"), "circle: element is a path");
        const QJsonArray pt = e.raw.value("point_type").toArray();
        check(pt.first().toInt() == 0 && pt.last().toInt() == 4, "circle: closed path rows");
        check(e.raw.value("points").toArray().size() == pt.size()
              && e.raw.value("cp1").toArray().size() == pt.size()
              && e.raw.value("cp2").toArray().size() == pt.size()
              && e.raw.value("smooth").toArray().size() == pt.size(), "circle: row arrays aligned");
        check(near(std::fabs(e.painterPath.boundingRect().width()), 20.0, 0.3), "circle: element path width");

        // smoothed variant: cubic rows, same extent
        const c2d::TraceOptions os = [&] { auto t = o; t.smooth = true; return t; }();
        const c2d::TraceResult rs = c2d::traceImage(img, os);
        check(rs.contours.size() == 1 && rs.contours.first().corner.size() == rs.contours.first().pts.size(),
              "smooth circle: corner flags per vertex");
        const c2d::Element es = c2d::traceContourElement(rs.contours.first(), true, QJsonObject());
        const QJsonArray pts = es.raw.value("point_type").toArray();
        int cubic = 0;
        for (const QJsonValue &v : pts) cubic += v.toInt() == 3 ? 1 : 0;
        check(cubic > 0, "smooth circle: cubic rows emitted");
        check(pts.first().toInt() == 0 && pts.last().toInt() == 4, "smooth circle: closed rows");
        check(near(es.painterPath.boundingRect().width(), 20.0, 0.4), "smooth circle: path width");
        // a re-parse of the JSON yields the same path (what the canvas/loader do)
        const c2d::Element again = c2d::Element::fromJson(es.raw);
        check(near(again.painterPath.boundingRect().width(), es.painterPath.boundingRect().width(), 1e-9),
              "smooth circle: JSON round trip");
    }

    // ---- 2. ring: outer contour + hole, areas ≈ πR² and πr²
    {
        const int W = 400, H = 400;
        QImage img = blank(W, H);
        {
            QPainter p(&img);
            p.setRenderHint(QPainter::Antialiasing, false);
            p.setPen(Qt::NoPen);
            p.setBrush(Qt::black);
            p.drawEllipse(QPoint(200, 200), 150, 150);
            p.setBrush(Qt::white);
            p.drawEllipse(QPoint(200, 200), 80, 80);
        }
        c2d::TraceOptions o;
        o.mmPerPixel = 0.1;
        o.simplifyTolMm = 0.08;
        const c2d::TraceResult r = c2d::traceImage(img, o);
        check(r.contours.size() == 2, "ring: two contours");
        const c2d::TraceContour *outer = nullptr, *hole = nullptr;
        for (const c2d::TraceContour &c : r.contours)
            (c.hole ? hole : outer) = &c;
        check(outer && hole, "ring: one outer, one hole");
        const double eo = M_PI * 15.0 * 15.0, eh = M_PI * 8.0 * 8.0;
        std::printf("ring: outer %.3f (exp %.3f), hole %.3f (exp %.3f)\n", outer->area, eo, hole->area, eh);
        check(near(outer->area, eo, eo * 0.02), "ring: outer area");
        check(near(hole->area, eh, eh * 0.02), "ring: hole area");
        check(near(outer->bounds().width(), 30.0, 0.25) && near(hole->bounds().width(), 16.0, 0.25),
              "ring: bbox widths");
        check(hole->bounds().center().x() > 19 && hole->bounds().center().x() < 21, "ring: concentric");

        // despeckle: min area above the hole size removes it
        c2d::TraceOptions o2 = o;
        o2.minAreaMm2 = eh * 1.5;
        check(c2d::traceImage(img, o2).contours.size() == 1, "ring: despeckle drops the hole");

        // invert: the white disc inside becomes the ink
        c2d::TraceOptions o3 = o;
        o3.invert = true;
        const c2d::TraceResult r3 = c2d::traceImage(img, o3);
        bool foundDisc = false;
        for (const c2d::TraceContour &c : r3.contours)
            if (!c.hole && near(c.area, eh, eh * 0.02)) foundDisc = true;
        check(foundDisc, "ring: invert traces the inner disc as ink");
    }

    // ---- 3. rectangle: 4 vertices after simplification, corners preserved by smoothing
    {
        QImage img = blank(300, 200);
        {
            QPainter p(&img);
            p.fillRect(QRect(50, 40, 200, 100), Qt::black);
        }
        c2d::TraceOptions o;
        o.mmPerPixel = 0.5;
        o.simplifyTolMm = 0.3;
        o.smooth = true;
        const c2d::TraceResult r = c2d::traceImage(img, o);
        check(r.contours.size() == 1, "rect: one contour");
        const c2d::TraceContour &c = r.contours.first();
        check(c.pts.size() == 4, "rect: four vertices");
        int corners = 0;
        for (bool b : c.corner) corners += b ? 1 : 0;
        check(corners == 4, "rect: all four corners preserved");
        check(near(c.area, 100.0 * 50.0, 1.0), "rect: area 100×50 mm");
        const c2d::Element e = c2d::traceContourElement(c, true, QJsonObject());
        const QJsonArray pt = e.raw.value("point_type").toArray();
        int cubic = 0;
        for (const QJsonValue &v : pt) cubic += v.toInt() == 3 ? 1 : 0;
        check(cubic == 0, "rect: corner-only contour stays straight");
    }

    // ---- 4. Douglas–Peucker on its own
    {
        QPolygonF ring;
        for (int i = 0; i < 360; ++i)
            ring << QPointF(50 * std::cos(qDegreesToRadians(double(i))),
                            50 * std::sin(qDegreesToRadians(double(i))));
        const QPolygonF s1 = c2d::simplifyClosed(ring, 0.05);
        const QPolygonF s2 = c2d::simplifyClosed(ring, 2.0);
        check(s1.size() < ring.size() && s2.size() < s1.size() && s2.size() >= 3,
              "DP: coarser tolerance -> fewer points");
        check(c2d::simplifyClosed(ring, 0).size() == ring.size(), "DP: zero tolerance keeps all");
    }

    // ---- 5. performance: 2000×2000 with a busy pattern in < 2 s
    {
        QImage img = blank(2000, 2000);
        {
            QPainter p(&img);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);
            p.setBrush(Qt::black);
            for (int j = 0; j < 16; ++j)
                for (int i = 0; i < 16; ++i)
                    p.drawEllipse(QPoint(100 + 120 * i, 100 + 120 * j), 45, 45);
            p.setBrush(Qt::white);
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i)
                    p.drawEllipse(QPoint(100 + 240 * i, 100 + 240 * j), 20, 20);
        }
        c2d::TraceOptions o;
        o.mmPerPixel = 0.1;
        o.blurRadius = 1;
        o.minAreaMm2 = 0.2;
        o.simplifyTolMm = 0.05;
        o.smooth = true;
        // Best of three runs: the wall clock on a loaded build box is noisy.
        c2d::TraceResult r;
        qint64 ms = -1;
        for (int run = 0; run < 3; ++run) {
            QElapsedTimer t;
            t.start();
            r = c2d::traceImage(img, o);
            const qint64 e = t.elapsed();
            if (ms < 0 || e < ms) ms = e;
        }
        std::printf("perf: 2000x2000 -> %d contours, best of 3: %lld ms\n", int(r.contours.size()), (long long)ms);
        check(r.contours.size() == 16 * 16 + 8 * 8, "perf: 256 discs + 64 holes");
#ifdef NDEBUG
        check(ms < 2000, "perf: 2000x2000 traced in < 2 s");
#endif
    }

    // ---- 6. background image container round trip on a minimal .c2d-shaped DB
    {
        QTemporaryDir dir;
        check(dir.isValid(), "bg: temp dir");
        const QString path = dir.filePath(QStringLiteral("bg.c2d"));
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("mk"));
            db.setDatabaseName(path);
            check(db.open(), "bg: create db");
            QSqlQuery q(db);
            q.exec(QStringLiteral("CREATE TABLE params(key TEXT PRIMARY KEY, value TEXT)"));
            q.exec(QStringLiteral("CREATE TABLE sqlar(name TEXT PRIMARY KEY, mode INT, mtime INT, sz INT, data BLOB)"));
            q.exec(QStringLiteral("INSERT INTO params VALUES('background_visible','0')"));
            q.exec(QStringLiteral("INSERT INTO sqlar VALUES('background.png',33188,'2026-01-01 00:00:00',0,x'')"));
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("mk"));

        c2d::BackgroundImage none;
        check(none.loadFrom(path) && none.isNull() && !none.visible, "bg: empty placeholder loads as none");

        c2d::BackgroundImage bg;
        QImage pic(64, 32, QImage::Format_ARGB32);
        pic.fill(QColor(200, 30, 30));
        bg.setImage(pic);
        bg.visible = true;
        bg.x = 12.5; bg.y = -3.25;
        bg.setWidthMm(128.0);
        bg.rotationDeg = 15;
        bg.opacity = 0.35;
        bg.locked = true;
        check(near(bg.widthMm(), 128.0, 1e-9) && near(bg.heightMm(), 64.0, 1e-9), "bg: width keeps aspect");
        QString err;
        check(bg.saveTo(path, &err), "bg: save");

        c2d::BackgroundImage back;
        check(back.loadFrom(path, &err), "bg: reload");
        check(!back.isNull() && back.image.size() == QSize(64, 32), "bg: picture round trip");
        check(back.visible && back.locked, "bg: flags round trip");
        check(near(back.x, 12.5, 1e-9) && near(back.y, -3.25, 1e-9), "bg: position round trip");
        check(near(back.widthMm(), 128.0, 1e-6) && near(back.rotationDeg, 15, 1e-9)
              && near(back.opacity, 0.35, 1e-9), "bg: scale/rotation/opacity round trip");
        check(back.pngData.startsWith(QByteArray("\x89PNG")), "bg: stored as PNG bytes");

        // mapping: pixel bottom-left (0,H) -> (x,y); top-right (W,0) -> (x+w, y+h) with no rotation
        back.rotationDeg = 0;
        const QTransform t = back.pixelToMm();
        const QPointF bl = t.map(QPointF(0, 32)), tr = t.map(QPointF(64, 0));
        check(near(bl.x(), 12.5, 1e-9) && near(bl.y(), -3.25, 1e-9), "bg: pixel->mm bottom-left");
        check(near(tr.x(), 12.5 + 128, 1e-9) && near(tr.y(), -3.25 + 64, 1e-9), "bg: pixel->mm top-right (Y-up)");

        // clear + save removes the picture again
        back.clear();
        check(back.saveTo(path, &err), "bg: save cleared");
        c2d::BackgroundImage gone;
        check(gone.loadFrom(path) && gone.isNull(), "bg: cleared round trip");

        // A background we could not decode must survive a save. Otherwise the
        // first Ctrl+S after opening such a file destroys the user's picture.
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                        QStringLiteral("bad"));
            db.setDatabaseName(path);
            check(db.open(), "bg: reopen to plant an undecodable row");
            QSqlQuery q(db);
            q.exec(QStringLiteral("INSERT OR REPLACE INTO sqlar VALUES"
                                  "('background.png',33188,'2026-01-01 00:00:00',8,x'6E6F7461706E6721')"));
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("bad"));

        c2d::BackgroundImage broken;
        check(!broken.loadFrom(path, &err) && broken.isNull(),
              "bg: an undecodable background reports failure");
        check(broken.saveTo(path, &err), "bg: saving after a failed load succeeds");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                        QStringLiteral("chk"));
            db.setDatabaseName(path);
            check(db.open(), "bg: reopen to inspect");
            QSqlQuery q(db);
            q.exec(QStringLiteral("SELECT length(data) FROM sqlar WHERE name='background.png'"));
            check(q.next() && q.value(0).toInt() == 8,
                  "bg: the undecodable row is left untouched");
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("chk"));

        // A document that never had a background gains no background row.
        const QString clean = dir.filePath(QStringLiteral("clean.c2d"));
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                        QStringLiteral("mk2"));
            db.setDatabaseName(clean);
            check(db.open(), "bg: create a document with no background");
            QSqlQuery q(db);
            q.exec(QStringLiteral("CREATE TABLE params(key TEXT PRIMARY KEY, value TEXT)"));
            q.exec(QStringLiteral("CREATE TABLE sqlar(name TEXT PRIMARY KEY, mode INT, "
                                  "mtime INT, sz INT, data BLOB)"));
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("mk2"));
        c2d::BackgroundImage fresh;
        check(fresh.loadFrom(clean, &err), "bg: document with no background loads");
        check(fresh.saveTo(clean, &err), "bg: saving it succeeds");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                        QStringLiteral("chk2"));
            db.setDatabaseName(clean);
            check(db.open(), "bg: reopen the clean document");
            QSqlQuery q(db);
            q.exec(QStringLiteral("SELECT count(*) FROM sqlar WHERE name='background.png'"));
            check(q.next() && q.value(0).toInt() == 0,
                  "bg: no empty background row is created");
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("chk2"));
    }

    std::printf("test_trace: %d checks passed\n", g_checks);
    return 0;
}
