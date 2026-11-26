// Bit stock and Collections shared between copies of the app (StockSync):
// a fresh install's first edit pools its Collections with a peer's instead of
// replacing them, collections merge one by one (edits and deletions), an older
// copy's whole-set packets still work, only bits the tool library knows are
// taken, and no packet is bigger than a datagram may be.
#include "../src/stocksync.h"
#include "../src/stockstamp.h"
#include "../src/toolcollections.h"
#include "../src/toollibrary.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QEventLoop>
#include <QHostAddress>
#include <QNetworkDatagram>
#include <QTemporaryDir>
#include <QTimer>
#include <QUdpSocket>
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

static void putFresh(const QString &ini, const QByteArray &json, qint64 ts, const QByteArray &stamps)
{
    putIni(ini, json, ts, stamps);
    QSettings(ini, QSettings::IniFormat).setValue(QStringLiteral("tools/collectionsFresh"), true);
}

static qint64 wholeTs(const QByteArray &payload)
{
    return static_cast<qint64>(QJsonDocument::fromJson(payload).object().value(QStringLiteral("coll"))
                                   .toObject().value(QStringLiteral("ts")).toDouble());
}

static void runFor(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
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
        // (A's whole-set stamp is "now": its merges raised it, see 5.)
        check(A.merge(oldPacket(QDateTime::currentMSecsSinceEpoch() + 60000,
                                "{\"collections\":[{\"name\":\"Old\",\"tools\":[201]}]}")),
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

    // 4. Two fresh installs meet: they pool once, then both are ordinary
    //    copies, agree, stop answering each other, and a deletion sticks.
    const QString aIni = tmp.path() + QStringLiteral("/fa.ini"), cIni = tmp.path() + QStringLiteral("/fc.ini");
    {
        // A added #201 to My Tools; C made Roughing and deleted Unused.
        putFresh(aIni, "{\"collections\":[{\"name\":\"My Tools\",\"tools\":[201]},{\"name\":\"Unused\",\"tools\":[]}]}",
                 1000, "{\"ts\":{\"My Tools\":1000},\"gone\":{}}");
        putFresh(cIni, "{\"collections\":[{\"name\":\"My Tools\",\"tools\":[]},{\"name\":\"Roughing\",\"tools\":[251]}]}",
                 1100, "{\"ts\":{\"Roughing\":1100},\"gone\":{\"Unused\":1100}}");
        c2d::StockSync A(aIni, 0), C(cIni, 0);
        check(A.merge(C.payload()), "fresh A pools fresh C's Roughing");
        check(!QSettings(aIni, QSettings::IniFormat).value(QStringLiteral("tools/collectionsFresh")).toBool(),
              "after pooling with another fresh copy A is fresh no more");
        check(C.merge(A.payload()), "C takes A's My Tools");
        check(!QSettings(cIni, QSettings::IniFormat).value(QStringLiteral("tools/collectionsFresh")).toBool(),
              "and C is fresh no more either");
        A.merge(C.payload());                       // C's deletion of Unused, nothing to show
        check(!A.merge(C.payload()) && !C.merge(A.payload()) && !A.merge(C.payload()),
              "fresh <-> fresh: the two agree and nothing changes any more");
        const c2d::ToolCollections a = collectionsIn(aIni), c = collectionsIn(cIni);
        check(has(a, QStringLiteral("My Tools"), {201}) && has(a, QStringLiteral("Roughing"), {251})
                  && has(c, QStringLiteral("My Tools"), {201}) && has(c, QStringLiteral("Roughing"), {251}),
              "both have My Tools with #201 and Roughing");
        check(!a.byName(QStringLiteral("Unused")) && !c.byName(QStringLiteral("Unused")),
              "Unused, deleted on C, is on neither");

        // C now deletes Roughing: it goes on A too and stays gone.
        {
            QSettings cs(cIni, QSettings::IniFormat);
            c2d::ToolCollections cc = collectionsIn(cs);
            cc.remove(QStringLiteral("Roughing"));
            QJsonObject st = QJsonDocument::fromJson(cs.value(QStringLiteral("tools/collectionsStamps")).toByteArray()).object();
            const qint64 t = QDateTime::currentMSecsSinceEpoch() + 5;
            QJsonObject ts = st.value(QStringLiteral("ts")).toObject(), gone = st.value(QStringLiteral("gone")).toObject();
            ts.remove(QStringLiteral("Roughing"));
            gone.insert(QStringLiteral("Roughing"), double(t));
            st.insert(QStringLiteral("ts"), ts);
            st.insert(QStringLiteral("gone"), gone);
            cs.setValue(QStringLiteral("tools/collections"), cc.toJson());
            cs.setValue(QStringLiteral("tools/collectionsStamps"), QJsonDocument(st).toJson(QJsonDocument::Compact));
            cs.setValue(QStringLiteral("tools/collectionsTs"), t);
            cs.sync();
        }
        check(A.merge(C.payload()) && !collectionsIn(aIni).byName(QStringLiteral("Roughing")),
              "a deletion after pooling goes over");
        check(!C.merge(A.payload()) && !A.merge(C.payload()) && !collectionsIn(cIni).byName(QStringLiteral("Roughing"))
                  && !collectionsIn(aIni).byName(QStringLiteral("Roughing")),
              "and sticks: nothing pools it back");

        // Over the wire: a packet that changes nothing gets no answer; one
        // from a copy that is behind does.
        const QHostAddress lo(QHostAddress::LocalHost);
        QUdpSocket spy, send;
        check(spy.bind(lo, 0), "spy socket binds");
        c2d::StockSync Cw(cIni, 0, lo, spy.localPort());
        check(Cw.start() && Cw.localPort() != 0, "C listens");
        runFor(1400);                               // its start-up announce
        while (spy.hasPendingDatagrams())
            spy.receiveDatagram();
        send.writeDatagram(A.payload(), lo, Cw.localPort());
        runFor(1200);
        check(!spy.hasPendingDatagrams(), "no answer to a packet that changes nothing");
        const QByteArray behind = QJsonDocument(QJsonObject{{QStringLiteral("app"), QStringLiteral("phobiCCCP-stock")},
                                                            {QStringLiteral("v"), 1},
                                                            {QStringLiteral("from"), QStringLiteral("someone")}})
                                      .toJson(QJsonDocument::Compact);
        send.writeDatagram(behind, lo, Cw.localPort());
        runFor(1200);
        check(spy.hasPendingDatagrams(), "a copy with no Collections at all gets an answer");
    }

    // 5. An older copy (whole-set stamp only) that took P's set at 300 takes
    //    L's merge of L's and P's edits: its stamp is strictly newer.
    {
        const QString l = tmp.path() + QStringLiteral("/l.ini"), pi = tmp.path() + QStringLiteral("/p.ini");
        putIni(l, "{\"collections\":[{\"name\":\"Roughing\",\"tools\":[251,201]},{\"name\":\"Finishing\",\"tools\":[102]}]}",
               200, "{\"ts\":{\"Roughing\":200,\"Finishing\":100},\"gone\":{}}");
        putIni(pi, "{\"collections\":[{\"name\":\"Roughing\",\"tools\":[251]},{\"name\":\"Finishing\",\"tools\":[102,112]}]}",
               300, "{\"ts\":{\"Roughing\":100,\"Finishing\":300},\"gone\":{}}");
        c2d::StockSync L(l, 0), P(pi, 0);
        const qint64 oldPeerTs = wholeTs(P.payload());          // the older copy took P's set
        check(oldPeerTs == 300, "P announces its set at 300");
        check(L.merge(P.payload()), "L takes P's Finishing");
        const QByteArray lp = L.payload();
        check(wholeTs(lp) > oldPeerTs, "L's merged set has a whole-set stamp newer than the older copy's");
        c2d::ToolCollections lset;
        c2d::ToolCollections::fromJson(QJsonDocument::fromJson(lp).object().value(QStringLiteral("coll")).toObject()
                                           .value(QStringLiteral("json")).toString().toUtf8(), &lset);
        check(has(lset, QStringLiteral("Roughing"), {251, 201}) && has(lset, QStringLiteral("Finishing"), {102, 112}),
              "so the older copy gets both edits");
        check(P.merge(lp), "P takes L's Roughing");
        check(wholeTs(P.payload()) == wholeTs(lp), "P ends with L's set and does not raise the stamp again");
        check(!L.merge(P.payload()) && !P.merge(L.payload()), "no back and forth between the two");
    }

    // 6. A merge that turns our set into exactly the peer's, while our stamp
    //    is not below the peer's: an older copy that holds our old set at our
    //    stamp must still get the new one. All had c1 and c7 at T0; A deleted
    //    c7 at T7 (the older copy O took {c1} at T7); B, offline, deleted c7
    //    at T5 and changed c1 at T6.
    {
        const QString a6 = tmp.path() + QStringLiteral("/a6.ini"), b6 = tmp.path() + QStringLiteral("/b6.ini");
        putIni(a6, "{\"collections\":[{\"name\":\"c1\",\"tools\":[201]}]}", 7000,
               "{\"ts\":{\"c1\":1000},\"gone\":{\"c7\":7000}}");
        putIni(b6, "{\"collections\":[{\"name\":\"c1\",\"tools\":[201,102]}]}", 6000,
               "{\"ts\":{\"c1\":6000},\"gone\":{\"c7\":5000}}");
        c2d::StockSync A6(a6, 0), B6(b6, 0);
        const qint64 oldPeerTs = wholeTs(A6.payload());       // O took A's set
        check(A6.merge(B6.payload()), "A takes B's changed c1");
        check(has(collectionsIn(a6), QStringLiteral("c1"), {201, 102}) && !collectionsIn(a6).byName(QStringLiteral("c7")),
              "A: B's c1, c7 still gone");
        const QByteArray ap = A6.payload();
        check(wholeTs(ap) > oldPeerTs, "A's stamp rises above the one the older copy holds");
        check(!B6.merge(ap) || wholeTs(B6.payload()) == wholeTs(ap), "B takes the stamp, not a new one");
        check(!A6.merge(B6.payload()) && !B6.merge(A6.payload()), "and the two stop there");
    }

    // 7. Big Collections and the stock split over packets: every packet fits,
    //    the Collections always go (in full, or as the whole set when the
    //    stamps do not fit), and a copy that hears all packets has the
    //    Collections and every count. Grown step by step through: all in one
    //    packet; Collections and some counts, the rest after; Collections
    //    alone, all counts after; Collections without stamps.
    {
        c2d::ToolLibrary lib;
        lib.loadDefault();
        QList<int> numbers;
        for (const c2d::LibraryTool &t : lib.tools())
            if (t.number > 0)
                numbers.append(t.number);
        check(numbers.size() >= 10, "the library has bits to count");
        const QString snd = tmp.path() + QStringLiteral("/snd.ini");
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        {
            QSettings ss(snd, QSettings::IniFormat);
            ss.clear();
            for (int i = 0; i < numbers.size(); ++i) {
                const QString base = QStringLiteral("toolStock/%1/").arg(numbers[i]);
                ss.setValue(base + QStringLiteral("have"), numbers[i] % 7 + 1);
                ss.setValue(base + QStringLiteral("broken"), 0);
                ss.setValue(base + QStringLiteral("ts"), now - 10000 + i);
            }
            ss.setValue(QStringLiteral("tools/collectionsTs"), 5000);
        }
        c2d::StockSync S(snd, 0);
        const QString quotes(6000, QLatin1Char('"'));   // escaped twice in a packet
        bool allFit = true, neverEmpty = true, delivered = true;
        bool sawMixed = false, sawCollOnly = false, sawSmall = false;
        for (int pad = 11000; pad <= 14000 && !(sawMixed && sawCollOnly && sawSmall); pad += 7) {
            const QString padName(pad, QLatin1Char('P'));
            const QJsonArray list{QJsonObject{{QStringLiteral("name"), quotes}, {QStringLiteral("tools"), QJsonArray{201}}},
                                  QJsonObject{{QStringLiteral("name"), padName}, {QStringLiteral("tools"), QJsonArray{102}}}};
            {
                QSettings ss(snd, QSettings::IniFormat);
                ss.setValue(QStringLiteral("tools/collections"),
                            QJsonDocument(QJsonObject{{QStringLiteral("collections"), list}}).toJson(QJsonDocument::Compact));
                ss.sync();
            }
            const QList<QByteArray> ps = S.payloads();
            for (const QByteArray &pk : ps)
                allFit = allFit && pk.size() <= c2d::StockSync::kMaxDatagram;
            if (ps.isEmpty()) {
                neverEmpty = false;
                continue;
            }
            const QJsonObject first = QJsonDocument::fromJson(ps.first()).object();
            const QJsonObject coll = first.value(QStringLiteral("coll")).toObject();
            const bool full = coll.contains(QStringLiteral("items"));
            const bool small = coll.contains(QStringLiteral("json")) && !full;
            neverEmpty = neverEmpty && (full || small);
            const bool stock0 = !first.value(QStringLiteral("stock")).toObject().isEmpty();
            bool *seen = nullptr;
            if (small)
                seen = &sawSmall;
            else if (full && ps.size() >= 2)
                seen = stock0 ? &sawMixed : &sawCollOnly;
            if (!seen || *seen)
                continue;
            *seen = true;
            // A copy with nothing hears every packet.
            const QString rcv = tmp.path() + QStringLiteral("/rcv.ini");
            QSettings(rcv, QSettings::IniFormat).clear();
            c2d::StockSync R(rcv, 0);
            for (const QByteArray &pk : ps)
                R.merge(pk);
            const c2d::ToolCollections got = collectionsIn(rcv);
            bool ok = has(got, quotes, {201}) && has(got, padName, {102});
            QSettings rs(rcv, QSettings::IniFormat);
            for (int n : numbers)
                ok = ok && rs.value(QStringLiteral("toolStock/%1/have").arg(n)).toInt() == n % 7 + 1;
            delivered = delivered && ok;
        }
        check(allFit, "no packet is bigger than a datagram");
        check(neverEmpty, "the Collections always go, in full or as the whole set");
        check(sawMixed, "seen: Collections and some counts in the first packet, the rest after");
        check(sawCollOnly, "seen: the Collections alone in the first packet, the counts after");
        check(sawSmall, "seen: Collections too big with their stamps go without them");
        check(delivered, "in each case a copy that hears the packets has the Collections and every count");
    }

    QSettings().clear();
    return g_fail ? 1 : 0;
}
