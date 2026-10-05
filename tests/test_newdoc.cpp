// Headless check for File -> New: a blank container must load, take an
// element, save and reload with the element and stock size intact.
#include "../src/c2ddocument.h"
#include "../src/element.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <cstdio>

static int g_fail = 0;
static void check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    // Optional argv[1]: where to write the blank, kept for further manual checks.
    const QString blank = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                   : dir.filePath(QStringLiteral("new.c2d"));
    const QString saved = dir.filePath(QStringLiteral("saved.c2d"));
    QString err;

    check(c2d::Document::createBlank(blank, 300, 200, 19, &err), "createBlank");
    c2d::Document doc;
    check(doc.load(blank, &err), "load blank");
    check(doc.elements().isEmpty(), "blank has no elements");
    check(doc.params().value(QStringLiteral("width")) == QLatin1String("300")
              && doc.params().value(QStringLiteral("height")) == QLatin1String("200"),
          "stock size stored");

    doc.elementsRef().append(
        c2d::Element::makeCircle(QPointF(150, 100), 25, doc.defaultLayer()));
    check(doc.save(saved, &err), "save with an element");

    c2d::Document back;
    check(back.load(saved, &err), "reload saved");
    check(back.elements().size() == 1, "element survives round trip");
    if (!err.isEmpty())
        std::printf("     last error: %s\n", qPrintable(err));
    return g_fail ? 1 : 0;
}
