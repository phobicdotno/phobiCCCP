// The pad's Machine page (src/remotemachine.h) against a real main window's
// pad link (MainWindow::onPadRequest behind a ProjectReceiver on loopback):
// it polls "info" for the machine's status, switches machines, sends the
// open design to be opened, and a start is refused while the computer does
// not let pads start cutting. Also the addresses discovery sweeps.
#define private public
#include "../src/mainwindow.h"
#include "../src/projectreceiver.h"
#undef private
#include "../src/machinepanel.h"
#include "../src/machineprofiles.h"
#include "../src/remotemachine.h"
#include "../src/sendtodialog.h"

#include <QApplication>
#include <QDir>
#include <QJsonDocument>
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
#include <QUdpSocket>

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

    void remembersTheUsersPick()
    {
        // Typed-in computers restored at start are shown, not taken for the
        // user's pick: the computer picked last is picked again when it turns up.
        forgetHosts();
        const QString lastKey = QStringLiteral("remoteMachine/last");
        const QString found = QStringLiteral("192.0.2.9:%1").arg(ProjectReceiver::kPort);
        QSettings().setValue(lastKey, found);
        QSettings().setValue(QStringLiteral("remoteMachine/hosts"),
                             QStringList{QStringLiteral("192.0.2.7"), QStringLiteral("192.0.2.8")});
        {
            RemoteMachine rm(QStringLiteral("pad-1"));
            QCOMPARE(rm.computers().size(), 2);
            QCOMPARE(rm.selected(), 0);
            QCOMPARE(QSettings().value(lastKey).toString(), found);
            SendToDialog::Computer f;
            f.id = QStringLiteral("o");
            f.address = QStringLiteral("192.0.2.9");
            f.host = QStringLiteral("shop-pc");
            rm.addFound(f);
            QCOMPARE(rm.current()->host, QStringLiteral("shop-pc"));
            // The user's pick is what is remembered.
            rm.select(1);
            QCOMPARE(QSettings().value(lastKey).toString(), QStringLiteral("192.0.2.8:%1").arg(ProjectReceiver::kPort));
        }
        {
            RemoteMachine rm(QStringLiteral("pad-1"));
            QCOMPARE(rm.current()->address, QStringLiteral("192.0.2.8"));
            // Forgotten: no longer remembered, nor picked when it turns up again.
            rm.forget(rm.selected());
            QVERIFY(!QSettings().contains(lastKey));
            QCOMPARE(rm.current()->address, QStringLiteral("192.0.2.7"));
            SendToDialog::Computer f;
            f.id = QStringLiteral("x");
            f.address = QStringLiteral("192.0.2.8");
            rm.addFound(f);
            QCOMPARE(rm.current()->address, QStringLiteral("192.0.2.7"));
            QVERIFY(!QSettings().contains(lastKey));
            // Picking the one already shown remembers it too (Add does so).
            rm.select(rm.selected());
            QCOMPARE(QSettings().value(lastKey).toString(), QStringLiteral("192.0.2.7:%1").arg(ProjectReceiver::kPort));
        }
        forgetHosts();
    }

    void foundComputerMoves()
    {
        forgetHosts();
        RemoteMachine rm(QStringLiteral("me"));
        SendToDialog::Computer self, pad, pc;
        self.id = QStringLiteral("me");
        self.address = QStringLiteral("192.0.2.2");
        pad.id = QStringLiteral("p");
        pad.address = QStringLiteral("192.0.2.3");
        pad.pad = true;
        // Not computers found: they must not stop the sweep (m_heard).
        QVERIFY(!rm.addFound(self));
        QVERIFY(!rm.addFound(pad));
        pc.id = QStringLiteral("pc");
        pc.address = QStringLiteral("192.0.2.4");
        pc.host = QStringLiteral("shop-pc");
        QVERIFY(rm.addFound(pc));
        QCOMPARE(rm.computers().size(), 1);
        // A new address from the router: it is not answering where it was,
        // so it is asked where it answers now.
        pc.address = QStringLiteral("192.0.2.40");
        QVERIFY(rm.addFound(pc));
        QCOMPARE(rm.computers().size(), 1);
        QCOMPARE(rm.computers().first().address, QStringLiteral("192.0.2.40"));
        QVERIFY(rm.computers().first().info.isEmpty());
        // One typed in keeps the address it was given.
        rm.addAddress(QStringLiteral("192.0.2.5"));
        SendToDialog::Computer typed;
        typed.id = QStringLiteral("t");
        typed.address = QStringLiteral("192.0.2.5");
        rm.addFound(typed);
        typed.address = QStringLiteral("192.0.2.50");
        rm.addFound(typed);
        QCOMPARE(rm.computers().size(), 2);
        QCOMPARE(rm.computers().at(1).address, QStringLiteral("192.0.2.5"));
        forgetHosts();
    }

    void staleInfoAfterSwitchDropped()
    {
        // An "info" asked before a machine switch, answered after it, names
        // the machine from before: it must not undo the switch on the page.
        QTcpServer fake;
        QVERIFY(fake.listen(QHostAddress::LocalHost, 0));
        auto answer = [](const QString &machine) {
            return QJsonObject{{"ok", true}, {"id", "fake"}, {"activeMachine", machine},
                               {"machines", QJsonArray{QJsonObject{{"id", "old"}, {"name", "Old"}},
                                                       QJsonObject{{"id", "new"}, {"name", "New"}}}}};
        };
        connect(&fake, &QTcpServer::newConnection, &fake, [&fake, answer] {
            QTcpSocket *s = fake.nextPendingConnection();
            connect(s, &QTcpSocket::readyRead, s, [s, answer] {
                const QByteArray all = s->peek(1 << 20);
                if (all.count('\n') < 2)
                    return;
                const QJsonObject req = QJsonDocument::fromJson(all.split('\n').at(1)).object();
                s->readAll();
                if (req.value("op").toString() == QLatin1String("info")) {
                    QTimer::singleShot(800, s, [s, answer] {
                        s->write(QJsonDocument(answer("old")).toJson(QJsonDocument::Compact) + '\n');
                    });
                } else {
                    s->write(QJsonDocument(answer("new")).toJson(QJsonDocument::Compact) + '\n');
                }
            });
        });
        forgetHosts();
        RemoteMachine rm(QStringLiteral("pad-1"));
        rm.addAddress(QStringLiteral("127.0.0.1:%1").arg(fake.serverPort()));
        rm.pollNow();   // on its way, slowly
        QSignalSpy switched(&rm, &RemoteMachine::machineSwitched);
        rm.switchMachine(QStringLiteral("new"));
        QVERIFY(switched.wait(3000));
        QCOMPARE(rm.current()->info.value("activeMachine").toString(), QStringLiteral("new"));
        QTRY_VERIFY_WITH_TIMEOUT(!rm.current()->asking, 3000);   // the slow answer came
        QCOMPARE(rm.current()->info.value("activeMachine").toString(), QStringLiteral("new"));
        forgetHosts();
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

    void startRefusedWhenMachineChanged()
    {
        // The user said yes to a start on one machine; the computer has gone
        // over to another since: nothing is sent, and the pad says why.
        forgetHosts();
        RemoteMachine rm(QStringLiteral("pad-1"));
        rm.addAddress(address());
        rm.start();
        QTRY_VERIFY_WITH_TIMEOUT(rm.current() && !rm.current()->info.isEmpty(), 5000);
        QCOMPARE(rm.current()->info.value("activeMachine").toString(), a);
        const QString before = w->m_doc.filePath();
        QSignalSpy sent(&rm, &RemoteMachine::sent);
        QVERIFY(rm.send(QStringLiteral("Elsewhere"), design(), true, b));
        QCOMPARE(sent.size(), 1);   // at once, nothing on its way
        QVERIFY(!rm.isSending());
        QCOMPARE(sent.at(0).at(0).toBool(), false);
        const QString msg = sent.at(0).at(2).toString();
        QVERIFY2(msg.startsWith(QStringLiteral("Not sent")) && msg.contains(QStringLiteral("switched")), qPrintable(msg));
        QTest::qWait(300);
        QCOMPARE(w->m_doc.filePath(), before);
        QCOMPARE(activeMachine(), a);
    }

    void relistenKeepsPads()
    {
        // Back in front with a pad connected and the sockets gone: the pad is
        // kept and only the sockets are listened on again.
        QTcpSocket pad;
        pad.connectToHost(QHostAddress::LocalHost, rx->port());
        QVERIFY(pad.waitForConnected(3000));
        pad.write("PHOBICCCP-API 1\n");
        QTRY_VERIFY_WITH_TIMEOUT(!rx->isIdle(), 3000);
        const quint16 port = rx->port();
        rx->m_tcp->close();
        rx->m_udp->close();
        QVERIFY(!rx->isHealthy());
        w->relisten(true);
        QCOMPARE(w->m_receiver, rx);
        QVERIFY(rx->isHealthy());
        QCOMPARE(rx->port(), port);
        pad.write("{\"op\":\"info\"}\n");
        QTRY_VERIFY_WITH_TIMEOUT(pad.canReadLine(), 5000);
        QVERIFY(QJsonDocument::fromJson(pad.readLine()).object().value("ok").toBool());
        // And new connections come in on the port listened on again.
        forgetHosts();
        RemoteMachine rm(QStringLiteral("pad-1"));
        rm.addAddress(address());
        rm.start();
        QTRY_VERIFY_WITH_TIMEOUT(rm.current() && !rm.current()->info.isEmpty(), 5000);
        rm.stop();
        pad.abort();
        QTRY_VERIFY_WITH_TIMEOUT(rx->isIdle(), 3000);
        forgetHosts();
    }

    void requestIdsNeverReused()
    {
        // A receiver started again must not give out an id an answer still on
        // its way for the old one is meant for.
        QList<int> ids;
        auto ask = [&ids](ProjectReceiver &r) {
            const auto conn = connect(&r, &ProjectReceiver::request, &r,
                                      [&ids](int id, const QJsonObject &, const QString &) { ids << id; });
            QTcpSocket s;
            s.connectToHost(QHostAddress::LocalHost, r.port());
            s.waitForConnected(3000);
            s.write("PHOBICCCP-API 1\n{\"op\":\"x\"}\n");
            const int had = ids.size();
            QTRY_VERIFY_WITH_TIMEOUT(ids.size() > had, 3000);
            disconnect(conn);
        };
        {
            ProjectReceiver one;
            QVERIFY(one.start(0));
            ask(one);
        }
        ProjectReceiver two;
        QVERIFY(two.start(0));
        ask(two);
        QCOMPARE(ids.size(), 2);
        QVERIFY2(ids.at(1) > ids.at(0), qPrintable(QStringLiteral("%1 then %2").arg(ids.at(0)).arg(ids.at(1))));
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
        // Switched to another machine there while the question was up:
        // refused, nothing sent, and the page says so.
        QTimer::singleShot(300, [this] {
            QString why;
            w->switchMachine(b, &why);
            QTimer::singleShot(2 * RemoteMachine::kPollMs + 500, [] {
                if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                    for (QAbstractButton *bt : box->buttons())
                        if (bt->text().remove(QLatin1Char('&')) == QLatin1String("Send and start"))
                            bt->click();
            });
        });
        const QString before = w->m_doc.filePath();
        start->click();
        QVERIFY(!panel.client()->isSending());
        QVERIFY(labelWith(QStringLiteral("switched to Router B")));
        QCOMPARE(w->m_doc.filePath(), before);
        {
            QString why;
            QVERIFY(w->switchMachine(a, &why));
        }
        QTRY_COMPARE_WITH_TIMEOUT(panel.client()->current()->info.value("activeMachine").toString(), a, 5000);
        // A copy shared (or being saved) meanwhile is not the one a send
        // reads and removes.
        QDir(MainWindow::shareDir()).removeRecursively();
        w->onShareDesign();
        const QStringList shared = QDir(MainWindow::shareDir()).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QCOMPARE(shared.size(), 1);
        const QString sharedCopy = QDir(MainWindow::shareDir() + QLatin1Char('/') + shared.first())
                                       .filePath(QFileInfo(w->m_doc.filePath()).completeBaseName() + QStringLiteral(".c2d"));
        QVERIFY2(QFileInfo::exists(sharedCopy), qPrintable(sharedCopy));
        asked.clear();
        answer(QStringLiteral("Send and start"));
        start->click();
        QVERIFY(asked.contains(QStringLiteral("Bracket")));
        QVERIFY(panel.client()->isSending());
        // The machine here is not connected, so it opens but does not start.
        QTRY_VERIFY_WITH_TIMEOUT(labelWith(QStringLiteral("not started")), 20000);
        QVERIFY(!w->m_machine->isBusy());
        QVERIFY(QFileInfo::exists(sharedCopy));
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
