#include "mainwindow.h"
#include <QApplication>
#include <QDebug>
#include <QJsonArray>
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

    if (!doc.save(out, &err)) { qWarning() << "save failed:" << err; return 2; }

    c2d::Document check;
    if (!check.load(out, &err)) { qWarning() << "reload failed:" << err; return 3; }
    qInfo() << "selftest: before =" << before
            << "after reload =" << check.elements().size()
            << "toolpaths =" << check.toolpaths().size();
    if (check.elements().size() != before + 5)
        return 4;

    // Parametric edit: regen must change geometry but keep the identity.
    for (const c2d::Element &e : check.elements()) {
        if (e.geometryType == QLatin1String("circle")
            && e.raw.value("center").toArray().at(1).toDouble() == 100.0) {
            const c2d::Element r =
                c2d::Element::regen(e, {{QStringLiteral("radius"), 30.0}});
            const bool ok = r.id == e.id
                            && r.raw.value("radius").toDouble() == 30.0
                            && r.painterPath.boundingRect().width() > 59.0;
            qInfo() << "selftest regen:" << (ok ? "OK" : "FAILED");
            return ok ? 0 : 5;
        }
    }
    return 6;
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

    c2d::MainWindow w;
    w.show();

    // Optional: open a file passed on the command line.
    if (argc > 1)
        w.openFile(QString::fromLocal8Bit(argv[1]));

    return app.exec();
}
