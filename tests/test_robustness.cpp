// Damaged and hostile input must never crash or hang the app: a CNC program
// is often opened from a USB stick, a half-synced folder or a file someone
// else wrote. Every loader here is expected either to fail cleanly (return
// false / ok = false with a message) or to succeed with whatever it could
// salvage — but never to abort, and never to allocate on an attacker's word.
//
// Plain asserts, no framework; exits 0 on success.

#include "../src/backgroundimage.h"
#include "../src/c2ddocument.h"
#include "../src/gcodeexport.h"
#include "../src/importers.h"
#include "../src/model3d.h"

#include <QCoreApplication>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

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

static bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(bytes);
    return true;
}

// Copy `src` and run one SQL statement over the copy.
static bool damage(const QString &src, const QString &dst, const QString &sql)
{
    QFile::remove(dst);
    if (!QFile::copy(src, dst))
        return false;
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                    QStringLiteral("dmg"));
        db.setDatabaseName(dst);
        if (db.open()) {
            QSqlQuery q(db);
            ok = q.exec(sql);
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(QStringLiteral("dmg"));
    return ok;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QString sample = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();

    QTemporaryDir dir;
    check(dir.isValid(), "temp dir");

    // ---- documents that are not documents ---------------------------------
    {
        const QString empty = dir.filePath(QStringLiteral("empty.c2d"));
        check(writeFile(empty, QByteArray()), "write empty file");
        c2d::Document doc;
        QString err;
        check(!doc.load(empty, &err), "an empty file is rejected");
        check(!err.isEmpty(), "rejection says why");

        const QString junk = dir.filePath(QStringLiteral("junk.c2d"));
        check(writeFile(junk, QByteArray("not a database, just words\n").repeated(200)),
              "write junk file");
        c2d::Document doc2;
        check(!doc2.load(junk, &err), "a non-database is rejected");

        c2d::Document doc3;
        check(!doc3.load(dir.filePath(QStringLiteral("does-not-exist.c2d")), &err),
              "a missing file is rejected");

        // Loaders that read the same container must be just as careful.
        c2d::BackgroundImage bg;
        check(!bg.loadFrom(junk) && bg.isNull(), "background loader survives junk");
        c2d::Model3D m;
        check(!m.loadFrom(junk) || m.components.isEmpty(), "model loader survives junk");
    }

    // ---- a real document, damaged in every way that matters ---------------
    if (!sample.isEmpty() && QFile::exists(sample)) {
        struct Case { const char *name; const char *sql; };
        // Each of these has been seen in the wild in one form or another:
        // truncated writes, interrupted syncs, and rows written by another
        // tool. None may crash; a clean failure is a fine outcome.
        const Case cases[] = {
            {"no_items", "DROP TABLE items"},
            {"no_params", "DROP TABLE params"},
            {"no_sqlar", "DROP TABLE sqlar"},
            {"empty_items", "DELETE FROM items"},
            {"null_blobs", "UPDATE items SET data = NULL"},
            {"junk_blobs", "UPDATE items SET data = x'deadbeef'"},
            {"huge_sz", "UPDATE items SET sz = 999999999"},
            {"negative_sz", "UPDATE items SET sz = -5"},
            {"junk_params", "UPDATE params SET value = 'not-a-number'"},
            {"junk_model", "UPDATE items SET data = x'00' WHERE uuid = 'MODEL'"},
        };
        for (const Case &c : cases) {
            const QString path = dir.filePath(QString::fromLatin1(c.name) + QStringLiteral(".c2d"));
            if (!damage(sample, path, QString::fromLatin1(c.sql)))
                continue;               // the statement did not apply; skip
            c2d::Document doc;
            QString err;
            const bool loaded = doc.load(path, &err);
            ++g_checks;                 // reaching here at all is the check
            if (loaded) {
                // Whatever survived must still be safe to machine and to
                // hand to the loaders that share the container.
                const c2d::GcodeResult g = c2d::exportGcode(doc);
                (void)g;
                c2d::BackgroundImage bg;
                bg.loadFrom(path);
                c2d::Model3D m;
                m.loadFrom(path);
            }
        }
        std::printf("robustness: %d damaged documents handled\n", int(std::size(cases)));

        // A toolpath row this build cannot decode must survive a save. It is
        // skipped at load (so it cannot be shown or machined), but deleting it
        // would silently throw away the user's work on the next Ctrl+S.
        {
            const QString path = dir.filePath(QStringLiteral("onebadtp.c2d"));
            QFile::remove(path);
            check(QFile::copy(sample, path), "copy sample for the undecodable-row case");
            int before = 0;
            {
                QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                            QStringLiteral("bad1"));
                db.setDatabaseName(path);
                check(db.open(), "open the copy");
                QSqlQuery q(db);
                q.exec(QStringLiteral("SELECT count(*) FROM items WHERE type='toolpath'"));
                check(q.next(), "count toolpath rows");
                before = q.value(0).toInt();
                check(before >= 2, "the sample has several toolpaths");
                // Corrupt exactly one payload.
                q.exec(QStringLiteral("UPDATE items SET data = x'deadbeef' WHERE id = "
                                      "(SELECT min(id) FROM items WHERE type='toolpath')"));
                db.close();
            }
            QSqlDatabase::removeDatabase(QStringLiteral("bad1"));

            c2d::Document doc;
            QString err;
            check(doc.load(path, &err), "document with one unreadable toolpath still loads");
            check(doc.toolpaths().size() == before - 1, "the unreadable toolpath is not shown");
            check(doc.unreadableToolpaths().size() == 1, "it is remembered instead");

            const QString out = dir.filePath(QStringLiteral("onebadtp-saved.c2d"));
            check(doc.save(out, &err), "saving succeeds");
            {
                QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                            QStringLiteral("bad2"));
                db.setDatabaseName(out);
                check(db.open(), "open the saved file");
                QSqlQuery q(db);
                q.exec(QStringLiteral("SELECT count(*) FROM items WHERE type='toolpath'"));
                check(q.next() && q.value(0).toInt() == before,
                      "every toolpath row survives the save, decodable or not");
                db.close();
            }
            QSqlDatabase::removeDatabase(QStringLiteral("bad2"));
        }

        // Truncation at several points: the most common real-world damage.
        QFile in(sample);
        check(in.open(QIODevice::ReadOnly), "read sample");
        const QByteArray whole = in.readAll();
        in.close();
        for (double frac : {0.01, 0.1, 0.5, 0.9, 0.99}) {
            const QString path = dir.filePath(QStringLiteral("trunc%1.c2d").arg(int(frac * 100)));
            check(writeFile(path, whole.left(int(whole.size() * frac))), "write truncated");
            c2d::Document doc;
            QString err;
            doc.load(path, &err);       // must return, either way
            ++g_checks;
        }
    }

    // ---- vector imports: malformed, hostile and absurd ---------------------
    {
        c2d::ImportOptions opt;
        opt.stockWidth = 200;
        opt.stockHeight = 200;

        struct V { const char *name; const char *body; };
        const V svgs[] = {
            {"unclosed", "<svg xmlns='http://www.w3.org/2000/svg' width='10mm' height='10mm'>"
                         "<path d='M0 0 C 1 1"},
            {"nonsense_path", "<svg xmlns='http://www.w3.org/2000/svg' width='10mm' height='10mm'>"
                              "<path d='A A A z z M'/></svg>"},
            {"nan_numbers", "<svg xmlns='http://www.w3.org/2000/svg' width='NaNmm' height='1e400mm'>"
                            "<rect x='nan' y='inf' width='-5' height='1e999'/></svg>"},
            {"huge_arc", "<svg xmlns='http://www.w3.org/2000/svg' width='10mm' height='10mm'>"
                         "<path d='M0 0 A 1e300 1e300 0 1 1 5 5'/></svg>"},
            {"no_viewbox", "<svg xmlns='http://www.w3.org/2000/svg'><circle r='5'/></svg>"},
            {"empty", ""},
        };
        for (const V &v : svgs) {
            const c2d::ImportResult r =
                c2d::importSvgData(QByteArray(v.body), opt, QString::fromLatin1(v.name));
            ++g_checks;
            for (const c2d::Element &e : r.elements) {
                const QRectF bb = e.painterPath.boundingRect();
                check(std::isfinite(bb.x()) && std::isfinite(bb.y())
                          && std::isfinite(bb.width()) && std::isfinite(bb.height()),
                      "imported svg geometry is finite");
            }
        }

        const V dxfs[] = {
            {"truncated", "0\nSECTION\n2\nENTITIES\n0\nLWPOLYLINE\n90\n5\n10\n0.0\n"},
            {"bad_codes", "0\nSECTION\n2\nENTITIES\n0\nCIRCLE\n10\nabc\n20\ndef\n40\n-1\n"
                          "0\nENDSEC\n0\nEOF\n"},
            // A vertex count no file could hold: must not be trusted as a size.
            {"absurd_count", "0\nSECTION\n2\nENTITIES\n0\nLWPOLYLINE\n90\n999999999\n"
                             "10\n0.0\n20\n0.0\n0\nENDSEC\n0\nEOF\n"},
            // A block that inserts itself: must not recurse forever.
            {"self_insert", "0\nSECTION\n2\nBLOCKS\n0\nBLOCK\n2\nA\n0\nINSERT\n2\nA\n10\n0\n20\n0\n"
                            "0\nENDBLK\n0\nENDSEC\n2\nENTITIES\n0\nINSERT\n2\nA\n10\n0\n20\n0\n"
                            "0\nENDSEC\n0\nEOF\n"},
            {"nan_spline", "0\nSECTION\n2\nENTITIES\n0\nSPLINE\n70\n8\n71\n3\n10\nnan\n20\ninf\n"
                           "10\n1\n20\n1\n0\nENDSEC\n0\nEOF\n"},
            {"empty", ""},
        };
        for (const V &v : dxfs) {
            const c2d::ImportResult r =
                c2d::importDxfData(QByteArray(v.body), opt, QString::fromLatin1(v.name));
            ++g_checks;
            for (const c2d::Element &e : r.elements) {
                const QRectF bb = e.painterPath.boundingRect();
                check(std::isfinite(bb.x()) && std::isfinite(bb.y())
                          && std::isfinite(bb.width()) && std::isfinite(bb.height()),
                      "imported dxf geometry is finite");
            }
        }
    }

    std::printf("test_robustness: %d checks OK\n", g_checks);
    return 0;
}
