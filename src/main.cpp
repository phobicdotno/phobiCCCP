#include "mainwindow.h"
#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("phobicCC"));
    QCoreApplication::setOrganizationName(QStringLiteral("phobicdotno"));

    c2d::MainWindow w;
    w.show();

    // Optional: open a file passed on the command line.
    if (argc > 1)
        w.openFile(QString::fromLocal8Bit(argv[1]));

    return app.exec();
}
