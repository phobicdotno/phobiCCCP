// The pad's Machine page (src/remotemachine.h) against a real main window's
// pad link (MainWindow::onPadRequest behind a ProjectReceiver on loopback):
// it polls "info" for the machine's status, switches machines, sends the
// open design to be opened, and a start is refused while the computer does
// not let pads start cutting. Also the addresses discovery sweeps.
#define private public
#include "../src/mainwindow.h"
#undef private
#include "../src/machinepanel.h"
#include "../src/machineprofiles.h"
#include "../src/projectreceiver.h"
#include "../src/remotemachine.h"
#include "../src/sendtodialog.h"

#include <QApplication>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAddressEntry>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace c2d;

class TestRemoteMachine : public QObject
{
    Q_OBJECT
    MainWindow *w = nullptr;
    ProjectReceiver *rx = nullptr;
    int infos = 0;
    QString a, b;   // the two machines on the "computer"

    QString address() const { return QStringLiteral("127.0.0.1:%1").arg(rx->port()); }
    // A fresh client remembers nothing typed in before.
    static void forgetHosts()
    {
        QSettings().remove(QStringLiteral("remoteMachine"));
    }
    QByteArray design()
    {
        QString err;
        const QString path = w->sendCopy(w->m_doc.filePath(), &err);
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return {};
        return f.readAll();
    }
    static QString activeMachine()
    {
        QSettings s;
        return machineprofiles::activeId(s);
    }

private slots:
    void initTestCase()
    {
        QSettings().clear();
        {
            QSettings s;
            a = machineprofiles::activeId(s);
            b = machineprofiles::add(s, QStringLiteral("Router B"));
            machineprofiles::activate(s, a);
        }
        QSettings().setValue(QStringLiteral("pad/allowStart"), false);
        w = new MainWindow;
        w->resize(1200, 800);
        w->show();
        QVERIFY(w->newUntitled(300, 200, 19));
        rx = new ProjectReceiver(w);
        QString err;
        QVERIFY2(rx->start(0, &err), qPrintable(err));
        QVERIFY(rx->isHealthy());
        QVERIFY(MulticastHold::holders() >= 1);
        w->m_receiver = rx;
        // As MainWindow::setReceiving wires it.
        connect(rx, &ProjectReceiver::request, w, [this](int id, const QJsonObject &req, const QString &peer) {
            if (req.value(QStringLiteral("op")).toString() == QLatin1String("info"))
                ++infos;
            QTimer::singleShot(0, w, [this, id, req, peer] { w->onPadRequest(id, req, peer); });
        });
    }

    void sweepAddresses()
    {
        auto entry = [](const char *ip) {
            QNetworkAddressEntry e;
            e.setIp(QHostAddress(QLatin1String(ip)));
            return e;
        };
        const QList<QHostAddress> t = SendToDialog::sweepTargets(
            {entry("192.168.1.37"), entry("127.0.0.1"), entry("169.254.3.4"), entry("fe80::1"),
             entry("10.0.0.5"), entry("192.168.1.80")});
        // Two /24s, 1 to 254, without this device's own three addresses.
        QCOMPARE(t.size(), 2 * 254 - 3);
        QVERIFY(t.contains(QHostAddress(QStringLiteral("192.168.1.1"))));
        QVERIFY(t.contains(QHostAddress(QStringLiteral("192.168.1.254"))));
        QVERIFY(t.contains(QHostAddress(QStringLiteral("10.0.0.1"))));
        QVERIFY(!t.contains(QHostAddress(QStringLiteral("192.168.1.37"))));
        QVERIFY(!t.contains(QHostAddress(QStringLiteral("192.168.1.80"))));
        QVERIFY(!t.contains(QHostAddress(QStringLiteral("192.168.1.0"))));
        QVERIFY(!t.contains(QHostAddress(QStringLiteral("192.168.1.255"))));
        for (const QHostAddress &h : t)
            QVERIFY(!h.isLoopback() && !h.isLinkLocal());
        // Never more than a few networks (a desktop with many bridges).
        QList<QNetworkAddressEntry> many;
        for (int i = 0; i < 10; ++i)
            many << entry(qPrintable(QStringLiteral("10.%1.0.1").arg(i)));
        QCOMPARE(SendToDialog::sweepTargets(many).size(), SendToDialog::kMaxSweepNets * 253);
    }

    void statusWords()
    {
        using namespace remotestatus;
        QCOMPARE(words({}), QStringLiteral("Not connected"));
        const QJsonObject idle{{"connected", true}, {"homed", true}, {"state", "Idle"}};
        QCOMPARE(words(idle), QStringLiteral("Idle, ready"));
        QVERIFY(readyToCut(idle));
        QCOMPARE(words({{"connected", true}, {"homed", false}, {"state", "Idle"}}),
                 QStringLiteral("Connected, not homed"));
        QCOMPARE(words({{"connected", true}, {"state", "Alarm:1"}}), QStringLiteral("Alarm: needs Initialize"));
        QCOMPARE(words({{"connected", true}, {"streaming", true}, {"job", "Sign.c2d"}}),
                 QStringLiteral("Cutting Sign.c2d"));
        QVERIFY(!readyToCut({{"connected", true}, {"homed", true}, {"busy", true}, {"state", "Idle"}}));
        QCOMPARE(jobHeading({{"streaming", true}}, QStringLiteral("Router")), QStringLiteral("RUNNING ON ROUTER"));
        QCOMPARE(jobHeading({{"jobEnd", "finished"}}, QStringLiteral("Router")), QStringLiteral("FINISHED ON ROUTER"));
    }

    void pollsInfo()
    {
        forgetHosts();
        RemoteMachine rm(QStringLiteral("pad-1"));
        QCOMPARE(rm.addAddress(address()), 0);
        rm.start();
        QVERIFY(MulticastHold::holders() >= 2);   // the receiver's and the client's
        QTRY_VERIFY_WITH_TIMEOUT(rm.current() && !rm.current()->info.isEmpty(), 5000);
        const RemoteMachine::Computer *c = rm.current();
        QVERIFY(rm.online(0));
        QCOMPARE(c->host, QSysInfo::machineHostName());
        QCOMPARE(c->info.value("machines").toArray().size(), 2);
        QCOMPARE(c->info.value("activeMachine").toString(), a);
        QCOMPARE(c->info.value("allowStart").toBool(), false);
        QCOMPARE(remotestatus::words(c->info.value("status").toObject()), QStringLiteral("Not connected"));
        // Asked again and again, about every 1.5 s.
        const int before = infos;
        QTest::qWait(3 * RemoteMachine::kPollMs + 400);
        QVERIFY2(infos - before >= 2, qPrintable(QString::number(infos - before)));
        rm.stop();
        // Typed in once, it is there the next time.
        RemoteMachine again(QStringLiteral("pad-1"));
        QCOMPARE(again.computers().size(), 1);
        QCOMPARE(again.current()->port, rx->port());
    }

    void ownInstallLeftOut()
    {
        forgetHosts();
        RemoteMachine rm(QStringLiteral("me"));
        SendToDialog::Computer self, pad, other;
        self.id = QStringLiteral("me");
        self.address = QStringLiteral("10.0.0.2");
        pad.id = QStringLiteral("p");
        pad.address = QStringLiteral("10.0.0.3");
        pad.pad = true;
        other.id = QStringLiteral("o");
        other.address = QStringLiteral("10.0.0.4");
        other.host = QStringLiteral("shop-pc");
        rm.addFound(self);
        rm.addFound(pad);
        rm.addFound(other);
        rm.addFound(other);
        QCOMPARE(rm.computers().size(), 1);
        QCOMPARE(rm.current()->host, QStringLiteral("shop-pc"));
    }

    void switchesMachine()
    {
        forgetHosts();
        RemoteMachine rm(QStringLiteral("pad-1"));
        rm.addAddress(address());
        rm.start();
        QTRY_VERIFY_WITH_TIMEOUT(rm.current() && !rm.current()->info.isEmpty(), 5000);
        QSignalSpy switched(&rm, &RemoteMachine::machineSwitched);
        rm.switchMachine(b);
        QVERIFY(switched.wait(10000));
        QCOMPARE(switched.at(0).at(0).toBool(), true);
        QVERIFY(switched.at(0).at(1).toString().contains(QStringLiteral("Router B")));
        QCOMPARE(activeMachine(), b);
        QCOMPARE(rm.current()->info.value("activeMachine").toString(), b);
        rm.switchMachine(a);
        QVERIFY(switched.wait(10000));
        QCOMPARE(activeMachine(), a);
    }

    void sendOpens()
    {
        forgetHosts();
        RemoteMachine rm(QStringLiteral("pad-1"));
        rm.addAddress(address());
        rm.start();
        QTRY_VERIFY_WITH_TIMEOUT(rm.current() && !rm.current()->info.isEmpty(), 5000);
        const QByteArray bytes = design();
        QVERIFY(bytes.startsWith("SQLite format 3"));
        QSignalSpy sent(&rm, &RemoteMachine::sent);
        QVERIFY(rm.send(QStringLiteral("Bracket"), bytes, false));
        QVERIFY(rm.isSending());
        QVERIFY(!rm.send(QStringLiteral("Bracket"), bytes, false));   // one at a time
        QVERIFY(sent.wait(20000));
        QCOMPARE(sent.at(0).at(0).toBool(), true);
        QCOMPARE(sent.at(0).at(1).toBool(), false);
        QVERIFY2(sent.at(0).at(2).toString().contains(QStringLiteral("opened Bracket.c2d")),
                 qPrintable(sent.at(0).at(2).toString()));
        QCOMPARE(QFileInfo(w->m_doc.filePath()).fileName(), QStringLiteral("Bracket.c2d"));
        // The next "info" says so.
        QTRY_COMPARE_WITH_TIMEOUT(rm.current()->info.value("document").toString(), QStringLiteral("Bracket.c2d"), 5000);
    }

    void startRefusedWhenPadsMayNot()
    {
        forgetHosts();
        QSettings().setValue(QStringLiteral("pad/allowStart"), false);
        RemoteMachine rm(QStringLiteral("pad-1"));
        rm.addAddress(address());
        rm.start();
        QTRY_VERIFY_WITH_TIMEOUT(rm.current() && !rm.current()->info.isEmpty(), 5000);
        QSignalSpy sent(&rm, &RemoteMachine::sent);
        QVERIFY(rm.send(QStringLiteral("Bracket"), design(), true));
        QVERIFY(sent.wait(20000));
        const QString msg = sent.at(0).at(2).toString();
        QCOMPARE(sent.at(0).at(1).toBool(), false);   // not started
        QVERIFY2(msg.contains(QStringLiteral("not started")) && msg.contains(QStringLiteral("Pads may start cutting")),
                 qPrintable(msg));
        QVERIFY(!w->m_machine->isBusy());
    }

    void refusedConnection()
    {
        forgetHosts();
        QTcpServer gone;
        QVERIFY(gone.listen(QHostAddress::LocalHost, 0));
        const quint16 port = gone.serverPort();
        gone.close();
        RemoteMachine rm(QStringLiteral("pad-1"));
        rm.addAddress(QStringLiteral("127.0.0.1:%1").arg(port));
        QSignalSpy sent(&rm, &RemoteMachine::sent);
        QVERIFY(rm.send(QStringLiteral("Bracket"), QByteArray("x"), true));
        QVERIFY(sent.wait(10000));
        QCOMPARE(sent.at(0).at(0).toBool(), false);
        QVERIFY2(sent.at(0).at(2).toString().startsWith(QStringLiteral("Not sent")),
                 qPrintable(sent.at(0).at(2).toString()));
    }

    void cutOffMayStillOpen()
    {
        // The request went out and the line dropped: it may still be opened
        // (and started) there, so the pad must not suggest sending again.
        forgetHosts();
        QTcpServer cut;
        QVERIFY(cut.listen(QHostAddress::LocalHost, 0));
        connect(&cut, &QTcpServer::newConnection, &cut, [&cut] {
            QTcpSocket *s = cut.nextPendingConnection();
            connect(s, &QTcpSocket::readyRead, s, [s] {
                if (s->readAll().contains("\"op\""))
                    s->abort();
            });
        });
        RemoteMachine rm(QStringLiteral("pad-1"));
        rm.addAddress(QStringLiteral("127.0.0.1:%1").arg(cut.serverPort()));
        QSignalSpy sent(&rm, &RemoteMachine::sent);
        QVERIFY(rm.send(QStringLiteral("Bracket"), QByteArray("x"), true));
        QVERIFY(sent.wait(10000));
        const QString msg = sent.at(0).at(2).toString();
        QVERIFY2(msg.contains(QStringLiteral("may still open and start")), qPrintable(msg));
    }

    void panelGatesStart()
    {
        forgetHosts();
        QSettings().setValue(QStringLiteral("pad/allowStart"), false);
        RemoteMachinePanel::Design d;
        d.name = [] { return QStringLiteral("Bracket"); };
        d.toolpaths = [] { return 1; };
        d.makeCopy = [this](QString *err) { return w->sendCopy(w->m_doc.filePath(), err); };
        RemoteMachinePanel panel(QStringLiteral("pad-1"), d);
        panel.resize(1100, 700);
        panel.show();
        auto *edit = panel.findChild<QLineEdit *>();
        QVERIFY(edit);
        edit->setText(address());
        QMetaObject::invokeMethod(edit, "returnPressed");
        QPushButton *start = nullptr, *send = nullptr;
        for (QPushButton *p : panel.findChildren<QPushButton *>()) {
            if (p->objectName() == QLatin1String("start"))
                start = p;
            if (p->text() == QLatin1String("Send without starting"))
                send = p;
        }
        QVERIFY(start && send);
        auto labelWith = [&panel](const QString &text) {
            for (QLabel *l : panel.findChildren<QLabel *>())
                if (l->isVisible() && l->text().contains(text))
                    return true;
            return false;
        };
        // Starting is off on that computer: only Send, and the page says why.
        QTRY_VERIFY_WITH_TIMEOUT(send->isEnabled(), 5000);
        QVERIFY(!start->isEnabled());
        QVERIFY(labelWith(QStringLiteral("Start is off on")));
        QVERIFY(labelWith(QStringLiteral("Not connected")));
        // Turned on there: Start too, after a confirmation.
        QSettings().setValue(QStringLiteral("pad/allowStart"), true);
        QTRY_VERIFY_WITH_TIMEOUT(start->isEnabled(), 5000);
        QVERIFY(labelWith(QStringLiteral("The machine is not ready")));
        // Cancelled first: nothing is sent.
        QString asked;
        auto answer = [&asked](const QString &button) {
            QTimer::singleShot(300, [&asked, button] {
                auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                if (!box)
                    return;
                asked = box->text();
                for (QAbstractButton *bt : box->buttons())
                    if (bt->text().remove(QLatin1Char('&')) == button)
                        bt->click();
            });
        };
        answer(QStringLiteral("Cancel"));
        start->click();
        QVERIFY(asked.contains(QStringLiteral("will start on")));
        QVERIFY(!panel.client()->isSending());
        asked.clear();
        answer(QStringLiteral("Send and start"));
        start->click();
        QVERIFY(asked.contains(QStringLiteral("Bracket")));
        QVERIFY(panel.client()->isSending());
        // The machine here is not connected, so it opens but does not start.
        QTRY_VERIFY_WITH_TIMEOUT(labelWith(QStringLiteral("not started")), 20000);
        QVERIFY(!w->m_machine->isBusy());
        panel.hide();
        QVERIFY(!panel.client()->isRunning());
    }

    void cleanupTestCase()
    {
        w->m_receiver = nullptr;
        delete rx;
        rx = nullptr;
        QCOMPARE(MulticastHold::holders(), 0);
        w->m_dirty = false;
        w->close();
        delete w;
    }
};

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // A home of its own: what a pad sends is saved under Documents.
    QTemporaryDir home;
    qputenv("HOME", home.path().toLocal8Bit());
    qunsetenv("XDG_DOCUMENTS_DIR");
    qunsetenv("XDG_DATA_HOME");
    qunsetenv("XDG_CONFIG_HOME");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("phobicccp-test"));
    QCoreApplication::setApplicationName(QStringLiteral("test_remotemachine"));
    TestRemoteMachine t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_remotemachine.moc"
