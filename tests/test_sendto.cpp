// File > Send to phobiCCCP…: the dialog against a receiver on loopback. It
// asks for the machines, then sends the design whole, as base64 in an "open"
// request, with the machine and the start flag the user chose.
#include "../src/projectreceiver.h"
#include "../src/sendtodialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <cstdio>
#include <functional>

using namespace c2d;

static int g_fail = 0;
static void check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

static bool waitFor(const std::function<bool()> &done, int ms = 5000)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

static QPushButton *button(QWidget *w, const QString &text)
{
    for (QPushButton *b : w->findChildren<QPushButton *>())
        if (b->text() == text)
            return b;
    return nullptr;
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("phobicccp-test-sendto"));
    QTemporaryDir tmp;
    const QByteArray design = QByteArray("SQLite format 3") + '\0' + QByteArray(5000, 'x');

    ProjectReceiver rx;
    QString err;
    check(rx.start(0, &err), "receiver listens on a free port");
    QList<QJsonObject> asked;
    QObject::connect(&rx, &ProjectReceiver::request, &rx,
                     [&](int id, const QJsonObject &req, const QString &) {
                         asked.append(req);
                         const QString op = req.value(QStringLiteral("op")).toString();
                         if (op == QLatin1String("info"))
                             rx.respond(id, QJsonObject{
                                 {QStringLiteral("ok"), true},
                                 {QStringLiteral("allowStart"), true},
                                 {QStringLiteral("machines"), QJsonArray{
                                     QJsonObject{{QStringLiteral("id"), QStringLiteral("m1")},
                                                 {QStringLiteral("name"), QStringLiteral("Shapeoko")}},
                                     QJsonObject{{QStringLiteral("id"), QStringLiteral("m2")},
                                                 {QStringLiteral("name"), QStringLiteral("Router")},
                                                 {QStringLiteral("active"), true}}}}});
                         else
                             rx.respond(id, QJsonObject{{QStringLiteral("ok"), true},
                                                        {QStringLiteral("message"), QStringLiteral("opened it")}});
                     });

    int copies = 0;
    SendToDialog dlg(QStringLiteral("Bracket"), QStringLiteral("me"), [&](QString *) {
        ++copies;
        const QString p = tmp.filePath(QStringLiteral("copy.c2d"));
        QFile f(p);
        f.open(QIODevice::WriteOnly);
        f.write(design);
        return p;
    });
    dlg.show();
    auto *address = dlg.findChild<QLineEdit *>();
    auto *list = dlg.findChild<QListWidget *>();
    auto *machine = dlg.findChild<QComboBox *>();
    auto *start = dlg.findChild<QCheckBox *>();
    QPushButton *send = button(&dlg, QStringLiteral("Send"));
    check(address && list && machine && start && send, "dialog has its parts");
    if (g_fail)
        return 1;

    address->setText(QStringLiteral("127.0.0.1:%1").arg(rx.port()));
    button(&dlg, QStringLiteral("Add"))->click();
    check(waitFor([&] { return machine->count() == 2; }), "machines listed after info");
    check(machine->currentData().toString() == QLatin1String("m2"), "active machine preselected");
    check(start->isEnabled(), "start allowed when the computer allows it");
    check(send->isEnabled(), "send enabled");

    machine->setCurrentIndex(0);
    start->setChecked(true);
    send->click();
    check(waitFor([&] { return asked.size() == 2; }), "open request arrived");
    if (asked.size() == 2) {
        const QJsonObject r = asked[1];
        check(r.value(QStringLiteral("op")).toString() == QLatin1String("open"), "op is open");
        check(QByteArray::fromBase64(r.value(QStringLiteral("c2d")).toString().toLatin1()) == design,
              "design arrives whole");
        check(r.value(QStringLiteral("name")).toString() == QLatin1String("Bracket"), "name sent");
        check(r.value(QStringLiteral("machine")).toString() == QLatin1String("m1"), "chosen machine sent");
        check(r.value(QStringLiteral("start")).toBool(), "start flag sent");
    }
    check(waitFor([&] {
              for (QLabel *l : dlg.findChildren<QLabel *>())
                  if (l->text().contains(QLatin1String("opened it")))
                      return true;
              return false;
          }),
          "answer shown");
    check(copies == 1, "one copy made");
    check(!QFile::exists(tmp.filePath(QStringLiteral("copy.c2d"))), "copy removed after sending");

    // Nothing listening: said so, nothing sent.
    rx.stop();
    address->setText(QStringLiteral("127.0.0.1:%1").arg(1));
    button(&dlg, QStringLiteral("Add"))->click();
    check(waitFor([&] {
              for (QLabel *l : dlg.findChildren<QLabel *>())
                  if (l->text().contains(QLatin1String("does not answer")))
                      return true;
              return false;
          }),
          "unreachable computer reported");
    check(!send->isEnabled(), "cannot send to it");
    QSettings().clear();
    std::printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
