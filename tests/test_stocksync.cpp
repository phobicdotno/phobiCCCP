// Bit stock and Collections shared between copies of the app (StockSync):
// a fresh install's first edit pools its Collections with a peer's instead of
// replacing them, collections merge one by one (edits and deletions), an older
// copy's whole-set packets still work, only bits the tool library knows are
// taken, and no packet is bigger than a datagram may be.
#include "../src/stocksync.h"
#include "../src/stockstamp.h"
#include "../src/toolcollections.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <cstdio>

static int g_fail = 0;
static void check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++g_fail;
}

static c2d::ToolCollections collectionsIn(QSettings &s)
{
    c2d::ToolCollections c;
    c2d::ToolCollections::fromJson(s.value(QStringLiteral("tools/collections")).toByteArray(), &c);
    return c;
}

static c2d::ToolCollections collectionsIn(const QString &ini)
{
    QSettings s(ini, QSettings::IniFormat);
    return collectionsIn(s);
}

static bool has(const c2d::ToolCollections &c, const QString &name, std::initializer_list<int> bits)
{
    const c2d::ToolCollection *x = c.byName(name);
    if (!x)
        return false;
    for (int b : bits)
        if (!x->tools.contains(b))
            return false;
    return true;
}

static void putIni(const QString &ini, const QByteArray &json, qint64 ts, const QByteArray &stamps = {})
{
    QSettings s(ini, QSettings::IniFormat);
    s.clear();
    s.setValue(QStringLiteral("tools/collections"), json);
    s.setValue(QStringLiteral("tools/collectionsTs"), ts);
    if (!stamps.isEmpty())
        s.setValue(QStringLiteral("tools/collectionsStamps"), stamps);
    s.sync();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("phobicccp-test"));
    QCoreApplication::setApplicationName(QStringLiteral("test_stocksync"));
    QTemporaryDir tmp;
    qputenv("PHOBIC_TOOLLIB_CACHE", (tmp.path() + QStringLiteral("/cache")).toLocal8Bit());
    QSettings().clear();

    // 1. A fresh install edits its Collections once, then meets a copy with a
    //    rich set: nothing of the rich set is lost on either side.
    {
        const QString rich = tmp.path() + QStringLiteral("/rich.ini");
        putIni(rich, "{\"collections\":[{\"name\":\"My Tools\",\"tools\":[201,102,1203]},"
                     "{\"name\":\"Roughing\",\"tools\":[251]}]}", 1000);
        c2d::StockSync R(rich, 0), F;           // F: the app's own settings, fresh

        c2d::ToolCollections mine = c2d::ToolCollections::loadSettings();
        mine.addTool(QStringLiteral("My Tools"), 112);
        mine.saveSettings();
        check(QSettings().value(QStringLiteral("tools/collectionsFresh")).toBool(),
              "a first edit with no Collections before marks the copy fresh");

        const QJsonObject fcoll = QJsonDocument::fromJson(F.payload()).object()
                                      .value(QStringLiteral("coll")).toObject();
        check(fcoll.value(QStringLiteral("fresh")).toBool() && !fcoll.contains(QStringLiteral("ts")),
              "a fresh copy says so and sends no whole-set stamp an older copy would take");

        check(R.merge(F.payload()), "the rich copy takes the fresh copy's bit");
        c2d::ToolCollections r = collectionsIn(rich);
        check(has(r, QStringLiteral("My Tools"), {201, 102, 1203, 112}),
              "rich: My Tools keeps its bits and gains the fresh one");
        check(has(r, QStringLiteral("Roughing"), {251}), "rich: Roughing is still there");
        check(!r.byName(QStringLiteral("Unused")), "rich: the fresh copy's untouched empty default is not added");

        check(F.merge(R.payload()), "the fresh copy takes the rich set");
        QSettings fs;
        c2d::ToolCollections f = collectionsIn(fs);
        check(has(f, QStringLiteral("My Tools"), {201, 102, 1203, 112}), "fresh: My Tools pooled");
        check(has(f, QStringLiteral("Roughing"), {251}), "fresh: Roughing came over");
        check(!fs.value(QStringLiteral("tools/collectionsFresh")).toBool(),
              "after meeting a copy with history the copy is fresh no more");

        check(!R.merge(F.payload()) && !F.merge(R.payload()), "the two agree: nothing more changes");

        // From now on an edit here is an ordinary edit: removing a bit goes over.
        c2d::ToolCollections again = c2d::ToolCollections::loadSettings();
        again.removeTool(QStringLiteral("Roughing"), 251);
        again.saveSettings();
        check(R.merge(F.payload()), "the rich copy takes a later edit");
        const c2d::ToolCollections r2 = collectionsIn(rich);
        const c2d::ToolCollection *rough = r2.byName(QStringLiteral("Roughing"));
        check(rough && !rough->tools.contains(251), "a later edit on the once-fresh copy wins as the newest");
        check(has(collectionsIn(rich), QStringLiteral("My Tools"), {201, 102, 1203, 112}),
              "and leaves the other collections as they were");
        QSettings().clear();
    }

    // 2. Per-collection merge: each side keeps its newer collections, and a
    //    deletion goes over to a copy that had not touched it since.
    {
        const QString a = tmp.path() + QStringLiteral("/a.ini"), b = tmp.path() + QStringLiteral("/b.ini");
        putIni(a, "{\"collections\":[{\"name\":\"Mine\",\"tools\":[201]},{\"name\":\"Shared\",\"tools\":[102]}]}",
               5000, "{\"ts\":{\"Mine\":5000,\"Shared\":1000},\"gone\":{}}");
        putIni(b, "{\"collections\":[{\"name\":\"Shared\",\"tools\":[102,251]},{\"name\":\"Other\",\"tools\":[112]}]}",
               3000, "{\"ts\":{\"Shared\":3000,\"Other\":2000},\"gone\":{\"Mine\":4000}}");
        c2d::StockSync A(a, 0), B(b, 0);
        check(B.merge(A.payload()), "B merges A");
        c2d::ToolCollections cb = collectionsIn(b);
        check(has(cb, QStringLiteral("Mine"), {201}), "B: A's Mine, edited after B deleted it, comes back");
        check(has(cb, QStringLiteral("Shared"), {102, 251}), "B: its newer Shared stays");
        check(has(cb, QStringLiteral("Other"), {112}), "B: its own Other stays (the whole set no longer replaces)");
        check(A.merge(B.payload()), "A merges B");
        c2d::ToolCollections ca = collectionsIn(a);
        check(has(ca, QStringLiteral("Shared"), {102, 251}) && has(ca, QStringLiteral("Other"), {112})
                  && has(ca, QStringLiteral("Mine"), {201}),
              "A: B's newer Shared and its Other, and its own Mine");

        putIni(b, "{\"collections\":[{\"name\":\"Shared\",\"tools\":[102,251]},{\"name\":\"Mine\",\"tools\":[201]}]}",
               6000, "{\"ts\":{\"Shared\":3000,\"Mine\":5000},\"gone\":{\"Other\":6000}}");
        check(A.merge(B.payload()) && !collectionsIn(a).byName(QStringLiteral("Other")),
              "a collection deleted on B goes on A");
        check(!A.merge(B.payload()), "and the same packet again changes nothing");

        // An older copy sends its Collections as one: newer replaces the set,
        // older is ignored, as before.
        const auto oldPacket = [](qint64 ts, const QByteArray &json) {
            QJsonObject coll{{QStringLiteral("ts"), double(ts)}, {QStringLiteral("json"), QString::fromUtf8(json)}};
            return QJsonDocument(QJsonObject{{QStringLiteral("app"), QStringLiteral("phobiCCCP-stock")},
                                             {QStringLiteral("v"), 1},
                                             {QStringLiteral("coll"), coll}})
                .toJson(QJsonDocument::Compact);
        };
        check(!A.merge(oldPacket(100, "{\"collections\":[{\"name\":\"Old\",\"tools\":[201]}]}")),
              "an older copy's older set is ignored");
        check(A.merge(oldPacket(9000, "{\"collections\":[{\"name\":\"Old\",\"tools\":[201]}]}")),
              "an older copy's newer set is taken");
        ca = collectionsIn(a);
        check(ca.all().size() == 1 && has(ca, QStringLiteral("Old"), {201}),
              "and replaces the whole set, as that copy means it");
    }

    // 3. Only bits the tool library knows are taken or sent; packets stay
    //    under the datagram limit.
    {
        const QString s = tmp.path() + QStringLiteral("/s.ini");
        putIni(s, "{\"collections\":[]}", 1);
        c2d::StockSync S(s, 0);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const QJsonObject stock{
            {QStringLiteral("201"), QJsonObject{{QStringLiteral("have"), 4}, {QStringLiteral("broken"), 0}, {QStringLiteral("ts"), double(now)}}},
            {QStringLiteral("4242"), QJsonObject{{QStringLiteral("have"), 4}, {QStringLiteral("broken"), 0}, {QStringLiteral("ts"), double(now)}}},
            {QStringLiteral("99999"), QJsonObject{{QStringLiteral("have"), 1}, {QStringLiteral("broken"), 0}, {QStringLiteral("ts"), double(now)}}}};
        const QByteArray packet = QJsonDocument(QJsonObject{{QStringLiteral("app"), QStringLiteral("phobiCCCP-stock")},
                                                            {QStringLiteral("v"), 1},
                                                            {QStringLiteral("stock"), stock}})
                                      .toJson(QJsonDocument::Compact);
        check(S.merge(packet), "a known bit's count is taken");
        QSettings st(s, QSettings::IniFormat);
        check(st.value(QStringLiteral("toolStock/201/have")).toInt() == 4, "#201 (in the library) is set");
        check(!st.contains(QStringLiteral("toolStock/4242/have")) && !st.contains(QStringLiteral("toolStock/99999/have")),
              "numbers the library does not know are refused");

        // An unknown number stored by an older version is not passed on.
        st.setValue(QStringLiteral("toolStock/77777/have"), 3);
        st.setValue(QStringLiteral("toolStock/77777/ts"), now);
        // Collections near the size limit.
        QByteArray big = "{\"collections\":[";
        for (int i = 0; big.size() < 45000; ++i)
            big += (i ? "," : "") + QStringLiteral("{\"name\":\"C%1\",\"tools\":[201,102]}").arg(i).toUtf8();
        big += "]}";
        st.setValue(QStringLiteral("tools/collections"), big);
        st.setValue(QStringLiteral("tools/collectionsTs"), now);
        st.sync();
        const QList<QByteArray> out = S.payloads();
        bool small = !out.isEmpty();
        for (const QByteArray &p : out)
            small = small && p.size() <= c2d::StockSync::kMaxDatagram;
        check(small, "every packet fits in a datagram");
        check(!out.first().contains("\"77777\"") && out.first().contains("\"201\""),
              "only library bits are announced");
    }

    QSettings().clear();
    return g_fail ? 1 : 0;
}
