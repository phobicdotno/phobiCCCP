// The PhobiCCCP phone app's side of phobiCCCP: a pad design checked and
// turned into an ordinary document that exports, the design store that keeps
// every phone in step, and machine profiles swapping the machine/ settings.
#include "../src/c2ddocument.h"
#include "../src/gcodeexport.h"
#include "../src/machineprofiles.h"
#include "../src/padlink.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>

using namespace c2d;

static int g_fail = 0;
static void check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

// A flat end mill and the material it has feeds for, from the library itself.
static int pickTool(const char *kind, QString *material)
{
    const QJsonObject lib = padlink::toolsJson();
    for (const QJsonValue &v : lib.value(QStringLiteral("tools")).toArray()) {
        const QJsonObject t = v.toObject();
        const QJsonArray mats = t.value(QStringLiteral("materials")).toArray();
        if (t.value(QStringLiteral("kind")).toString() == QLatin1String(kind)
            && mats.contains(QStringLiteral("softwood"))) {
            *material = QStringLiteral("softwood");
            return t.value(QStringLiteral("number")).toInt();
        }
    }
    return 0;
}

static QJsonObject design(int tool, const QString &material)
{
    auto tp = [&](const char *name, const char *type, const QStringList &shapes, double depth,
                  const char *side = "outside") {
        return QJsonObject{{QStringLiteral("name"), QLatin1String(name)},
                           {QStringLiteral("type"), QLatin1String(type)},
                           {QStringLiteral("side"), QLatin1String(side)},
                           {QStringLiteral("shapes"), QJsonArray::fromStringList(shapes)},
                           {QStringLiteral("tool"), tool},
                           {QStringLiteral("depth"), depth}};
    };
    return QJsonObject{
        {QStringLiteral("format"), QStringLiteral("phobicccp-pad")},
        {QStringLiteral("version"), 1},
        {QStringLiteral("id"), QStringLiteral("sign-1")},
        {QStringLiteral("name"), QStringLiteral("Sign")},
        {QStringLiteral("modified"), 1000.0},
        {QStringLiteral("material"), material},
        {QStringLiteral("stock"), QJsonObject{{QStringLiteral("width"), 300},
                                              {QStringLiteral("height"), 200},
                                              {QStringLiteral("thickness"), 12}}},
        {QStringLiteral("shapes"), QJsonArray{
            QJsonObject{{"id", "r"}, {"type", "rect"}, {"x", 10}, {"y", 10}, {"w", 280}, {"h", 180}},
            QJsonObject{{"id", "c"}, {"type", "circle"}, {"x", 60}, {"y", 100}, {"r", 25}},
            QJsonObject{{"id", "g"}, {"type", "polygon"}, {"x", 240}, {"y", 100}, {"r", 25}, {"sides", 6}},
            QJsonObject{{"id", "t"}, {"type", "text"}, {"x", 100}, {"y", 40}, {"text", "Hi"}, {"size", 20}},
            QJsonObject{{"id", "p"}, {"type", "path"}, {"closed", false},
                        {"points", QJsonArray{QJsonArray{120, 150}, QJsonArray{180, 150}, QJsonArray{180, 170}}}},
            QJsonObject{{"id", "h"}, {"type", "point"}, {"x", 150}, {"y", 100}}}},
        {QStringLiteral("toolpaths"), QJsonArray{
            tp("Pocket", "pocket", {"c", "g"}, 3),
            tp("Lines", "engrave", {"p", "t"}, 1),
            tp("Hole", "drill", {"h"}, 5),
            tp("Outline", "profile", {"r"}, 12.5)}}};
}

static void testCheck(const QJsonObject &good)
{
    check(padlink::check(good).isEmpty(), "a good design passes");
    auto broken = [&](const std::function<void(QJsonObject &)> &f) {
        QJsonObject d = good;
        f(d);
        return !padlink::check(d).isEmpty();
    };
    check(broken([](QJsonObject &d) { d.insert("version", 2); }), "newer version refused");
    check(broken([](QJsonObject &d) { d.insert("id", "../x"); }), "path in id refused");
    check(broken([](QJsonObject &d) {
        QJsonArray s = d.value("shapes").toArray();
        QJsonObject r = s.at(0).toObject();
        r.insert("w", std::numeric_limits<double>::quiet_NaN());
        s.replace(0, r);
        d.insert("shapes", s);
    }), "NaN size refused");
    check(broken([](QJsonObject &d) {
        QJsonArray t = d.value("toolpaths").toArray();
        QJsonObject p = t.at(0).toObject();
        p.insert("shapes", QJsonArray{"nope"});
        t.replace(0, p);
        d.insert("toolpaths", t);
    }), "toolpath on an unknown shape refused");
    check(broken([](QJsonObject &d) {
        QJsonArray s;
        for (int i = 0; i <= padlink::kMaxShapes; ++i)
            s.append(QJsonObject{{"id", QString::number(i)}, {"type", "point"}, {"x", 1}, {"y", 1}});
        d.insert("shapes", s);
        d.insert("toolpaths", QJsonArray());
    }), "too many shapes refused");
    QJsonObject gone{{"format", "phobicccp-pad"}, {"version", 1}, {"id", "x"},
                     {"modified", 5.0}, {"deleted", true}};
    check(padlink::check(gone).isEmpty(), "a deletion needs no content");
}

static void testBuild(const QString &dir, const QJsonObject &d)
{
    const QString path = QDir(dir).filePath("sign.c2d");
    QStringList notes;
    QString err;
    const bool ok = padlink::buildDocument(d, path, &notes, &err);
    check(ok, "design builds a document");
    if (!ok) {
        std::printf("     %s\n", qPrintable(err));
        return;
    }
    Document doc;
    check(doc.load(path, &err), "it loads back");
    check(doc.elements().size() >= 6 && doc.toolpaths().size() == 4, "every shape and toolpath is there");
    check(std::abs(doc.boardWidth() - 300) < 1e-6 && std::abs(doc.boardHeight() - 200) < 1e-6,
          "stock size kept");
    const Toolpath &outline = doc.toolpaths().last();
    check(outline.type == QLatin1String("contour")
              && outline.json.value("ofset_dir").toInt() == 1
              && outline.json.value("name").toString() == QLatin1String("Outline")
              && std::abs(std::abs(outline.json.value("end_depth").toString().toDouble()) - 12.5) < 1e-6,
          "profile outside, named, at its depth");
    check(notes.join(' ').contains(QStringLiteral("deeper than the stock")), "too deep is noted");
    const GcodeResult g = exportGcode(doc);
    check(g.done.size() == 4 && g.gcode.count(QLatin1String("G1")) > 20, "the document exports");

    QJsonObject bad = d;
    QJsonArray t = bad.value("toolpaths").toArray();
    QJsonObject p = t.at(0).toObject();
    p.insert("tool", 987654);
    t.replace(0, p);
    bad.insert("toolpaths", t);
    check(!padlink::buildDocument(bad, QDir(dir).filePath("bad.c2d"), nullptr, &err)
              && err.contains(QStringLiteral("987654")),
          "an unknown tool is refused, not swapped");
}

// The same design file the Android app's tests check its own JSON against
// (Android/app/src/test/.../DesignJsonTest.kt): what the phone writes builds here.
static void testPhoneFixture(const QString &dir)
{
    QFile f(QStringLiteral(PHOBICCCP_SOURCE_DIR "/tests/data/pad-design.json"));
    check(f.open(QIODevice::ReadOnly), "phone fixture readable");
    const QJsonObject d = QJsonDocument::fromJson(f.readAll()).object();
    check(padlink::check(d).isEmpty(), "phone fixture is a good design");
    QStringList notes;
    QString err;
    const QString path = QDir(dir).filePath("fixture.c2d");
    check(padlink::buildDocument(d, path, &notes, &err), "phone fixture builds");
    Document doc;
    doc.load(path, &err);
    const GcodeResult g = exportGcode(doc);
    check(doc.toolpaths().size() == 4 && g.done.size() == 4, "and all four toolpaths export");
}

static void testStore(const QString &dir, QJsonObject d)
{
    padlink::Store store(QDir(dir).filePath("Pad"));
    check(store.list().isEmpty(), "empty store");
    check(store.put(d), "design stored");
    check(store.get("sign-1").value("name").toString() == QLatin1String("Sign"), "and read back");
    d.insert("name", "Older");
    d.insert("modified", 500.0);
    check(!store.put(d) && store.get("sign-1").value("name").toString() == QLatin1String("Sign"),
          "an older copy does not overwrite");
    d.insert("name", "Newer");
    d.insert("modified", 2000.0);
    check(store.put(d) && store.get("sign-1").value("name").toString() == QLatin1String("Newer"),
          "a newer copy does");
    QJsonObject gone{{"format", "phobicccp-pad"}, {"version", 1}, {"id", "sign-1"},
                     {"modified", 3000.0}, {"deleted", true}};
    check(store.put(gone), "a deletion is kept");
    const QJsonArray l = store.list();
    check(l.size() == 1 && l.at(0).toObject().value("deleted").toBool(), "and listed as deleted");
    QString err;
    check(!store.put(QJsonObject{{"format", "x"}}, &err) && !err.isEmpty(), "junk refused with a reason");
    check(store.get("../../etc/passwd").isEmpty(), "no paths through get");
}

static void testProfiles(const QString &dir)
{
    QSettings s(QDir(dir).filePath("settings.ini"), QSettings::IniFormat);
    s.setValue("machine/size", "s4-xl");
    s.setValue("machine/port", "/dev/ttyACM0");
    s.setValue("machine/bitsetter/x", -30.0);
    s.setValue("machine/logOpen", true);
    using namespace machineprofiles;
    check(list(s).size() == 1 && list(s).first().name == QLatin1String("Machine 1"),
          "one machine to start with");
    const QString first = activeId(s);
    const QString shop = add(s, "Shop");
    check(list(s).size() == 2 && activeId(s) == first, "a machine is added, not switched to");
    check(activate(s, shop), "switch to it");
    check(s.value("machine/size").toString() == QLatin1String("s4-xl")
              && s.value("machine/bitsetter/x").toDouble() == -30.0,
          "a new machine starts with the size and positions");
    check(s.value("machine/port").toString().isEmpty(), "but not the port");
    check(s.value("machine/logOpen").toBool(), "UI choices stay");
    s.setValue("machine/port", "/dev/ttyUSB1");
    s.setValue("machine/size", "s5pro-4x4");
    check(activate(s, first) && s.value("machine/port").toString() == QLatin1String("/dev/ttyACM0")
              && s.value("machine/size").toString() == QLatin1String("s4-xl"),
          "switching back brings the first machine's settings");
    check(activate(s, shop) && s.value("machine/port").toString() == QLatin1String("/dev/ttyUSB1")
              && s.value("machine/size").toString() == QLatin1String("s5pro-4x4"),
          "and the second's are kept");
    check(!remove(s, shop), "the active machine cannot be removed");
    check(rename(s, first, "Garage") && nameOf(s, first) == QLatin1String("Garage"), "rename");
    check(remove(s, first) && list(s).size() == 1, "another can");
    check(!activate(s, first), "a removed machine is gone");
}

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QTemporaryDir tmp;
    QString material;
    const int tool = pickTool("endmill", &material);
    check(tool > 0, "the tool library has a flat end mill for softwood");
    const QJsonObject d = design(tool, material);
    testCheck(d);
    testBuild(tmp.path(), d);
    testPhoneFixture(tmp.path());
    testStore(tmp.path(), d);
    testProfiles(tmp.path());
    std::printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
