// The pad's Preview (padlink::previewJson): a design with one pocket comes
// back as a picture of its toolpaths with a time; a huge one is refused.
//
// Plain checks, no framework; exits 0 on success.
#include "../src/padpreview.h"

#include <QBuffer>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QStandardPaths>
#include <cstdio>

using namespace c2d;

static int failures = 0;
static void check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++failures;
}

static QJsonObject design(int shapes)
{
    QJsonArray s;
    s.append(QJsonObject{{"id", "r"}, {"type", "rect"}, {"x", 20}, {"y", 20}, {"w", 60}, {"h", 40}});
    for (int i = 1; i < shapes; ++i)
        s.append(QJsonObject{{"id", QStringLiteral("c%1").arg(i)}, {"type", "circle"},
                             {"x", 100}, {"y", 50}, {"r", 5}});
    return QJsonObject{
        {"format", "phobicccp-pad"}, {"version", 1}, {"id", "prev-1"}, {"name", "Preview"},
        {"modified", 1000.0}, {"material", "softwood"},
        {"stock", QJsonObject{{"width", 200}, {"height", 100}, {"thickness", 12}}},
        {"shapes", s},
        {"toolpaths", QJsonArray{QJsonObject{{"name", "Pocket"}, {"type", "pocket"},
                                             {"shapes", QJsonArray{"r"}}, {"tool", 201}, {"depth", 3}}}}};
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QGuiApplication app(argc, argv);

    const QJsonObject r = padlink::previewJson(design(1));
    if (!r.value("ok").toBool())
        std::printf("error: %s\n", qPrintable(r.value("error").toString()));
    check(r.value("ok").toBool(), "a design with one pocket previews");
    const QByteArray png = QByteArray::fromBase64(r.value("png").toString().toLatin1());
    check(png.startsWith("\x89PNG"), "the preview is a PNG");
    QImage img;
    img.loadFromData(png, "PNG");
    if (const QString out = qEnvironmentVariable("PADPREVIEW_PNG"); !out.isEmpty())
        img.save(out);   // to look at it
    check(!img.isNull() && qMax(img.width(), img.height()) == 1024, "about 1024 px on its long side");
    check(img.width() > img.height(), "as wide as the stock is");
    check(r.value("seconds").toDouble() > 0, "it takes some time");
    const QJsonArray per = r.value("perToolpath").toArray();
    check(per.size() == 1 && per.at(0).toObject().value("name").toString() == QLatin1String("Pocket")
              && per.at(0).toObject().value("seconds").toDouble() > 0,
          "the pocket's own time");
    check(r.value("notes").isArray(), "notes are a list");

    const QJsonObject big = padlink::previewJson(design(padlink::kMaxPreviewShapes + 1));
    check(!big.value("ok").toBool() && big.value("error").toString().contains("too big"),
          "a huge design is refused with a reason");
    QJsonObject bad = design(1);
    bad.insert("version", 9);
    check(!padlink::previewJson(bad).value("ok").toBool(), "a malformed design is refused");

    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
