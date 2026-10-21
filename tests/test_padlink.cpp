// The PhobiCCCP pad app's side of phobiCCCP: a pad design checked and
// turned into an ordinary document that exports (and back again after an
// edit here), the design store and links that keep every pad in step, and
// machine profiles swapping the machine/ settings.
#include "../src/c2ddocument.h"
#include "../src/gcodeexport.h"
#include "../src/machineprofiles.h"
#include "../src/element.h"
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

// The same design file the pad app's tests check its own JSON against
// (Android/test/model_test.dart): what the pad writes builds here.
static void testPadFixture(const QString &dir)
{
    QFile f(QStringLiteral(PHOBICCCP_SOURCE_DIR "/tests/data/pad-design.json"));
    check(f.open(QIODevice::ReadOnly), "pad fixture readable");
    const QJsonObject d = QJsonDocument::fromJson(f.readAll()).object();
    check(padlink::check(d).isEmpty(), "pad fixture is a good design");
    QStringList notes;
    QString err;
    const QString path = QDir(dir).filePath("fixture.c2d");
    check(padlink::buildDocument(d, path, &notes, &err), "pad fixture builds");
    Document doc;
    doc.load(path, &err);
    const GcodeResult g = exportGcode(doc);
    check(doc.toolpaths().size() == 4 && g.done.size() == 4, "and all four toolpaths export");
}

// A pad's edit rebuilds the linked file, keeping what the pad design does
// not carry: the retract height a Carbide Create file raised (clamps), the
// tiling.
static void testRebuildKeepsRetract(const QString &dir)
{
    QFile f(QStringLiteral(PHOBICCCP_SOURCE_DIR "/tests/data/pad-design.json"));
    f.open(QIODevice::ReadOnly);
    const QJsonObject d = QJsonDocument::fromJson(f.readAll()).object();
    const QString path = QDir(dir).filePath("retract.c2d");
    QString err;
    check(padlink::buildDocument(d, path, nullptr, &err), "retract fixture builds");
    check(Document::writeParam(path, "retract", "10", &err), "retract raised to 10 mm");
    check(Document::writeParam(path, "tiling_enabled", "1", &err)
              && Document::writeParam(path, "tile_height", "300", &err), "tiling set");
    check(padlink::rebuildDocument(d, path, &err), "a pad's edit rebuilds the file");
    Document doc;
    doc.load(path, &err);
    check(doc.params().value("retract") == QLatin1String("10"), "the rebuilt file keeps its retract height");
    check(doc.params().value("tiling_enabled") == QLatin1String("1")
              && doc.params().value("tile_height") == QLatin1String("300"), "and its tiling");
}

// Layers and groups are not something the pad holds: a file with them is not
// the pad's to replace.
static void testLayersAndGroupsLost(const QString &dir)
{
    QFile f(QStringLiteral(PHOBICCCP_SOURCE_DIR "/tests/data/pad-design.json"));
    f.open(QIODevice::ReadOnly);
    const QJsonObject d = QJsonDocument::fromJson(f.readAll()).object();
    const QString path = QDir(dir).filePath("layers.c2d");
    QString err;
    check(padlink::buildDocument(d, path, nullptr, &err), "layers fixture builds");
    QStringList lost;
    padlink::designFromDocument(path, d, &lost, &err);
    check(!lost.contains(QStringLiteral("layers and groups")), "a pad-built file has no layers or groups to lose");
    Document doc;
    doc.load(path, &err);
    QJsonObject raw = doc.elements().first().raw;
    raw.insert("group_id", QJsonArray{"{G1}"});
    doc.replaceElement(Element::fromJson(raw));
    doc.save(path, &err);
    lost.clear();
    padlink::designFromDocument(path, d, &lost, &err);
    check(lost.contains(QStringLiteral("layers and groups")), "a group is reported as lost to the pad");
}

// A design from a pad, saved here, goes back to the pads unchanged; once
// something only phobiCCCP has is added, the pad copy says what it lacks.
static void testBackToPad(const QString &dir)
{
    QFile f(QStringLiteral(PHOBICCCP_SOURCE_DIR "/tests/data/pad-design.json"));
    f.open(QIODevice::ReadOnly);
    const QJsonObject d = QJsonDocument::fromJson(f.readAll()).object();
    const QString path = QDir(dir).filePath("back.c2d");
    QString err;
    check(padlink::buildDocument(d, path, nullptr, &err), "fixture builds for the way back");
    QStringList lost;
    const QJsonObject back = padlink::designFromDocument(path, d, &lost, &err);
    check(padlink::check(back).isEmpty(), "the way back is a good pad design");
    check(lost.isEmpty(), qPrintable(QStringLiteral("nothing lost on the way back (%1)").arg(lost.join(", "))));
    check(back.value("id") == d.value("id") && back.value("name") == d.value("name")
              && back.value("material").toString() == QLatin1String("softwood"),
          "id, name and material kept");
    const QJsonArray shapes = back.value("shapes").toArray();
    check(shapes.size() == 6, "all six shapes come back");
    QStringList types;
    for (const QJsonValue &v : shapes)
        types << v.toObject().value("type").toString();
    check(types == QStringList({"rect", "circle", "polygon", "text", "path", "point"}), "as the same kinds");
    const QJsonObject rect = shapes.at(0).toObject();
    check(std::abs(rect.value("x").toDouble() - 5) < 1e-6 && std::abs(rect.value("w").toDouble() - 290) < 1e-6,
          "a rectangle comes back at its corner");
    const QJsonObject text = shapes.at(3).toObject();
    check(text.value("text").toString() == QLatin1String("PHOBIC") && std::abs(text.value("x").toDouble() - 60) < 1e-6,
          "text comes back with its words and place");
    check(shapes.at(4).toObject().value("points").toArray().size() == 3, "an open path keeps its three points");
    const QJsonArray tps = back.value("toolpaths").toArray();
    check(tps.size() == 4, "all four toolpaths come back");
    const QJsonObject cut = tps.at(3).toObject();
    check(cut.value("type").toString() == QLatin1String("profile") && cut.value("side").toString() == QLatin1String("outside")
              && std::abs(cut.value("depth").toDouble() - 12.5) < 1e-6
              && cut.value("shapes").toArray().size() == 1
              && cut.value("shapes").toArray().at(0) == shapes.at(0).toObject().value("id"),
          "a profile keeps its side, depth and shape");

    // Built again from what came back: still nothing lost.
    const QString again = QDir(dir).filePath("again.c2d");
    check(padlink::buildDocument(back, again, nullptr, &err), "what came back builds");
    padlink::designFromDocument(again, back, &lost, &err);
    check(lost.isEmpty(), "and comes back again with nothing lost");

    // A toolpath turned off on the pad stays off here and comes back off.
    QJsonObject off = d;
    QJsonArray offTps = off.value("toolpaths").toArray();
    QJsonObject first = offTps.at(0).toObject();
    first.insert("enabled", false);
    offTps[0] = first;
    off.insert("toolpaths", offTps);
    const QString offPath = QDir(dir).filePath("off.c2d");
    check(padlink::buildDocument(off, offPath, nullptr, &err), "a turned-off toolpath builds");
    Document offDoc;
    offDoc.load(offPath, &err);
    check(!offDoc.toolpaths().first().json.value("enabled").toBool(true), "and is off in the document");
    const QJsonObject offBack = padlink::designFromDocument(offPath, off, &lost, &err);
    check(lost.isEmpty() && !offBack.value("toolpaths").toArray().at(0).toObject().value("enabled").toBool(true),
          "and comes back off, with nothing lost");

    // A toolpath still waiting for shapes, and a material this library does
    // not know, are not built here, but a save here must not take them off
    // the pads.
    {
        QJsonObject waiting = d;
        QJsonArray tps2 = waiting.value("toolpaths").toArray();
        QJsonObject empty = tps2.at(0).toObject();
        empty.insert("name", "Later");
        empty.insert("shapes", QJsonArray());
        tps2.append(empty);
        waiting.insert("toolpaths", tps2);
        waiting.insert("material", "unobtainium");
        const QString wPath = QDir(dir).filePath("waiting.c2d");
        QStringList notes;
        const bool wBuilt = padlink::buildDocument(waiting, wPath, &notes, &err);
        check(wBuilt && notes.filter("unobtainium").size() == 1 && notes.filter("Later").size() == 1,
              "an empty toolpath and an unknown material are noted");
        const QJsonObject wBack = padlink::designFromDocument(wPath, waiting, &lost, &err);
        const QJsonArray wTps = wBack.value("toolpaths").toArray();
        check(wTps.size() == tps2.size() && wTps.last().toObject().value("name").toString() == QLatin1String("Later"),
              "the toolpath waiting for shapes comes back");
        check(wBack.value("material").toString() == QLatin1String("unobtainium"), "and so does the material");
    }

    // A text scaled evenly here keeps a transform of 1 only within rounding
    // (0.9999999999999999): still plain text, not "turned or curved".
    {
        Document sd;
        sd.load(path, &err);
        for (Element &e : sd.elementsRef()) {
            if (e.geometryType != QLatin1String("text"))
                continue;
            QJsonArray t = e.raw.value("transform").toArray();
            t[0] = 0.9999999999999999;
            t[4] = 1.0000000000000002;
            t[1] = 1e-17;
            e.raw.insert("transform", t);
        }
        const QString sPath = QDir(dir).filePath("scaled-text.c2d");
        sd.save(sPath, &err);
        padlink::designFromDocument(sPath, d, &lost, &err);
        check(lost.isEmpty(), qPrintable(QStringLiteral("a text transform of 1 within rounding is plain (%1)")
                                             .arg(lost.join(", "))));
        Document td;
        td.load(sPath, &err);
        for (Element &e : td.elementsRef()) {
            if (e.geometryType != QLatin1String("text"))
                continue;
            QJsonArray t = e.raw.value("transform").toArray();
            t[0] = 0.98;
            e.raw.insert("transform", t);
        }
        const QString tPath = QDir(dir).filePath("squeezed-text.c2d");
        td.save(tPath, &err);
        padlink::designFromDocument(tPath, d, &lost, &err);
        check(lost.contains(QStringLiteral("turned or curved text")), "a text really squeezed is still named");
    }

    // Edits only phobiCCCP can hold.
    Document doc;
    doc.load(path, &err);
    Toolpath t = doc.toolpaths().first();
    QJsonObject speeds = t.json.value("speeds").toObject();
    speeds.insert("feedrate", speeds.value("feedrate").toDouble() + 111);
    t.json.insert("speeds", speeds);
    doc.replaceToolpath(t);
    QVector<Element> &els = doc.elementsRef();
    els[1] = Element::withConstruction(els[1], true);
    doc.save(path, &err);
    const QJsonObject edited = padlink::designFromDocument(path, d, &lost, &err);
    check(lost.size() == 2, qPrintable(QStringLiteral("own feeds and construction lines are named (%1)").arg(lost.join(", "))));
    check(edited.value("shapes").toArray().size() == 5 && padlink::check(edited).isEmpty(),
          "and the rest still goes to the pad");
}

static void testLinks(const QString &dir)
{
    const QString pad = QDir(dir).filePath("LinksPad");
    const QString file = QDir(dir).filePath("linked.c2d");
    QFile(file).open(QIODevice::WriteOnly);
    {
        padlink::Links l(pad);
        check(l.idFor(file).isEmpty(), "nothing linked to start with");
        l.set(file, "sign-1", true);
    }
    padlink::Links l(pad);
    check(l.idFor(file) == QLatin1String("sign-1") && l.padOnly(file), "a link is kept on disk");
    check(l.pathsFor("sign-1").size() == 1, "and found from the design");
    padlink::Store store(pad);
    check(store.list().isEmpty(), "the links file is not a design");
    check(l.modified(file) == 0, "no design time known for an old link");
    l.set(file, "sign-1", true, 1760850000123.0);
    check(padlink::Links(pad).modified(file) == 1760850000123.0, "the design time the file holds is kept");
    l.set(file, "sign-1", false);
    check(!l.padOnly(file), "marked as holding more than the pad can");
    l.remove(file);
    check(padlink::Links(pad).idFor(file).isEmpty(), "and unlinked");

    // The file's size and time are noted, so an edit made outside
    // phobiCCCP (Carbide Create) is told from one made here.
    l.set(file, "sign-1", true, 1000.0);
    check(!padlink::Links(pad).changedOutside(file), "a file just linked is not changed outside");
    {
        QFile f(file);
        f.open(QIODevice::Append);
        f.write("edited elsewhere");
    }
    check(padlink::Links(pad).changedOutside(file), "a file written since is");
    l.set(file, "sign-1", true, 1000.0);
    check(!padlink::Links(pad).changedOutside(file), "until it is linked again");

    // Links that cannot be read are never written over.
    const QString links = QDir(pad).filePath(".links.json");
    {
        QFile f(links);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write("{\"/some/file.c2d\": {\"id\": \"x\"");   // cut short
    }
    padlink::Links broken(pad);
    check(!broken.ok() && !broken.error().isEmpty(), "damaged links are noticed");
    check(!broken.set(file, "sign-1", true) && !broken.remove(file), "and refuse changes");
    QFile f(links);
    f.open(QIODevice::ReadOnly);
    check(f.readAll().endsWith("\"x\""), "and the file is left as it was");
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

// A pad's put carries the time of the copy it edited ("base"): a stored copy
// changed after that is not overwritten by the pad's edit.
static void testConflicts(const QString &dir, QJsonObject d)
{
    padlink::Store store(QDir(dir).filePath("ConflictPad"));
    const double now = 1760850000000.0;
    d.insert("name", "Sign");
    d.insert("modified", 1000.0);
    check(store.put(d, nullptr, nullptr, now), "first copy stored");

    // The pad edited the copy it has: an ordinary newer copy.
    QJsonObject pad = d;
    pad.insert("modified", 2000.0);
    pad.insert("base", 1000.0);
    padlink::Store::PutOutcome out;
    check(store.put(pad, nullptr, &out, now) && out.forked.isEmpty() && !out.conflict,
          "an edit of the stored copy replaces it");
    check(!store.get("sign-1").contains("base"), "base is not stored");

    // Changed here (3000) after the pad's copy (2000), then the pad's edit
    // of 2000 arrives stamped newer: kept as a design of its own.
    d.insert("modified", 3000.0);
    d.insert("name", "Desk edit");
    check(store.put(d, nullptr, nullptr, now), "an edit made here");
    pad.insert("modified", 4000.0);
    pad.insert("base", 2000.0);
    pad.insert("name", "Pad edit");
    out = {};
    check(store.put(pad, nullptr, &out, now) && !out.forked.isEmpty() && padlink::validId(out.forked),
          "a pad edit made meanwhile is forked");
    check(store.get("sign-1").value("name").toString() == QLatin1String("Desk edit"),
          "and the edit made here stays");
    const QJsonObject fork = store.get(out.forked);
    check(fork.value("name").toString() == QLatin1String("Pad edit (pad)")
              && fork.value("modified").toDouble() == 4000.0 && !fork.contains("base"),
          "the fork is the pad's edit, named as from the pad");
    check(store.list().size() == 2, "both are listed");

    // Even when the pad's edit is the older one, it is not lost.
    pad.insert("modified", 2500.0);
    out = {};
    check(store.put(pad, nullptr, &out, now) && !out.forked.isEmpty(), "an older concurrent edit is forked too");

    // A deletion of a copy changed meanwhile is refused.
    QJsonObject gone{{"format", "phobicccp-pad"}, {"version", 1}, {"id", "sign-1"},
                     {"modified", 5000.0}, {"deleted", true}, {"base", 2000.0}};
    out = {};
    check(!store.put(gone, nullptr, &out, now) && out.conflict && out.forked.isEmpty()
              && !store.get("sign-1").value("deleted").toBool(),
          "a deletion over a newer edit is refused");

    // Another computer that is behind (its copy older than the pad's base)
    // just takes the pad's edit; so does one without a base (an older pad).
    pad.insert("modified", 6000.0);
    pad.insert("base", 5500.0);
    pad.insert("name", "From a pad that synced elsewhere");
    out = {};
    check(store.put(pad, nullptr, &out, now) && out.forked.isEmpty(), "a computer behind the pad's base takes it");
    QJsonObject old = pad;
    old.remove("base");
    old.insert("modified", 7000.0);
    out = {};
    check(store.put(old, nullptr, &out, now) && out.forked.isEmpty(), "an old pad's put works as before");
    // The same copy again: nothing to do, no conflict.
    out = {};
    check(!store.put(old, nullptr, &out, now) && out.forked.isEmpty() && !out.conflict,
          "the copy it already has is not stored again");
    QJsonObject same = old;
    same.insert("base", 6000.0);
    check(!store.put(same, nullptr, &out, now) && out.forked.isEmpty() && !out.conflict,
          "nor with a base older than it");

    // Times from a clock gone wrong.
    QJsonObject far = d;
    far.insert("modified", now + 2 * padlink::kMaxAheadMs);
    QString err;
    check(!store.put(far, &err, nullptr, now) && !err.isEmpty(), "a time years ahead is refused");
    check(padlink::plausibleStamp(now + 1000, now) && !padlink::plausibleStamp(1e15, now),
          "plausible times");
    check(padlink::nextStamp(1000, now) == now, "an edit is stamped with the clock");
    const double after = padlink::nextStamp(now + 10, now);
    check(after > now + 10 && after <= now + 1011, "or just after a copy from a clock ahead");
    check(padlink::nextStamp(1e15, now) == now, "but not after an absurd one");
}

// A toolpath whose shapes were deleted here comes back once, however often
// the file is saved (bug check 4, item 3).
static void testDeletedShapesToolpath(const QString &dir, QJsonObject d)
{
    QJsonArray tps = d.value("toolpaths").toArray();
    QJsonObject pocket = tps.at(0).toObject();
    pocket.insert("shapes", QJsonArray{"c"});
    tps.replace(0, pocket);
    d.insert("toolpaths", tps);
    const QString path = QDir(dir).filePath("deleted-shapes.c2d");
    QString err;
    check(padlink::buildDocument(d, path, nullptr, &err), "a design with a pocket on the circle builds");
    Document doc;
    doc.load(path);
    QString circle;
    for (const Element &e : doc.elements())
        if (e.geometryType == QLatin1String("circle"))
            circle = e.id;
    check(!circle.isEmpty() && doc.removeElementById(circle) && doc.save(path, &err), "the circle deleted here");
    QJsonObject base = d;
    int pockets = 0;
    for (int i = 0; i < 4; ++i) {
        QStringList lost;
        const QJsonObject out = padlink::designFromDocument(path, base, &lost);
        pockets = 0;
        for (const QJsonValue &v : out.value("toolpaths").toArray())
            pockets += v.toObject().value("name").toString() == QLatin1String("Pocket");
        if (pockets != 1)
            break;
        base = out;   // what the save shares, and gets back as the base next time
    }
    check(pockets == 1, "a toolpath without shapes is not added again on every save");
}

// The same edit put again after its answer was lost lands on the fork already
// made (item 6); a stored copy this build cannot read is not written over (item 8).
static void testForkRetryAndUnreadable(const QString &dir, QJsonObject d)
{
    const double now = 1760850000000.0;
    padlink::Store store(QDir(dir).filePath("RetryPad"));
    d.insert("modified", 1000.0);
    store.put(d, nullptr, nullptr, now);
    QJsonObject desk = d;
    desk.insert("modified", 2000.0);
    desk.insert("name", "Desk edit");
    store.put(desk, nullptr, nullptr, now);
    QJsonObject pad = d;
    pad.insert("modified", 3000.0);
    pad.insert("base", 1000.0);
    pad.insert("name", "Pad edit");
    padlink::Store::PutOutcome o1, o2;
    check(store.put(pad, nullptr, &o1, now) && !o1.forked.isEmpty(), "a pad edit made meanwhile is forked");
    check(store.put(pad, nullptr, &o2, now) && o2.forked == o1.forked, "put again, it answers the same fork");
    int forks = 0;
    for (const QJsonValue &v : store.list())
        forks += v.toObject().value("name").toString().endsWith(QLatin1String("(pad)"));
    check(forks == 1, "and no second fork is made");

    // Written by a newer phobiCCCP: version 2.
    QJsonObject newer = d;
    newer.insert("id", "newer-1");
    newer.insert("version", 2);
    const QString file = QDir(store.dir()).filePath("newer-1.json");
    QFile f(file);
    f.open(QIODevice::WriteOnly);
    const QByteArray bytes = QJsonDocument(newer).toJson();
    f.write(bytes);
    f.close();
    QJsonObject over = d;
    over.insert("id", "newer-1");
    over.insert("modified", 9000.0);
    QString err;
    check(!store.put(over, &err, nullptr, now) && !err.isEmpty(), "a put over a copy that cannot be read is refused");
    f.open(QIODevice::ReadOnly);
    check(f.readAll() == bytes, "and that copy is left as it is");
}

// A save of one linked file reaches the design's other linked files, as a pad
// edit does (round 2): left as they were, they held the old design, and a save
// of one of them later forked it.
static void testFollowDesign(const QString &dir, QJsonObject d)
{
    const QString pad = QDir(dir).filePath("FollowPad");
    const QString saved = QDir(dir).filePath("follow-saved.c2d"), other = QDir(dir).filePath("follow-other.c2d"),
                  lossy = QDir(dir).filePath("follow-lossy.c2d"), open = QDir(dir).filePath("follow-open.c2d");
    d.insert("id", "follow-1");
    padlink::Links links(pad);
    for (const QString &f : {saved, other, lossy, open}) {
        padlink::buildDocument(d, f, nullptr, nullptr);
        links.set(f, "follow-1", f != lossy, 1000);
    }
    QJsonArray shapes = d.value("shapes").toArray();
    shapes.append(QJsonObject{{"id", "new"}, {"type", "point"}, {"x", 33}, {"y", 44}});
    d.insert("shapes", shapes);
    d.insert("modified", 2000.0);
    const QStringList rebuilt = padlink::followDesign(links, d, saved, [&](const QString &p) {
        return QFileInfo(p) == QFileInfo(open);
    });
    check(rebuilt.size() == 1 && QFileInfo(rebuilt.value(0)) == QFileInfo(other),
          "a save reaches the other file linked to the design, and only that one");
    auto points = [](const QString &f) {
        QStringList lost;
        int n = 0;
        for (const QJsonValue &v : padlink::designFromDocument(f, QJsonObject{}, &lost).value("shapes").toArray())
            n += v.toObject().value("type").toString() == QLatin1String("point");
        return n;
    };
    check(points(other) == 2 && padlink::Links(pad).modified(other) == 2000 && !padlink::Links(pad).changedOutside(other),
          "it holds the change, linked as holding that copy");
    check(points(lossy) == 1 && points(open) == 1 && padlink::Links(pad).modified(open) == 1000,
          "one holding what a pad cannot, and an open one with changes, are left alone");
}

// Version vectors (round 2): pads and computers that never talk to each other
// directly. A copy made from another replaces it; two edited apart are both
// kept the same way on every computer, whichever arrives first.
static void testVersions(const QString &dir, QJsonObject d)
{
    const double now = 1760850000000.0;
    auto copy = [&](double modified, const QJsonObject &vv, const char *name, bool deleted = false) {
        QJsonObject c = d;
        c.insert("modified", modified);
        c.insert("vv", vv);
        c.insert("name", QLatin1String(name));
        if (deleted)
            c = QJsonObject{{"format", "phobicccp-pad"}, {"version", 1}, {"id", "sign-1"},
                            {"modified", modified}, {"deleted", true}, {"vv", vv}};
        return c;
    };
    const QJsonObject a = copy(1000, {{"pad1", 1000}}, "A");
    const QJsonObject b = copy(2000, {{"pad1", 1000}, {"pad2", 2000}}, "B");    // made from A
    const QJsonObject c = copy(3000, {{"pad1", 3000}}, "C");                   // also from A, apart from B
    const QJsonObject e = copy(2500, {{"pad1", 1000}, {"desk", 2500}}, "E");   // from A, apart from both
    using padlink::Order;
    check(padlink::order(a, a) == Order::Same && padlink::order(b, a) == Order::After
              && padlink::order(a, b) == Order::Before && padlink::order(b, c) == Order::Concurrent,
          "versions: same, made from, what it was made from, edited apart");
    QJsonObject stale = b;
    stale.insert("modified", 2600.0);   // an older phobiCCCP stamped it again, vv as it was
    check(padlink::versionVector(stale).isEmpty(), "a vv whose newest time is not the copy's is not believed");
    // But it is the vv of the copy the older one edited: made from B, and
    // apart from C (an entry above it), so C does not simply replace it.
    check(padlink::order(stale, b) == Order::After && padlink::order(b, stale) == Order::Before
              && padlink::order(stale, c) == Order::Concurrent && padlink::order(c, stale) == Order::Concurrent,
          "an older pad's edit stands to a copy with a vv by the vv it kept");
    QJsonObject none = a;
    none.remove("vv");
    check(padlink::order(none, a) == Order::Same && padlink::order(none, b) == Order::Concurrent,
          "a copy with none (an older pad's design) has nothing of the newer ones in it");
    QJsonObject noneGone = none;
    noneGone.insert("deleted", true);
    noneGone.insert("modified", 1500.0);
    check(padlink::order(noneGone, b) == Order::Unknown && padlink::order(none, stale) == Order::Unknown,
          "an older pad's deletion, or two older copies, tell nothing: base and newer time");

    // Made from the copy kept: replaces it, whatever its base says.
    padlink::Store s1(QDir(dir).filePath("VersionsPad1"));
    padlink::Store::PutOutcome out;
    check(s1.put(a, nullptr, &out, now), "first copy stored");
    QJsonObject bb = b;
    bb.insert("base", 1.0);
    check(s1.put(bb, nullptr, &out, now) && out.forked.isEmpty() && !out.moved,
          "a copy made from the one kept replaces it");
    check(!s1.put(a, nullptr, &out, now) && out.forked.isEmpty() && !out.conflict,
          "and the one it was made from does not come back");
    check(s1.list().at(0).toObject().value("vv").toObject().value("pad2").toDouble() == 2000,
          "the list says each copy's versions");

    // Edited apart: the later edit (C) keeps the id, B becomes a design of its own.
    out = {};
    check(s1.put(c, nullptr, &out, now) && out.moved && !out.forked.isEmpty(),
          "a later edit made apart takes the id, and the copy here moves to a fork");
    check(s1.get("sign-1").value("name").toString() == QLatin1String("C")
              && s1.get(out.forked).value("name").toString() == QLatin1String("B (conflict)")
              && s1.get(out.forked).value("modified").toDouble() == 2000,
          "the fork is B, as it was");
    const QString forkB = out.forked;
    out = {};
    check(s1.put(e, nullptr, &out, now) && !out.moved && !out.forked.isEmpty()
              && s1.get("sign-1").value("name").toString() == QLatin1String("C"),
          "an earlier edit made apart is the one forked");
    const QString forkE = out.forked;
    out = {};
    check(s1.put(e, nullptr, &out, now) && out.forked == forkE && s1.list().size() == 3,
          "put again, it answers the same fork");

    // Another computer gets the same copies the other way round: the same ids, the same copies.
    padlink::Store s2(QDir(dir).filePath("VersionsPad2"));
    for (const QJsonObject &x : {e, c, a, b})
        s2.put(x, nullptr, nullptr, now);
    bool same = s2.list().size() == 3;
    for (const QString &id : {QStringLiteral("sign-1"), forkB, forkE})
        same = same && s1.get(id) == s2.get(id) && !s1.get(id).isEmpty();
    check(same, "every computer settles the same copies the same way");
    // Its forks, brought by a pad, are copies it already has.
    out = {};
    check(!s2.put(s1.get(forkB), nullptr, &out, now) && out.forked.isEmpty(), "a fork brought back is the same copy");

    // A deletion made apart: the later of the two wins; an edit that loses is kept as a fork.
    padlink::Store s3(QDir(dir).filePath("VersionsPad3"));
    s3.put(b, nullptr, nullptr, now);
    out = {};
    check(!s3.put(copy(1500, {{"pad1", 1500}}, "", true), nullptr, &out, now) && out.conflict
              && !s3.get("sign-1").value("deleted").toBool(),
          "an earlier deletion made apart is refused");
    out = {};
    check(s3.put(copy(4000, {{"pad1", 4000}}, "", true), nullptr, &out, now) && out.moved
              && s3.get("sign-1").value("deleted").toBool()
              && s3.get(out.forked).value("name").toString() == QLatin1String("B (conflict)"),
          "a later one is kept, and the edit it would have dropped is a design of its own");

    // An edit here: this computer's own entry, the same id every time.
    padlink::Store s4(QDir(dir).filePath("VersionsPad4"));
    const QString me = s4.replicaId();
    check(padlink::validId(me) && padlink::Store(s4.dir()).replicaId() == me, "a computer keeps its own id");
    {
        // Its designs copied elsewhere (Documents copied to another computer
        // or synced there): the copy gets an id of its own, or the two would
        // take each other's edits as made from their own.
        const QString copied = QDir(dir).filePath("VersionsPad4Copy");
        QDir().mkpath(copied);
        QFile::copy(QDir(s4.dir()).filePath(".replica"), QDir(copied).filePath(".replica"));
        const QString other = padlink::Store(copied).replicaId();
        check(padlink::validId(other) && other != me && padlink::Store(copied).replicaId() == other
                  && padlink::Store(s4.dir()).replicaId() == me,
              "a copied id is made again where it was copied to, and kept there");
        // Kept apart from the designs: an id an older phobiCCCP left beside
        // them is not taken over (it may be such a copy), and goes.
        const QString designs = QDir(dir).filePath("VersionsDocs"), local = QDir(dir).filePath("VersionsLocal");
        QDir().mkpath(designs);
        QFile legacy(QDir(designs).filePath(".replica"));
        legacy.open(QIODevice::WriteOnly);
        legacy.write("older-id");
        legacy.close();
        const QString fresh = padlink::Store(designs, local).replicaId();
        check(fresh != QLatin1String("older-id") && padlink::validId(fresh) && !legacy.exists()
                  && QFile::exists(QDir(local).filePath(".replica"))
                  && padlink::Store(designs, local).replicaId() == fresh,
              "an id kept beside the designs moves apart from them, made again");
    }
    const QJsonObject mine = s4.edited(b, 5000);
    check(mine.value("vv").toObject() == QJsonObject({{"pad1", 1000}, {"pad2", 2000}, {me, 5000}})
              && padlink::order(mine, b) == Order::After,
          "an edit here is made from the copy it edits");
    const QJsonObject onStale = s4.edited(stale, 5000);
    const QString olderEdit = padlink::olderEditId(stale);
    check(onStale.value("vv").toObject()
                  == QJsonObject({{"pad1", 1000}, {"pad2", 2000}, {olderEdit, 2600}, {me, 5000}})
              && padlink::order(onStale, stale) == Order::After && padlink::order(onStale, b) == Order::After,
          "an edit here of an older pad's copy keeps its vv and notes that edit, so it is made from it");
    QJsonObject staleToo = stale;
    staleToo.insert("modified", 2700.0);   // the older pad edited the same copy again, apart
    check(padlink::order(onStale, staleToo) == Order::Concurrent, "and is apart from another edit of it");
    QJsonObject fresh = a;
    fresh.remove("vv");
    fresh.insert("modified", 0);   // a design of its own made here (padShareSaved)
    check(s4.edited(fresh, 5000).value("vv").toObject() == QJsonObject({{me, 5000}}),
          "a design new here starts its own versions");
    {
        // At the limit: the oldest older-edit entry makes room, the vv stays believed.
        QJsonObject full = a;
        QJsonObject vv;
        for (int i = 1; i <= 64; ++i)
            vv.insert(QStringLiteral("e-%1").arg(i, 16, 16, QLatin1Char('0')), 1000.0 + i);
        full.insert("vv", vv);
        full.insert("modified", 1064.0);
        const QJsonObject capped = s4.edited(full, 5000);
        const QJsonObject cv = capped.value("vv").toObject();
        check(cv.size() == 64 && cv.value(me).toDouble() == 5000
                  && !cv.contains(QStringLiteral("e-%1").arg(1, 16, 16, QLatin1Char('0')))
                  && !padlink::versionVector(capped).isEmpty(),
              "64 versions at most: the oldest older-edit entry goes");
        check(padlink::order(capped, full) == Order::After && padlink::order(full, capped) == Order::Before,
              "and the capped copy is still made from the one it edited");
    }
    // The same on the pad (Android/lib/model.dart olderEditId, forkIdFor).
    check(olderEdit == QLatin1String("e-e625dcdd1333364a"), "the older edit's id is the pad's");
    check(padlink::Store::forkIdFor(c) == QLatin1String("2b4050ca-c995-58c3-b7c9-374462673dd3"), "a fork's id is the pad's");
    padlink::Store s5(QDir(dir).filePath("VersionsOld"));
    s5.put(b, nullptr, nullptr, now);
    out = {};
    check(s5.put(c, nullptr, &out, now) && out.moved, "set up: C took the id");
    out = {};
    check(s5.put(stale, nullptr, &out, now) && !out.forked.isEmpty() && !out.moved
              && s5.get(out.forked).value("modified").toDouble() == 2600,
          "an older pad's edit made apart from the copy kept is kept beside it, not dropped");
    // That design of its own deleted since, the same copy sent again: the
    // put answers the fork, now a deletion. padSend then opens the pad's copy
    // on its own (not the deletion, which cannot be built) and does not bring
    // the fork back for every pad.
    const QString staleFork = out.forked;
    QJsonObject forkGone = s5.get(staleFork);
    forkGone = s5.edited(QJsonObject{{"format", "phobicccp-pad"}, {"version", 1}, {"id", staleFork},
                                      {"name", forkGone.value("name")}, {"modified", 2600.0}, {"deleted", true},
                                      {"vv", forkGone.value("vv")}},
                         4000);
    check(s5.put(forkGone, nullptr, nullptr, now) && s5.get(staleFork).value("deleted").toBool(), "set up: fork deleted");
    out = {};
    check(s5.put(stale, nullptr, &out, now) && out.forked == staleFork && !out.moved
              && s5.get(staleFork).value("deleted").toBool() && !padlink::buildDocument(s5.get(staleFork), QDir(dir).filePath("gone.c2d"), nullptr, nullptr)
              && padlink::buildDocument(stale, QDir(dir).filePath("sent-again.c2d"), nullptr, nullptr),
          "sent again after its fork was deleted: the fork is a deletion, the pad's copy still builds");

    // The files holding a copy that moved to a fork follow it.
    const QString pad = QDir(dir).filePath("VersionsLinks");
    const QString f1 = QDir(dir).filePath("holds-b.c2d"), f2 = QDir(dir).filePath("older.c2d");
    QFile(f1).open(QIODevice::WriteOnly);
    QFile(f2).open(QIODevice::WriteOnly);
    padlink::Links l(pad);
    l.set(f1, "sign-1", false, 2000);
    l.set(f2, "sign-1", true, 1000);
    check(l.moveTo("sign-1", 2000, forkB) == 1, "one file held the copy that moved");
    padlink::Links back(pad);
    check(back.idFor(f1) == forkB && !back.padOnly(f1) && back.modified(f1) == 2000
              && back.idFor(f2) == QLatin1String("sign-1"),
          "it is linked to the fork, as it was otherwise; the older file stays");
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

// File > Send to pad…: pads in touch are listed, and a design sent to one
// waits for its next "info" until it says it opened it.
// A profile on a bit other than #201, built and read back unchanged, lost
// nothing (the lead-in was sized from another bit, so it looked set here).
static void testOtherBitRoundTrip(const QString &dir)
{
    for (int tool : {201, 102, 602}) {
        const QJsonObject d{{"format", "phobicccp-pad"}, {"version", 1}, {"id", "sign"}, {"name", "Sign"},
                            {"modified", 1000.0}, {"material", "softwood"},
                            {"stock", QJsonObject{{"width", 300}, {"height", 200}, {"thickness", 12}}},
                            {"shapes", QJsonArray{QJsonObject{{"id", "r"}, {"type", "rect"}, {"x", 10}, {"y", 10}, {"w", 100}, {"h", 50}}}},
                            {"toolpaths", QJsonArray{QJsonObject{{"name", "Cut"}, {"type", "profile"}, {"side", "outside"},
                                                                 {"shapes", QJsonArray{"r"}}, {"tool", tool}, {"depth", 12.5}},
                                                     QJsonObject{{"name", "Pocket"}, {"type", "pocket"},
                                                                 {"shapes", QJsonArray{"r"}}, {"tool", tool}, {"depth", 3}}}}};
        const QString path = QDir(dir).filePath(QStringLiteral("bit%1.c2d").arg(tool));
        QString err;
        check(padlink::buildDocument(d, path, nullptr, &err), "other bit: built");
        QStringList lost;
        padlink::designFromDocument(path, d, &lost, &err);
        check(lost.isEmpty(), "other bit: nothing lost on the way back");
        if (tool == 201)
            continue;
        // A file built before the sizes followed the bit: #201's lead-in and
        // rest, as the pad built them, are not settings made here; a lead-in
        // typed here still is.
        for (double lead : {201.0, 7.0}) {
            Document doc;
            doc.load(path, &err);
            for (Toolpath t : doc.toolpaths()) {
                if (t.json.contains("lead_radius"))
                    t.json.insert("lead_radius", lead == 201.0 ? 3.175 : lead);
                if (t.json.contains("rest_diameter"))
                    t.json.insert("rest_diameter", 6.35);
                doc.replaceToolpath(t);
            }
            doc.save(path, &err);
            lost.clear();
            padlink::designFromDocument(path, d, &lost, &err);
            check(lead == 201.0 ? lost.isEmpty() : !lost.isEmpty(),
                  lead == 201.0 ? "other bit: an older file's #201 sizes are not settings made here"
                                : "other bit: a lead-in typed here still is");
        }
    }
}

static void testPads()
{
    padlink::Pads pads;
    const qint64 t = 1700000000000;
    check(!pads.seen(QJsonObject(), QStringLiteral("10.0.0.5"), t), "an info without a pad is no pad");
    check(!pads.seen(QJsonObject{{QStringLiteral("id"), QStringLiteral("bad id/..")}}, QStringLiteral("10.0.0.5"), t),
          "nor is one with a malformed id");
    check(pads.seen(QJsonObject{{QStringLiteral("id"), QStringLiteral("p1")}, {QStringLiteral("name"), QStringLiteral("Tab")}},
                    QStringLiteral("10.0.0.5"), t), "a pad is noted");
    pads.seen(QJsonObject{{QStringLiteral("id"), QStringLiteral("p2")}}, QStringLiteral("10.0.0.6"), t + 1000);
    auto recent = pads.recent(t + 1000);
    check(recent.size() == 2 && recent.first().id == QLatin1String("p2"), "the newest pad first");
    check(recent.last().name == QLatin1String("Tab") && recent.first().name == QLatin1String("10.0.0.6"),
          "named as it says, else by its address");
    check(pads.recent(t + padlink::Pads::kRecentMs + 500).size() == 1, "a pad quiet for long is not listed");

    check(pads.pendingFor(QStringLiteral("p1"), t).isEmpty(), "nothing waits for a pad at first");
    const qint64 a = pads.sendOpen(QStringLiteral("p1"), QStringLiteral("d1"), QStringLiteral("Sign"),
                                   QStringLiteral("shop"), t + 2000);
    const qint64 b = pads.sendOpen(QStringLiteral("p1"), QStringLiteral("d2"), QStringLiteral("Box"),
                                   QStringLiteral("shop"), t + 2000);
    check(a > 0 && b > a, "each send is numbered after the one before");
    const QJsonObject open = pads.pendingFor(QStringLiteral("p1"), t + 3000);
    check(open.value(QStringLiteral("design")).toString() == QLatin1String("d2")
              && qint64(open.value(QStringLiteral("seq")).toDouble()) == b,
          "the pad gets the design sent it last");
    check(pads.pendingFor(QStringLiteral("p2"), t + 3000).isEmpty(), "and only that pad");
    check(!pads.opened(QStringLiteral("p1"), a), "opening an older send does not count");
    QString name;
    check(pads.waiting(QStringLiteral("p1"), b) && pads.opened(QStringLiteral("p1"), b, &name) && name == QLatin1String("Box"),
          "the pad says it opened it");
    check(!pads.waiting(QStringLiteral("p1"), b) && pads.pendingFor(QStringLiteral("p1"), t + 3000).isEmpty(),
          "then nothing waits any more");
    pads.sendOpen(QStringLiteral("p2"), QStringLiteral("d1"), QStringLiteral("Sign"), QStringLiteral("shop"), t);
    check(pads.pendingFor(QStringLiteral("p2"), t + padlink::Pads::kOpenMs + 1).isEmpty(), "a send not taken is given up");
    for (int i = 0; i < 200; ++i)
        pads.seen(QJsonObject{{QStringLiteral("id"), QStringLiteral("x%1").arg(i)}}, QStringLiteral("10.0.1.1"), t + 5000 + i);
    check(pads.recent(t + 5300).size() == 64, "only so many pads are kept");
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
    testPadFixture(tmp.path());
    testBackToPad(tmp.path());
    testRebuildKeepsRetract(tmp.path());
    testLayersAndGroupsLost(tmp.path());
    testLinks(tmp.path());
    testStore(tmp.path(), d);
    testConflicts(tmp.path(), d);
    testDeletedShapesToolpath(tmp.path(), d);
    testForkRetryAndUnreadable(tmp.path(), d);
    testVersions(tmp.path(), d);
    testFollowDesign(tmp.path(), d);
    testProfiles(tmp.path());
    testOtherBitRoundTrip(tmp.path());
    testPads();
    std::printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
