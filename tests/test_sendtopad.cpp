// File > Send to pad…: the dialog lists the pad apps in touch, sends to the
// one picked, and says so once that pad has taken the design.
#include "../src/sendtopaddialog.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
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

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    QList<padlink::Pads::Pad> pads;
    QString sentTo;
    bool waiting = true;
    SendToPadDialog::Hooks hooks;
    hooks.pads = [&] { return pads; };
    hooks.send = [&](const QString &padId, QString *note, QString *) -> qint64 {
        sentTo = padId;
        *note = QStringLiteral("Without the curves.");
        return 42;
    };
    hooks.waiting = [&](const QString &padId, qint64 seq) { return waiting && padId == sentTo && seq == 42; };
    hooks.makeCopy = [](QString *err) {
        *err = QStringLiteral("not used here");
        return QString();
    };
    hooks.ownId = QStringLiteral("me");

    SendToPadDialog dlg(QStringLiteral("Sign"), true, hooks);
    dlg.show();
    auto *list = dlg.findChild<QListWidget *>(QStringLiteral("pads"));
    auto *status = dlg.findChild<QLabel *>(QStringLiteral("status"));
    QPushButton *send = nullptr;
    for (QPushButton *b : dlg.findChildren<QPushButton *>())
        if (b->text() == QLatin1String("Send"))
            send = b;
    check(list && status && send, "dialog has its parts");
    if (g_fail)
        return 1;
    check(list->count() == 0 && status->text().contains(QLatin1String("No pad")), "no pad: said so");
    check(!send->isEnabled(), "nothing to send to");

    pads = {padlink::Pads::Pad{QStringLiteral("p1"), QStringLiteral("Tab"), QStringLiteral("10.0.0.5"), 1}};
    check(waitFor([&] { return list->count() == 1; }), "a pad in touch shows by itself");
    check(list->item(0)->text().contains(QLatin1String("Tab")), "by its name");
    check(list->currentRow() == 0 && send->isEnabled(), "the only pad is picked");
    check(status->text().isEmpty(), "and the no-pad line goes");

    send->click();
    check(sentTo == QLatin1String("p1"), "sent to the pad picked");
    check(status->text().contains(QLatin1String("Waiting for Tab")), "waits for the pad");
    check(!send->isEnabled(), "not sent twice meanwhile");
    waiting = false;
    check(waitFor([&] { return status->text().contains(QLatin1String("is open on Tab")); }),
          "says when the pad has it");
    check(status->text().contains(QLatin1String("Without the curves.")), "with what the pad does not hold");
    check(send->isEnabled(), "and can send again");

    SendToPadDialog off(QStringLiteral("Sign"), false, hooks);
    auto *offStatus = off.findChild<QLabel *>(QStringLiteral("status"));
    auto *offList = off.findChild<QListWidget *>(QStringLiteral("pads"));
    check(offList->count() == 0 && offStatus->text().contains(QLatin1String("Receive from")),
          "with receiving off, no pad app is listed and it says why");

    std::printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
