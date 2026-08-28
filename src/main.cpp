#include "mainwindow.h"
#include "gcodeexport.h"
#include "grblstreamer.h"
#include <QApplication>
#include <QDebug>
#include <QFile>
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
    return fitOk ? 0 : 14;
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
