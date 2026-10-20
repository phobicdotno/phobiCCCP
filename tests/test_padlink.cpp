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
    testLinks(tmp.path());
    testStore(tmp.path(), d);
    testConflicts(tmp.path(), d);
    testDeletedShapesToolpath(tmp.path(), d);
    testForkRetryAndUnreadable(tmp.path(), d);
    testProfiles(tmp.path());
    testPads();
    std::printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
