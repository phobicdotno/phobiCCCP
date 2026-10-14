// Opening a Syntexophobic .cccp project as its final product: the zip reader,
// the mimetype check, and what comes out of tests/data/plate.cccp (a 60 x 40 x
// 12 mm plate with a Ø6 hole, saved by Syntexophobic). Plain asserts; exits 0
// on success.

#include "../src/cccpfile.h"
#include "../src/importers.h"
#include "../src/model3d.h"

#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QTemporaryDir>

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

static bool near(double a, double b, double eps = 1e-3)
{
    return std::fabs(a - b) < eps;
}

static QString writeTemp(QTemporaryDir &dir, const QString &name, const QByteArray &bytes)
{
    const QString path = dir.filePath(name);
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(bytes);
    return path;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QString plate = QStringLiteral(TEST_DATA_DIR "/plate.cccp");

    check(isCccpFile(QStringLiteral("a/b/Part.CCCP")), "suffix, any case");
    check(!isCccpFile(QStringLiteral("part.c2d")), "c2d is not cccp");

    const CccpProduct p = readCccp(plate);
    check(p.ok, "plate.cccp opens");
    check(p.name == QLatin1String("plate"), "name from info.json");
    const QJsonArray size = p.info.value(QStringLiteral("size")).toArray();
    check(size.size() == 3 && near(size[0].toDouble(), 60) && near(size[1].toDouble(), 40)
              && near(size[2].toDouble(), 12), "info.json size");

    QVector<StlTriangle> tris;
    QString err;
    check(parseStl(p.stl, &tris, &err) && tris.size() > 12, "part.stl parses");
    float mx[3] = {-1e30f, -1e30f, -1e30f};
    for (const StlTriangle &t : tris)
        for (int v = 0; v < 3; ++v)
            for (int k = 0; k < 3; ++k)
                mx[k] = std::max(mx[k], t.v[v][k]);
    check(near(mx[0], 60, 1e-2) && near(mx[1], 40, 1e-2) && near(mx[2], 12, 1e-2), "part is 60 x 40 x 12 mm");

    ImportOptions opt;
    opt.autoPlace = false;
    const ImportResult r = importSvgData(p.svg, opt, QStringLiteral("drawing.svg"));
    check(r.ok, "drawing.svg imports");
    // the rectangle's four sides join into one outline, plus the hole
    check(r.elements.size() >= 2, "rectangle and hole");
    check(near(r.bounds.width(), 60, 0.05) && near(r.bounds.height(), 40, 0.05), "drawing is 60 x 40 mm");
    // drawing.svg's corner is 10 mm left of and below the part, so the drawing
    // lands where the STL goes when that is placed at (10, 10)
    check(near(r.bounds.left(), 10, 0.05) && near(r.bounds.top(), 10, 0.05), "drawing lines up with the part");

    // Not a Syntexophobic file: a plain zip, and junk.
    QTemporaryDir tmp;
    QHash<QString, QByteArray> all;
    QFile f(plate);
    f.open(QIODevice::ReadOnly);
    const QByteArray zip = f.readAll();
    check(readZip(zip, {}, &all, &err), "reads every entry");
    check(all.contains(QStringLiteral("job.toml")) && all.contains(QStringLiteral(".git/HEAD")),
          "the project and its history are in there too");
    check(all.value(QStringLiteral("mimetype")) == QByteArray(kCccpMimetype), "mimetype");

    QByteArray other = zip;
    other.replace(QByteArray(kCccpMimetype), QByteArray("application/x-something-else"));
    check(!readCccp(writeTemp(tmp, QStringLiteral("other.cccp"), other)).ok, "wrong mimetype refused");
    check(!readCccp(writeTemp(tmp, QStringLiteral("junk.cccp"), QByteArray("not a zip"))).ok, "junk refused");
    check(!readCccp(writeTemp(tmp, QStringLiteral("cut.cccp"), zip.left(zip.size() / 2))).ok,
          "truncated file refused");

    std::printf("cccpfile: %d checks passed\n", g_checks);
    return 0;
}
