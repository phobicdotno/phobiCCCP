#include "mainwindow.h"
#include "gcodeexport.h"
#include "grblstreamer.h"
#include "vcarve.h"
#include <QSet>
#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QRegularExpression>
#include <QTimer>
#include <functional>
#include <QPalette>
#include <QStyleFactory>

// Fusion + a hand-tuned dark palette; small QSS pass for spacing/accents.
static void applyDarkTheme(QApplication &app)
{
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QPalette p;
    const QColor bg(0x1e, 0x21, 0x27), base(0x16, 0x18, 0x1d),
        text(0xd8, 0xdc, 0xe4), disabled(0x6a, 0x71, 0x7d),
        accent(0xf2, 0xa5, 0x36), button(0x2a, 0x2e, 0x36);
    p.setColor(QPalette::Window, bg);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, bg);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, button);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, QColor(0x1a, 0x1a, 0x1a));
    p.setColor(QPalette::ToolTipBase, base);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, disabled);
    p.setColor(QPalette::Disabled, QPalette::Text, disabled);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    p.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    app.setPalette(p);

    app.setStyleSheet(QStringLiteral(R"(
        QToolBar { spacing: 4px; padding: 4px; border: 0; }
        QToolButton { padding: 4px 8px; border-radius: 4px; }
        QToolButton:checked { background: #3a4150; }
        QToolButton:hover { background: #333945; }
        QStatusBar { background: #16181d; }
        QPlainTextEdit { font-family: monospace; font-size: 12px; border: 0; }
        QDockWidget::title { background: #23262e; padding: 5px; }
        QSpinBox { padding: 2px 4px; }
    )"));
}

// --selftest <in.c2d> <out.c2d>: headless create/save/reload check — loads the
// input, adds one circle + rectangle + hexagon via the factories, saves, then
// reloads the output and reports the element count. Exits 0 on success.
static int selftest(const QString &in, const QString &out)
{
    c2d::Document doc;
    QString err;
    if (!doc.load(in, &err)) { qWarning() << "load failed:" << err; return 1; }
    const int before = doc.elements().size();

    const QJsonObject layer = doc.defaultLayer();
    doc.addElement(c2d::Element::makeCircle({100, 100}, 25, layer));
    doc.addElement(c2d::Element::makeRectangle({200, 100}, 60, 40, layer));
    doc.addElement(c2d::Element::makePolygon({300, 100}, 30, 6, layer));
    doc.addElement(c2d::Element::makePath({{400, 80}, {430, 130}, {460, 80}}, false, layer));
    doc.addElement(c2d::Element::makePath({{500, 80}, {530, 130}, {560, 80}}, true, layer));
    doc.addElement(c2d::Element::makeText(QStringLiteral("phobicCC"), {600, 80}, 15,
                                          QStringLiteral("Helvetica"), layer));

    if (!doc.save(out, &err)) { qWarning() << "save failed:" << err; return 2; }

    c2d::Document check;
    if (!check.load(out, &err)) { qWarning() << "reload failed:" << err; return 3; }
    qInfo() << "selftest: before =" << before
            << "after reload =" << check.elements().size()
            << "toolpaths =" << check.toolpaths().size();
    if (check.elements().size() != before + 6)
        return 4;

    // Text must round-trip with non-empty rendered glyph contours.
    bool textOk = false;
    for (const c2d::Element &e : check.elements())
        if (e.geometryType == QLatin1String("text"))
            textOk = !e.painterPath.isEmpty()
                     && e.raw.value("rendered").toArray().size() > 3
                     && e.raw.value("text").toString() == QLatin1String("phobicCC");
    qInfo() << "selftest text:" << (textOk ? "OK" : "FAILED");
    if (!textOk)
        return 10;

    // Parametric edit: regen must change geometry but keep the identity.
    bool regenOk = false;
    for (const c2d::Element &e : check.elements()) {
        if (e.geometryType == QLatin1String("circle")
            && e.raw.value("center").toArray().at(1).toDouble() == 100.0) {
            const c2d::Element r =
                c2d::Element::regen(e, {{QStringLiteral("radius"), 30.0}});
            regenOk = r.id == e.id
                      && r.raw.value("radius").toDouble() == 30.0
                      && r.painterPath.boundingRect().width() > 59.0;
            break;
        }
    }
    qInfo() << "selftest regen:" << (regenOk ? "OK" : "FAILED");
    if (!regenOk)
        return 5;

    // Toolpath edit round-trip: change end_depth (a string in CC's schema),
    // save in place, reload, verify it stuck and the tool payload survived.
    if (check.toolpaths().isEmpty()) { qWarning() << "no toolpaths"; return 6; }
    c2d::Toolpath tp = check.toolpaths().first();
    QJsonObject j = tp.json;
    j.insert("end_depth", QStringLiteral("-5.000"));
    tp.json = j;
    check.replaceToolpath(tp);
    if (!check.save(out, &err)) { qWarning() << "tp save failed:" << err; return 7; }
    c2d::Document check2;
    if (!check2.load(out, &err)) { qWarning() << "tp reload failed:" << err; return 8; }
    const c2d::Toolpath *tp2 = check2.toolpathByUuid(tp.uuid);
    const bool tpOk = tp2
        && tp2->json.value("end_depth").toString() == QLatin1String("-5.000")
        && tp2->json.value("tool").toObject().contains("diameter")
        && tp2->json.value("elements").toArray().size()
               == tp.json.value("elements").toArray().size();
    qInfo() << "selftest toolpath edit:" << (tpOk ? "OK" : "FAILED");
    if (!tpOk)
        return 9;

    // G-code export: turn one pocket into a no-offset contour so the exporter
    // has something it supports, then check for real motion + dialect markers.
    c2d::Toolpath ct = check2.toolpaths().first();
    QJsonObject cj = ct.json;
    cj.insert("type", QStringLiteral("contour"));
    cj.insert("ofset_dir", 0);
    ct.json = cj;
    ct.type = QStringLiteral("contour");
    check2.replaceToolpath(ct);
    const c2d::GcodeResult g = c2d::exportGcode(check2);
    const bool gOk = g.done.size() == 2 && g.skipped.isEmpty()
        && g.gcode.contains(QLatin1String("G90")) && g.gcode.contains(QLatin1String("G21"))
        && g.gcode.contains(QLatin1String("M03S")) && g.gcode.contains(QLatin1String("M02"))
        && g.gcode.count(QLatin1String("G1")) > 50
        && (g.gcode.contains(QLatin1String("G2X")) || g.gcode.contains(QLatin1String("G3X")));
    qInfo() << "selftest gcode:" << (gOk ? "OK" : "FAILED")
            << "lines =" << g.gcode.count(QChar('\n'))
            << "skipped =" << g.skipped;
    if (!gOk)
        return 11;

    // Fabricate the three newly supported types from the pocket payload and
    // make sure each produces motion.
    const c2d::Toolpath base = check2.toolpaths().first();
    auto fab = [&](const char *type, std::function<void(QJsonObject &)> tweak) {
        c2d::Toolpath x = base;
        QJsonObject o = x.json;
        o.insert("type", QLatin1String(type));
        o.insert("uuid", QStringLiteral("{fab-%1}").arg(QLatin1String(type)));
        tweak(o);
        x.json = o;
        x.type = QLatin1String(type);
        x.uuid = o.value("uuid").toString();
        check2.addToolpath(x);
    };
    fab("keyhole_toolpath", [](QJsonObject &o) {
        o.insert("name", QStringLiteral("kh"));
        o.insert("angle", 90);
        o.insert("length", 12.7);
        o.insert("end_depth", QStringLiteral("-2.5"));
    });
    fab("texture_toolpath", [](QJsonObject &o) {
        o.insert("name", QStringLiteral("tx"));
        o.insert("angle", 30);
        o.insert("min_length", 5);
        o.insert("max_length", 12);
        o.insert("min_depth", QStringLiteral("-0.5"));
        o.insert("max_depth", QStringLiteral("-1.5"));
        o.insert("stepover", 2.0);
    });
    fab("advanced_vcarve_toolpath", [](QJsonObject &o) {
        o.insert("name", QStringLiteral("vc"));
        o.insert("end_depth", QStringLiteral("-3.0"));
        o.insert("stepover", 0.5);
        QJsonObject tool = o.value("tool").toObject();
        tool.insert("angle", 60);
        tool.insert("type", 2);
        o.insert("tool", tool);
    });
    const c2d::GcodeResult g2 = c2d::exportGcode(check2);
    const bool g2Ok = g2.done.contains(QStringLiteral("kh"))
        && g2.done.contains(QStringLiteral("tx"))
        && g2.done.contains(QStringLiteral("vc"))
        && g2.gcode.size() > g.gcode.size();
    qInfo() << "selftest keyhole/texture/vcarve:" << (g2Ok ? "OK" : "FAILED")
            << "done =" << g2.done << "skipped =" << g2.skipped;
    if (!g2Ok)
        return 12;

    // Air-cut transform + job stats: spindle lines must vanish, the deepest Z
    // must rise by exactly the lift, and the stats must see real cutting.
    const c2d::JobStats st = c2d::computeStats(g2.ops);
    const QStringList plain = g2.gcode.split(QChar('\n'), Qt::SkipEmptyParts);
    const QStringList aired = c2d::airCutTransform(plain, 10.0);
    auto minZof = [](const QStringList &ls) {
        static const QRegularExpression re(QStringLiteral("Z(-?[0-9]+\\.?[0-9]*)"));
        double mn = 1e18;
        for (const QString &l : ls) {
            if (l.startsWith(QChar('(')))
                continue;
            auto it = re.globalMatch(l);
            while (it.hasNext())
                mn = qMin(mn, it.next().captured(1).toDouble());
        }
        return mn;
    };
    const double dz = minZof(aired) - minZof(plain);
    const bool airOk = aired.filter(QStringLiteral("M03")).isEmpty()
        && aired.filter(QStringLiteral("M05")).isEmpty()
        && qAbs(dz - 10.0) < 1e-3
        && st.hasBounds && st.cutLen > 100 && st.timeSec > 10;
    qInfo() << "selftest aircut/stats:" << (airOk ? "OK" : "FAILED")
            << "dz =" << dz << "cut mm =" << st.cutLen << "est s =" << st.timeSec;
    if (!airOk)
        return 13;

    // Arc fitting: an outside contour around the stage-1 rectangle gets
    // round-joined corners from the offsetter; the fitter must emit them as a
    // few G2/G3 arcs, not 1 mm G1 chatter.
    QString rectId;
    for (const c2d::Element &e : check2.elements())
        if (e.geometryType == QLatin1String("rectangle"))
            rectId = e.id;
    c2d::Toolpath rp = base;
    QJsonObject ro = rp.json;
    ro.insert("type", QStringLiteral("contour"));
    ro.insert("ofset_dir", 1);
    ro.insert("uuid", QStringLiteral("{fab-rect-outside}"));
    ro.insert("name", QStringLiteral("rp"));
    ro.insert("end_depth", QStringLiteral("-2.0"));
    ro.insert("elements", QJsonArray{QJsonObject{{QStringLiteral("uuid"), rectId}}});
    rp.json = ro;
    rp.type = QStringLiteral("contour");
    rp.uuid = ro.value("uuid").toString();
    check2.addToolpath(rp);
    const c2d::GcodeResult g4 = c2d::exportGcode(check2);
    int nArc = 0, nFeed = 0;
    bool inSection = false;
    for (const c2d::Op &op : g4.ops) {
        if (op.kind == c2d::Op::Comment) {
            inSection = (op.text == QLatin1String("rp"));
            continue;
        }
        if (!inSection)
            continue;
        if (op.kind == c2d::Op::Arc)  ++nArc;
        if (op.kind == c2d::Op::Feed) ++nFeed;
    }
    const bool fitOk = !rectId.isEmpty() && g4.done.contains(QStringLiteral("rp"))
        && nArc >= 4 && nFeed < nArc * 8;
    qInfo() << "selftest arcfit:" << (fitOk ? "OK" : "FAILED")
            << "arcs =" << nArc << "feeds =" << nFeed;
    if (!fitOk)
        return 14;

    // Medial-axis v-carve over the text element: chains must ride at many
    // distinct depths (clearance varies along a glyph) and stay inside the
    // glyph bounds. On builds without the Voronoi backend the ring fallback
    // still passes the depth-variety check, just with fewer levels.
    QString textId;
    QRectF textBB;
    for (const c2d::Element &e : check2.elements())
        if (e.geometryType == QLatin1String("text")) {
            textId = e.id;
            textBB = e.painterPath.boundingRect().adjusted(-2, -2, 2, 2);
        }
    c2d::Toolpath vt = base;
    QJsonObject vo = vt.json;
    vo.insert("type", QStringLiteral("advanced_vcarve_toolpath"));
    vo.insert("uuid", QStringLiteral("{fab-text-vcarve}"));
    vo.insert("name", QStringLiteral("vtx"));
    vo.insert("end_depth", QStringLiteral("-6.0"));
    vo.insert("elements", QJsonArray{QJsonObject{{QStringLiteral("uuid"), textId}}});
    QJsonObject vtool = vo.value("tool").toObject();
    vtool.insert("angle", 60);
    vtool.insert("type", 2);
    vo.insert("tool", vtool);
    vt.json = vo;
    vt.type = QStringLiteral("advanced_vcarve_toolpath");
    vt.uuid = vo.value("uuid").toString();
    check2.addToolpath(vt);
    const c2d::GcodeResult g5 = c2d::exportGcode(check2);
    QSet<int> zLevels;
    int nOut = 0, nMoves = 0;
    inSection = false;
    for (const c2d::Op &op : g5.ops) {
        if (op.kind == c2d::Op::Comment) {
            inSection = (op.text == QLatin1String("vtx"));
            continue;
        }
        if (!inSection || op.kind == c2d::Op::Spindle || op.kind == c2d::Op::Tool)
            continue;
        ++nMoves;
        if (!qIsFinite(op.x) || !qIsFinite(op.y) || !qIsFinite(op.z)
            || (op.z < 1.0 && !textBB.contains(QPointF(op.x, op.y))))
            ++nOut;
        if (op.kind == c2d::Op::Feed && op.z < -1e-6)
            zLevels.insert(int(op.z * 100));   // 0.01 mm buckets
    }
    const bool vOk = !textId.isEmpty() && g5.done.contains(QStringLiteral("vtx"))
        && nMoves > 50 && nOut == 0 && zLevels.size() >= 5;
    qInfo() << "selftest medial vcarve:" << (vOk ? "OK" : "FAILED")
            << "moves =" << nMoves << "zLevels =" << zLevels.size()
            << "outOfBounds =" << nOut
            << "medial =" << c2d::medialAxisAvailable();
    if (!vOk)
        return 15;

    // New-toolpath persistence: the fabricated toolpaths have no container
    // row yet — save must INSERT them, and a reload must bring them all back
    // (the inlay-male generator depends on this).
    const QString out2 = out + QStringLiteral(".tp.c2d");
    if (!check2.save(out2, &err)) {
        qWarning() << "tp-insert save failed:" << err;
        return 16;
    }
    c2d::Document check3;
    if (!check3.load(out2, &err)) {
        qWarning() << "tp-insert reload failed:" << err;
        return 16;
    }
    const c2d::Toolpath *vtx2 = check3.toolpathByUuid(QStringLiteral("{fab-text-vcarve}"));
    const bool insOk = check3.toolpaths().size() == check2.toolpaths().size()
        && vtx2
        && vtx2->json.value("end_depth").toString() == QLatin1String("-6.0");
    qInfo() << "selftest toolpath insert:" << (insOk ? "OK" : "FAILED")
            << "toolpaths =" << check3.toolpaths().size();
    return insOk ? 0 : 16;
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("phobicCC"));
    QCoreApplication::setOrganizationName(QStringLiteral("phobicdotno"));
    applyDarkTheme(app);

    if (argc == 4 && QByteArray(argv[1]) == "--selftest")
        return selftest(QString::fromLocal8Bit(argv[2]),
                        QString::fromLocal8Bit(argv[3]));

    // --grbl-check <port>: SAFE hardware handshake through the real streamer —
    // opens the port, reports the banner and live status, asks for $I build
    // info, disconnects. Sends NO motion, unlock, homing or spindle commands.
    if (argc == 3 && QByteArray(argv[1]) == "--grbl-check") {
        c2d::GrblStreamer grbl;
        bool sawStatus = false, sawOk = false;
        QObject::connect(&grbl, &c2d::GrblStreamer::consoleLine,
                         [&](const QString &l) {
            qInfo().noquote() << "RX:" << l;
            if (l == QLatin1String("ok"))
                sawOk = true;
        });
        QObject::connect(&grbl, &c2d::GrblStreamer::statusReport,
                         [&](const QString &st, double x, double y, double z) {
            if (!sawStatus)
                qInfo().noquote() << QStringLiteral("STATUS: %1  MPos X%2 Y%3 Z%4")
                                         .arg(st).arg(x).arg(y).arg(z);
            sawStatus = true;
        });
        QObject::connect(&grbl, &c2d::GrblStreamer::errorOccurred,
                         [](const QString &e) { qWarning().noquote() << "ERR:" << e; });
        if (!grbl.connectPort(QString::fromLocal8Bit(argv[2])))
            return 1;
        qInfo().noquote() << "connected to" << argv[2];
        QTimer::singleShot(2000, &grbl, [&] { grbl.sendCommand(QStringLiteral("$I")); });
        int rc = 1;
        QTimer::singleShot(5000, &app, [&] {
            rc = (sawStatus && sawOk) ? 0 : 1;
            qInfo().noquote() << (rc == 0 ? "GRBL_CHECK_OK" : "GRBL_CHECK_FAILED");
            grbl.disconnectPort();
            QCoreApplication::exit(rc);
        });
        return app.exec();
    }

    // --grbl-jog-z1 <port>: the first authorized MOTION test — unlock ($X)
    // and jog Z UP by exactly 1 mm at a gentle F200 via the same $J= command
    // the Machine panel's jog button emits. Verifies motion end-to-end by
    // watching MPos change. Spindle untouched.
    if (argc == 3 && QByteArray(argv[1]) == "--grbl-jog-z1") {
        c2d::GrblStreamer grbl;
        double firstZ = 0, lastZ = 0;
        bool haveFirst = false;
        QString lastState;
        QObject::connect(&grbl, &c2d::GrblStreamer::consoleLine,
                         [](const QString &l) { qInfo().noquote() << "RX:" << l; });
        QObject::connect(&grbl, &c2d::GrblStreamer::statusReport,
                         [&](const QString &st, double, double, double z) {
            if (!haveFirst) { firstZ = z; haveFirst = true; }
            lastZ = z;
            if (st != lastState) {
                lastState = st;
                qInfo().noquote() << "STATE:" << st << " Z =" << z;
            }
        });
        QObject::connect(&grbl, &c2d::GrblStreamer::errorOccurred,
                         [](const QString &e) { qWarning().noquote() << "ERR:" << e; });
        if (!grbl.connectPort(QString::fromLocal8Bit(argv[2])))
            return 1;
        qInfo().noquote() << "connected — unlocking, then jog Z+1 @ F200";
        QTimer::singleShot(2000, &grbl, [&] { grbl.sendCommand(QStringLiteral("$X")); });
        QTimer::singleShot(3500, &grbl, [&] {
            grbl.sendCommand(QStringLiteral("$J=G91 Z1 F200"));
        });
        QTimer::singleShot(9000, &app, [&] {
            const double dz = lastZ - firstZ;
            qInfo().noquote() << QStringLiteral("Z start %1  end %2  moved %3 mm")
                                     .arg(firstZ, 0, 'f', 3).arg(lastZ, 0, 'f', 3)
                                     .arg(dz, 0, 'f', 3);
            const bool ok = qAbs(dz - 1.0) < 0.05;
            qInfo().noquote() << (ok ? "JOG_TEST_OK" : "JOG_TEST_INCONCLUSIVE");
            grbl.disconnectPort();
            QCoreApplication::exit(ok ? 0 : 2);
        });
        return app.exec();
    }

    // --grbl-aircut <port> <template.c2d> [lift_mm]: the first full-program
    // hardware stream, as a rehearsal. Builds a compact two-pocket demo from
    // the template's container (elements replaced, extra toolpaths disabled),
    // exports real g-code, applies the air-cut transform (spindle stripped,
    // every Z lifted), then: home -> jog to a known clear spot -> zero there
    // -> stream. The whole program spans <90 mm XY and rides 7-13 mm ABOVE
    // work zero, which itself sits 30 mm below the homed Z top — safe air
    // everywhere on any Shapeoko. Spindle is never started.
    if ((argc == 4 || argc == 5) && QByteArray(argv[1]) == "--grbl-aircut") {
        const QString port = QString::fromLocal8Bit(argv[2]);
        const QString tmpl = QString::fromLocal8Bit(argv[3]);
        const double lift = argc == 5 ? QByteArray(argv[4]).toDouble() : 10.0;

        c2d::Document doc;
        QString err;
        if (!doc.load(tmpl, &err)) { qWarning() << "load failed:" << err; return 1; }
        if (doc.toolpaths().isEmpty()) { qWarning() << "template has no toolpaths"; return 1; }
        while (!doc.elements().isEmpty())
            doc.removeElementById(doc.elements().first().id);
        const QJsonObject layer = doc.defaultLayer();
        doc.addElement(c2d::Element::makeCircle({20, 20}, 8, layer));
        doc.addElement(c2d::Element::makeRectangle({60, 25}, 30, 20, layer));
        {
            const QVector<c2d::Toolpath> tps = doc.toolpaths();
            for (int i = 0; i < tps.size(); ++i) {
                c2d::Toolpath t = tps.at(i);
                QJsonObject j = t.json;
                if (i == 0) {
                    j.insert("name", QStringLiteral("aircut_demo"));
                    j.insert("end_depth", QStringLiteral("-3.0"));
                    QJsonArray refs;
                    for (const c2d::Element &e : doc.elements())
                        refs.append(QJsonObject{{QStringLiteral("uuid"), e.id}});
                    j.insert("elements", refs);
                    j.insert("enabled", true);
                } else {
                    j.insert("enabled", false);
                }
                t.json = j;
                doc.replaceToolpath(t);
            }
        }
        const c2d::GcodeResult r = c2d::exportGcode(doc);
        if (r.done.isEmpty()) { qWarning() << "nothing exported:" << r.skipped; return 1; }
        QStringList lines = c2d::airCutTransform(
            r.gcode.split(QChar('\n'), Qt::SkipEmptyParts), lift);
        for (int i = lines.size() - 1; i >= 0; --i)   // headless: no M0 pauses
            if (lines.at(i).startsWith(QLatin1String("M0 ")))
                lines.removeAt(i);
        qInfo().noquote() << QStringLiteral("program: %1 lines, Z lift %2 mm, spindle stripped")
                                 .arg(lines.size()).arg(lift);
        qInfo().noquote() << c2d::statsSummary(c2d::computeStats(r.ops));

        c2d::GrblStreamer grbl;
        // One line in flight at a time: burst writes through usbip have been
        // seen to stall the link; an air rehearsal doesn't need throughput.
        grbl.setConservative(true);
        // Phases: 0 boot, 1 homing, 2 jog Z-30, 3 jog X-100 Y-100, 4 zero,
        // 5 streaming. Transitions on repeated Idle status after a dwell.
        int phase = 0;
        int idleStreak = 0;
        int lastPct = -1;
        QElapsedTimer phaseTimer;
        phaseTimer.start();
        QString lastState;
        double mpx = 0, mpy = 0, mpz = 0;
        auto advance = [&](int p, const QString &msg) {
            phase = p;
            idleStreak = 0;
            phaseTimer.restart();
            qInfo().noquote() << msg;
        };
        QObject::connect(&grbl, &c2d::GrblStreamer::consoleLine, &app,
                         [](const QString &l) {
            if (l != QLatin1String("ok"))
                qInfo().noquote() << "RX:" << l;
        });
        QObject::connect(&grbl, &c2d::GrblStreamer::errorOccurred, &app,
                         [](const QString &e) { qWarning().noquote() << "ERR:" << e; });
        QObject::connect(&grbl, &c2d::GrblStreamer::progressChanged, &app,
                         [&](int a, int t) {
            const int pct = t > 0 ? a * 100 / t : 0;
            if (pct / 10 != lastPct / 10) {
                lastPct = pct;
                qInfo().noquote() << QStringLiteral("progress %1%% (%2/%3)  MPos %4,%5,%6")
                                         .arg(pct).arg(a).arg(t)
                                         .arg(mpx, 0, 'f', 1).arg(mpy, 0, 'f', 1)
                                         .arg(mpz, 0, 'f', 1);
            }
        });
        QObject::connect(&grbl, &c2d::GrblStreamer::streamFinished, &app,
                         [&](bool okStream) {
            qInfo().noquote() << QStringLiteral("final MPos %1,%2,%3")
                                     .arg(mpx, 0, 'f', 3).arg(mpy, 0, 'f', 3)
                                     .arg(mpz, 0, 'f', 3);
            qInfo().noquote() << (okStream ? "AIRCUT_OK" : "AIRCUT_FAILED");
            grbl.disconnectPort();
            QCoreApplication::exit(okStream ? 0 : 2);
        });
        QObject::connect(&grbl, &c2d::GrblStreamer::statusReport, &app,
                         [&](const QString &stt, double x, double y, double z) {
            mpx = x; mpy = y; mpz = z;
            if (stt != lastState) {
                lastState = stt;
                qInfo().noquote() << QStringLiteral("STATE: %1  MPos %2,%3,%4")
                                         .arg(stt).arg(x, 0, 'f', 2)
                                         .arg(y, 0, 'f', 2).arg(z, 0, 'f', 2);
            }
            idleStreak = (stt == QLatin1String("Idle")) ? idleStreak + 1 : 0;
            switch (phase) {
            case 1:   // homing: reports pause during $H; Idle again = done
                if (phaseTimer.elapsed() > 5000 && idleStreak >= 3) {
                    advance(2, QStringLiteral("homed — jog Z-30 for headroom"));
                    grbl.sendCommand(QStringLiteral("$J=G91 Z-30 F500"));
                }
                break;
            case 2:
                if (phaseTimer.elapsed() > 1500 && idleStreak >= 3) {
                    advance(3, QStringLiteral("jog X-100 Y-100 into the work area"));
                    grbl.sendCommand(QStringLiteral("$J=G91 X-100 Y-100 F2000"));
                }
                break;
            case 3:
                if (phaseTimer.elapsed() > 1500 && idleStreak >= 3) {
                    advance(4, QStringLiteral("zeroing work coordinates here"));
                    grbl.sendCommand(QStringLiteral("G10 L20 P1 X0 Y0 Z0"));
                    QTimer::singleShot(800, &grbl, [&] {
                        advance(5, QStringLiteral("== streaming air-cut program =="));
                        grbl.startStream(lines);
                    });
                }
                break;
            default:
                break;
            }
        });
        if (!grbl.connectPort(port))
            return 1;
        qInfo().noquote() << "connected — unlock, then home";
        QTimer::singleShot(2000, &grbl, [&] { grbl.sendCommand(QStringLiteral("$X")); });
        QTimer::singleShot(3000, &grbl, [&] {
            advance(1, QStringLiteral("homing ($H)…"));
            grbl.sendCommand(QStringLiteral("$H"));
        });
        // Ack-stall watchdog: if GRBL goes silent mid-stream, abort loudly
        // instead of hanging forever.
        auto *stallTimer = new QTimer(&app);
        stallTimer->setInterval(2000);
        QObject::connect(stallTimer, &QTimer::timeout, &app, [&] {
            // A giant arc's ok only arrives once the WHOLE arc has squeezed
            // through the 15-block planner — minutes for a metre-scale G2. A
            // late ack is a stall only when the machine isn't moving either.
            if (phase == 5 && grbl.isStreaming() && grbl.msSinceAck() > 10000
                && lastState != QLatin1String("Run")
                && lastState != QLatin1String("Hold")) {
                qWarning().noquote() << "STALL: no ack for 10 s while idle — aborting";
                grbl.stopStream();
                QCoreApplication::exit(4);
            }
        });
        stallTimer->start();
        QTimer::singleShot(15 * 60 * 1000, &app, [&] {
            qWarning() << "timeout — soft reset";
            grbl.stopStream();
            QCoreApplication::exit(3);
        });
        return app.exec();
    }

    // --grbl-circle <port> [feed]: sweep the largest circle that fits the
    // machine's actual travel. Homes, reads $130/$131 (X/Y max travel) from
    // the controller itself, zeroes at the home pull-off, then runs one full
    // G2 circle (two half arcs) centered in the envelope with a 15 mm rail
    // margin. Z never moves — the whole sweep happens at the homed Z top.
    if ((argc == 3 || argc == 4) && QByteArray(argv[1]) == "--grbl-circle") {
        const QString port = QString::fromLocal8Bit(argv[2]);
        const double feed = argc == 4 ? QByteArray(argv[3]).toDouble() : 3000.0;
        const double kMargin = 15.0;

        c2d::GrblStreamer grbl;
        grbl.setConservative(true);
        int phase = 0;
        int idleStreak = 0;
        QElapsedTimer phaseTimer;
        phaseTimer.start();
        QString lastState;
        double mpx = 0, mpy = 0, mpz = 0;
        double travelX = 0, travelY = 0;
        double zeroMx = 0, zeroMy = 0;   // MPos where work zero was set
        auto advance = [&](int p, const QString &msg) {
            phase = p;
            idleStreak = 0;
            phaseTimer.restart();
            qInfo().noquote() << msg;
        };
        QObject::connect(&grbl, &c2d::GrblStreamer::consoleLine, &app,
                         [&](const QString &l) {
            if (l == QLatin1String("ok"))
                return;
            qInfo().noquote() << "RX:" << l;
            if (l.startsWith(QLatin1String("$130=")))
                travelX = l.mid(5).toDouble();
            else if (l.startsWith(QLatin1String("$131=")))
                travelY = l.mid(5).toDouble();
            else if (l.startsWith(QLatin1String("Grbl ")) && phase >= 3) {
                qWarning().noquote() << "controller RESET mid-program — aborting";
                grbl.stopStream();
                grbl.disconnectPort();
                QCoreApplication::exit(6);
            }
        });
        QObject::connect(&grbl, &c2d::GrblStreamer::errorOccurred, &app,
                         [](const QString &e) { qWarning().noquote() << "ERR:" << e; });
        QObject::connect(&grbl, &c2d::GrblStreamer::streamFinished, &app,
                         [&](bool okStream) {
            if (!okStream) {
                qInfo().noquote() << "CIRCLE_FAILED";
                grbl.disconnectPort();
                QCoreApplication::exit(2);
                return;
            }
            // Acks mean queued, not done — disconnecting now would toggle DTR
            // and reset the controller mid-sweep. Wait for Idle instead.
            advance(6, QStringLiteral("program buffered — sweeping…"));
        });
        QObject::connect(&grbl, &c2d::GrblStreamer::statusReport, &app,
                         [&](const QString &stt, double x, double y, double z) {
            mpx = x; mpy = y; mpz = z;
            if (stt != lastState) {
                lastState = stt;
                qInfo().noquote() << QStringLiteral("STATE: %1  MPos %2,%3,%4")
                                         .arg(stt).arg(x, 0, 'f', 2)
                                         .arg(y, 0, 'f', 2).arg(z, 0, 'f', 2);
            }
            idleStreak = (stt == QLatin1String("Idle")) ? idleStreak + 1 : 0;
            switch (phase) {
            case 1:   // homing done -> ask the controller for its travel
                if (phaseTimer.elapsed() > 5000 && idleStreak >= 3) {
                    advance(2, QStringLiteral("homed — reading $$ settings"));
                    grbl.sendCommand(QStringLiteral("$$"));
                }
                break;
            case 2:
                if (phaseTimer.elapsed() > 2500) {
                    if (travelX < 100 || travelY < 100) {
                        qWarning().noquote()
                            << QStringLiteral("could not read travel ($130=%1 $131=%2)")
                                   .arg(travelX).arg(travelY);
                        grbl.disconnectPort();
                        QCoreApplication::exit(5);
                        return;
                    }
                    zeroMx = mpx;
                    zeroMy = mpy;
                    advance(3, QStringLiteral("travel %1 x %2 mm — zeroing at pull-off")
                                   .arg(travelX).arg(travelY));
                    grbl.sendCommand(QStringLiteral("G10 L20 P1 X0 Y0 Z0"));
                    QTimer::singleShot(800, &grbl, [&] {
                        // Envelope in work coords: MPos in [-travel, 0] and
                        // work = MPos - zeroM, so center and radius are:
                        const double cx = -travelX / 2.0 - zeroMx;
                        const double cy = -travelY / 2.0 - zeroMy;
                        const double r =
                            qMin(travelX, travelY) / 2.0 - kMargin;
                        qInfo().noquote()
                            << QStringLiteral("circle: center %1,%2  radius %3 mm "
                                              "(diameter %4) at F%5")
                                   .arg(cx, 0, 'f', 1).arg(cy, 0, 'f', 1)
                                   .arg(r, 0, 'f', 1).arg(2 * r, 0, 'f', 1)
                                   .arg(feed, 0, 'f', 0);
                        auto n3 = [](double v) {
                            return QString::number(v, 'f', 3);
                        };
                        // Eight 45° arcs, not two half-circles: the controller
                        // was seen hard-resetting when a second metre-scale G2
                        // arrived while the planner was saturated. Smaller
                        // tangent-continuous arcs keep the sweep one smooth
                        // circle with a steady ack cadence.
                        QStringList lines;
                        lines << QStringLiteral("G90") << QStringLiteral("G21")
                              << QStringLiteral("G0X%1Y%2")
                                     .arg(n3(cx + r), n3(cy));
                        for (int k = 1; k <= 8; ++k) {
                            const double a0 = -(k - 1) * M_PI / 4.0;
                            const double a1 = -k * M_PI / 4.0;
                            const double sx = cx + r * qCos(a0);
                            const double sy = cy + r * qSin(a0);
                            const double ex = cx + r * qCos(a1);
                            const double ey = cy + r * qSin(a1);
                            QString ln = QStringLiteral("G2X%1Y%2I%3J%4")
                                             .arg(n3(ex), n3(ey),
                                                  n3(cx - sx), n3(cy - sy));
                            if (k == 1)
                                ln += QStringLiteral("F%1").arg(n3(feed));
                            lines << ln;
                        }
                        lines << QStringLiteral("G0X%1Y%2").arg(n3(cx), n3(cy))
                              << QStringLiteral("M02");
                        advance(5, QStringLiteral("== sweeping the circle =="));
                        grbl.startStream(lines);
                    });
                }
                break;
            case 6:   // motion drain: Idle again = the sweep is done
                if (phaseTimer.elapsed() > 2000 && idleStreak >= 3) {
                    phase = 7;
                    qInfo().noquote() << QStringLiteral("final MPos %1,%2,%3")
                                             .arg(mpx, 0, 'f', 3)
                                             .arg(mpy, 0, 'f', 3)
                                             .arg(mpz, 0, 'f', 3);
                    qInfo().noquote() << "CIRCLE_OK";
                    grbl.disconnectPort();
                    QCoreApplication::exit(0);
                }
                break;
            default:
                break;
            }
        });
        if (!grbl.connectPort(port))
            return 1;
        qInfo().noquote() << "connected — unlock, then home";
        QTimer::singleShot(2000, &grbl, [&] { grbl.sendCommand(QStringLiteral("$X")); });
        QTimer::singleShot(3000, &grbl, [&] {
            advance(1, QStringLiteral("homing ($H)…"));
            grbl.sendCommand(QStringLiteral("$H"));
        });
        auto *stallTimer = new QTimer(&app);
        stallTimer->setInterval(2000);
        QObject::connect(stallTimer, &QTimer::timeout, &app, [&] {
            // A giant arc's ok only arrives once the WHOLE arc has squeezed
            // through the 15-block planner — minutes for a metre-scale G2. A
            // late ack is a stall only when the machine isn't moving either.
            if (phase == 5 && grbl.isStreaming() && grbl.msSinceAck() > 10000
                && lastState != QLatin1String("Run")
                && lastState != QLatin1String("Hold")) {
                qWarning().noquote() << "STALL: no ack for 10 s while idle — aborting";
                grbl.stopStream();
                QCoreApplication::exit(4);
            }
        });
        stallTimer->start();
        QTimer::singleShot(10 * 60 * 1000, &app, [&] {
            qWarning() << "timeout — soft reset";
            grbl.stopStream();
            QCoreApplication::exit(3);
        });
        return app.exec();
    }

    // --shot <in.c2d> <out.png>: open the full GUI, render one frame to a PNG
    // and exit — headless visual smoke test of the whole window (offscreen ok).
    if ((argc == 4 || argc == 5) && QByteArray(argv[1]) == "--shot") {
        c2d::MainWindow w;
        w.resize(1680, 980);
        w.show();
        w.openFile(QString::fromLocal8Bit(argv[2]));
        if (argc == 5 && QByteArray(argv[4]) == "preview")
            w.showToolpathPreview();
        const QString out = QString::fromLocal8Bit(argv[3]);
        int rc = 1;
        QTimer::singleShot(1500, &w, [&] {
            rc = w.grab().save(out) ? 0 : 1;
            qInfo().noquote() << (rc == 0 ? QStringLiteral("SHOT_OK %1").arg(out)
                                          : QStringLiteral("SHOT_FAILED"));
            QCoreApplication::exit(rc);
        });
        app.exec();
        return rc;
    }

    // --export <in.c2d> <out.nc>: headless g-code export (CLI/scripting).
    if (argc == 4 && QByteArray(argv[1]) == "--export") {
        c2d::Document doc;
        QString err;
        if (!doc.load(QString::fromLocal8Bit(argv[2]), &err)) {
            qWarning() << "load failed:" << err;
            return 1;
        }
        const c2d::GcodeResult r = c2d::exportGcode(doc);
        QFile f(QString::fromLocal8Bit(argv[3]));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            qWarning() << "write failed:" << f.errorString();
            return 2;
        }
        f.write(r.gcode.toUtf8());
        qInfo() << "exported:" << r.done << "skipped:" << r.skipped
                << "lines:" << r.gcode.count(QChar('\n'));
        return r.done.isEmpty() ? 3 : 0;
    }

    c2d::MainWindow w;
    w.show();

    // Optional: open a file passed on the command line.
    if (argc > 1)
        w.openFile(QString::fromLocal8Bit(argv[1]));

    return app.exec();
}
