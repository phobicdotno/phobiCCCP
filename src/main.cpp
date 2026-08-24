#include "mainwindow.h"
#include <QApplication>
#include <QDebug>

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

    if (!doc.save(out, &err)) { qWarning() << "save failed:" << err; return 2; }

    c2d::Document check;
    if (!check.load(out, &err)) { qWarning() << "reload failed:" << err; return 3; }
    qInfo() << "selftest: before =" << before
            << "after reload =" << check.elements().size()
            << "toolpaths =" << check.toolpaths().size();
    return check.elements().size() == before + 3 ? 0 : 4;
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("phobicCC"));
    QCoreApplication::setOrganizationName(QStringLiteral("phobicdotno"));

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
