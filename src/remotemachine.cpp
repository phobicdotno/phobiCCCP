#include "remotemachine.h"

#include <QAbstractButton>
#include <QDateTime>
#include <QFile>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QSysInfo>
#include <QTimer>
#include <QUdpSocket>
#include <QVBoxLayout>

namespace c2d {

namespace remotestatus {

static QString str(const QJsonObject &o, const char *key)
{
    return o.value(QLatin1String(key)).toString();
}

static bool flag(const QJsonObject &o, const char *key)
{
    return o.value(QLatin1String(key)).toBool();
}

// As MachineStatus.words() in Android/lib/model.dart.
QString words(const QJsonObject &s)
{
    if (!flag(s, "connected"))
        return QStringLiteral("Not connected");
    const QString state = str(s, "state"), job = str(s, "job");
    if (flag(s, "streaming"))
        return job.isEmpty() ? QStringLiteral("Cutting") : QStringLiteral("Cutting %1").arg(job);
    if (state.startsWith(QLatin1String("Alarm")))
        return QStringLiteral("Alarm: needs Initialize");
    if (!flag(s, "homed"))
        return QStringLiteral("Connected, not homed");
    if (flag(s, "busy"))
        return QStringLiteral("Busy (%1)").arg(state);
    if (state == QLatin1String("Idle"))
        return QStringLiteral("Idle, ready");
    return state.isEmpty() ? QStringLiteral("Connected") : state;
}

bool readyToCut(const QJsonObject &s)
{
    return flag(s, "connected") && flag(s, "homed") && !flag(s, "busy") && str(s, "state") == QLatin1String("Idle");
}

QString jobHeading(const QJsonObject &s, const QString &machine)
{
    const QString m = machine.toUpper();
    if (flag(s, "streaming"))
        return QStringLiteral("RUNNING ON %1").arg(m);
    const QString end = str(s, "jobEnd");
    if (end == QLatin1String("finished"))
        return QStringLiteral("FINISHED ON %1").arg(m);
    if (end == QLatin1String("stopped"))
        return QStringLiteral("STOPPED ON %1").arg(m);
    return QStringLiteral("LAST JOB ON %1").arg(m);
}

} // namespace remotestatus

namespace {

const QString kTypedKey = QStringLiteral("remoteMachine/hosts");
const QString kLastKey = QStringLiteral("remoteMachine/last");

qint64 now()
{
    return QDateTime::currentMSecsSinceEpoch();
}

QString activeMachineName(const QJsonObject &info)
{
    const QString active = info.value(QStringLiteral("activeMachine")).toString();
    for (const QJsonValue &v : info.value(QStringLiteral("machines")).toArray())
        if (v.toObject().value(QStringLiteral("id")).toString() == active)
            return v.toObject().value(QStringLiteral("name")).toString().left(60);
    return {};
}

} // namespace

// ---- the client ----------------------------------------------------------

RemoteMachine::RemoteMachine(const QString &ownId, QObject *parent) : QObject(parent), m_ownId(ownId)
{
    m_poll = new QTimer(this);
    m_poll->setInterval(kPollMs);
    connect(m_poll, &QTimer::timeout, this, &RemoteMachine::tick);
    m_discover = new QTimer(this);
    m_discover->setInterval(kDiscoverMs);
    connect(m_discover, &QTimer::timeout, this, &RemoteMachine::discoverNow);
    // Android takes sockets away in the background and pauses the app; on
    // coming back, ask again at once instead of showing stale answers.
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, &RemoteMachine::onAppState);

    m_rememberedKey = QSettings().value(kLastKey).toString();
    for (const QString &h : QSettings().value(kTypedKey).toStringList())
        addAddress(h);
}

void RemoteMachine::start()
{
    if (m_running)
        return;
    m_running = true;
    m_multicast = std::make_unique<MulticastHold>();
    rediscover();
    m_poll->start();
    m_discover->start();
    pollNow();
}

void RemoteMachine::stop()
{
    m_running = false;
    m_poll->stop();
    m_discover->stop();
    delete m_udp;
    m_udp = nullptr;
    m_multicast.reset();
}

void RemoteMachine::onAppState(Qt::ApplicationState state)
{
    if (!m_running)
        return;
    if (state == Qt::ApplicationSuspended || state == Qt::ApplicationHidden) {
        m_paused = true;
        m_poll->stop();
        m_discover->stop();
    } else if (state == Qt::ApplicationActive && m_paused) {
        m_paused = false;
        rediscover();
        pollNow();
        m_poll->start();
        m_discover->start();
    }
}

void RemoteMachine::rediscover()
{
    delete m_udp;
    m_udp = new QUdpSocket(this);
    m_udp->bind(QHostAddress::AnyIPv4, 0);
    // Maybe another network now: broadcast first again, then sweep while
    // no computer answers.
    m_heard = false;
    m_rounds = 0;
    connect(m_udp, &QUdpSocket::readyRead, this, [this] {
        for (const SendToDialog::Computer &c : SendToDialog::answers(m_udp))
            if (addFound(c))
                m_heard = true;   // not for a pad or this install
    });
    discoverNow();
}

void RemoteMachine::discoverNow()
{
    // The first round is broadcast only; while nothing answers, every
    // address of this pad's own network is asked too (as the Flutter pad does).
    // While the computer picked (or the one picked last time) is not
    // answering, it may be at a new address that broadcasts do not reach:
    // swept then too, though others answer, every kLostSweepRounds rounds.
    bool lost = m_selected < 0 || !online(m_selected);
    if (!m_rememberedKey.isEmpty()) {
        bool there = false;
        for (int i = 0; i < m_computers.size(); ++i)
            there = there || (isRemembered(m_computers[i]) && online(i));
        lost = lost || !there;
    }
    const bool sweep = m_rounds > 0 && (!m_heard || (lost && m_rounds % kLostSweepRounds == 0));
    if (m_udp)
        SendToDialog::discover(m_udp, sweep);
    ++m_rounds;
}

int RemoteMachine::indexOf(const QString &key) const
{
    for (int i = 0; i < m_computers.size(); ++i)
        if (m_computers[i].key() == key)
            return i;
    return -1;
}

int RemoteMachine::indexOfAsked(quint64 gen, const QString &key) const
{
    for (int i = 0; i < m_computers.size(); ++i)
        if (m_computers[i].gen == gen)
            return m_computers[i].key() == key ? i : -1;
    return -1;
}

QString RemoteMachine::rememberAs(const Computer &c)
{
    return !c.typed && !c.id.isEmpty() ? QStringLiteral("id:") + c.id : c.key();
}

bool RemoteMachine::isRemembered(const Computer &c) const
{
    if (m_rememberedKey.isEmpty())
        return false;
    if (m_rememberedKey.startsWith(QLatin1String("id:")))
        return !c.typed && QStringView(m_rememberedKey).mid(3) == c.id;
    return c.key() == m_rememberedKey;
}

bool RemoteMachine::addFound(const SendToDialog::Computer &f)
{
    // A pad has no machine; this install is not another computer.
    if (f.pad || (!f.id.isEmpty() && f.id == m_ownId))
        return false;
    const quint16 port = f.port ? f.port : ProjectReceiver::kPort;
    for (int i = 0; i < m_computers.size(); ++i) {
        Computer &c = m_computers[i];
        // The same install answering again, or on another of its addresses:
        // one typed in stays as typed.
        if ((!f.id.isEmpty() && c.id == f.id) || (c.address == f.address && c.port == port)) {
            if (c.id.isEmpty())
                c.id = f.id;
            if (!c.typed || c.host == c.address)
                c.host = f.host;
            // Found before, and no longer answering where it was (a new
            // address from the router, say): asked where it answers now.
            // Not while it answers: a computer on two networks answers on
            // both, and would flip between them.
            const QString was = c.key();
            const QString moved = QStringLiteral("%1:%2").arg(f.address).arg(port);
            if (!c.typed && moved != was && !online(i) && indexOf(moved) < 0) {
                c.address = f.address;
                c.port = port;
                c.info = QJsonObject();   // the old address's, stale
                c.answeredAt = 0;
                c.error.clear();
                c.asking = false;         // an answer from the old one is dropped (indexOf)
                if (m_rememberedKey == was) {   // remembered by address (before ids)
                    m_rememberedKey = rememberAs(c);
                    QSettings().setValue(kLastKey, m_rememberedKey);
                }
                if (m_running)
                    ask(i);
                emit changed();
            }
            // Picked last time, by its id: picked again wherever it answers.
            if (isRemembered(c) && m_selected != i && !online(m_selected))
                show(i);
            return true;
        }
    }
    Computer c;
    c.id = f.id;
    c.host = f.host;
    c.address = f.address;
    c.port = port;
    c.gen = ++m_gens;
    m_computers.append(c);
    if (m_selected < 0 || isRemembered(c))
        show(m_computers.size() - 1);
    ask(m_computers.size() - 1);
    emit changed();
    return true;
}

int RemoteMachine::addAddress(const QString &text)
{
    QString a = text.trimmed();
    if (a.isEmpty())
        return -1;
    quint16 port = ProjectReceiver::kPort;
    if (const int colon = a.lastIndexOf(QLatin1Char(':')); colon > 0 && a.count(QLatin1Char(':')) == 1) {
        bool ok = false;
        const int p = a.mid(colon + 1).toInt(&ok);
        if (ok && p > 0 && p < 65536) {
            port = quint16(p);
            a = a.left(colon);
        }
    }
    Computer c;
    c.address = a;
    c.host = a;
    c.port = port;
    c.typed = true;
    c.gen = ++m_gens;
    int i = indexOf(c.key());
    if (i < 0) {
        m_computers.append(c);
        i = m_computers.size() - 1;
    } else {
        m_computers[i].typed = true;
    }
    saveTyped();
    if (m_selected < 0 || isRemembered(m_computers[i]))
        show(i);
    if (m_running)
        ask(i);
    emit changed();
    return i;
}

void RemoteMachine::forget(int index)
{
    if (index < 0 || index >= m_computers.size() || !m_computers[index].typed)
        return;
    const QString was = m_selected >= 0 ? m_computers[m_selected].key() : QString();
    if (isRemembered(m_computers[index])) {
        // Not picked again should it turn up on the network.
        m_rememberedKey.clear();
        QSettings().remove(kLastKey);
    }
    m_computers.removeAt(index);
    saveTyped();
    m_selected = indexOf(was);
    if (m_selected < 0 && !m_computers.isEmpty())
        show(0);
    emit changed();
}

void RemoteMachine::saveTyped() const
{
    QStringList hosts;
    for (const Computer &c : m_computers)
        if (c.typed)
            hosts << (c.port == ProjectReceiver::kPort ? c.address : c.key());
    QSettings().setValue(kTypedKey, hosts);
}

void RemoteMachine::select(int index)
{
    if (index < 0 || index >= m_computers.size())
        return;
    m_rememberedKey = rememberAs(m_computers[index]);
    QSettings().setValue(kLastKey, m_rememberedKey);
    show(index);
}

void RemoteMachine::show(int index)
{
    if (index < 0 || index >= m_computers.size() || index == m_selected)
        return;
    m_selected = index;
    if (m_running)
        ask(index);
    emit changed();
}

const RemoteMachine::Computer *RemoteMachine::current() const
{
    return m_selected >= 0 && m_selected < m_computers.size() ? &m_computers[m_selected] : nullptr;
}

bool RemoteMachine::online(int index) const
{
    return index >= 0 && index < m_computers.size() && m_computers[index].answeredAt > 0
           && now() - m_computers[index].answeredAt < kStaleMs;
}

void RemoteMachine::pollNow()
{
    for (int i = 0; i < m_computers.size(); ++i)
        ask(i);
}

void RemoteMachine::tick()
{
    // The computer picked is asked every tick, the others now and then.
    ++m_ticks;
    for (int i = 0; i < m_computers.size(); ++i)
        if (i == m_selected || m_ticks % 4 == 0)
            ask(i);
    emit changed();   // "not answering" comes with time, not with an answer
}

void RemoteMachine::ask(int index)
{
    if (index < 0 || index >= m_computers.size() || m_computers[index].asking)
        return;
    Computer &c = m_computers[index];
    c.asking = true;
    const QString key = c.key();
    const quint64 seq = ++m_seq, gen = c.gen;
    SendToDialog::call(this, c.address, c.port, QJsonObject{{QStringLiteral("op"), QStringLiteral("info")}},
                       kInfoTimeoutMs, [this, key, seq, gen](const QJsonObject &r) {
                           const int i = indexOfAsked(gen, key);
                           if (i < 0)
                               return;   // forgotten or moved meanwhile
                           Computer &c = m_computers[i];
                           c.asking = false;
                           // Asked before a machine switch was answered: it
                           // may name the machine from before the switch.
                           if (seq < c.infoSeq)
                               return;
                           if (!r.value(QStringLiteral("ok")).toBool()) {
                               c.error = r.value(QStringLiteral("error")).toString();
                               emit changed();
                               return;
                           }
                           const QString id = r.value(QStringLiteral("id")).toString();
                           if (!id.isEmpty() && id == m_ownId) {
                               // Typed in, and it is this very install.
                               c.error = QStringLiteral("that is this pad itself");
                               emit changed();
                               return;
                           }
                           c.info = r;
                           c.infoSeq = seq;
                           c.error.clear();
                           c.answeredAt = now();
                           if (!id.isEmpty())
                               c.id = id;
                           const QString host = r.value(QStringLiteral("host")).toString().left(60);
                           if (!host.isEmpty() && (c.host.isEmpty() || c.host == c.address))
                               c.host = host;
                           emit changed();
                       });
}

bool RemoteMachine::switchMachine(const QString &id)
{
    const Computer *c = current();
    // One at a time: the answer of each is stamped as the newest news, so
    // two on their way could leave the older one shown.
    if (!c || m_switching)
        return false;
    m_switching = true;
    const QString key = c->key(), name = c->name();
    const quint64 gen = c->gen;
    SendToDialog::call(this, c->address, c->port,
                       QJsonObject{{QStringLiteral("op"), QStringLiteral("machine")}, {QStringLiteral("id"), id}},
                       30000, [this, key, name, gen](const QJsonObject &r) {
                           m_switching = false;
                           if (!r.value(QStringLiteral("ok")).toBool()) {
                               emit changed();
                               emit machineSwitched(false, QStringLiteral("Could not switch: %1")
                                                               .arg(r.value(QStringLiteral("error")).toString()));
                               return;
                           }
                           // The answer is the computer's "info" as it is now.
                           // An "info" asked before now may still be on its
                           // way with the machine from before: dropped.
                           if (const int i = indexOfAsked(gen, key); i >= 0) {
                               m_computers[i].info = r;
                               m_computers[i].infoSeq = ++m_seq;
                               m_computers[i].answeredAt = now();
                           }
                           emit machineSwitched(true, QStringLiteral("%1 now uses %2").arg(name, activeMachineName(r)));
                           emit changed();
                       });
    emit changed();
    return true;
}

bool RemoteMachine::send(const QString &name, const QByteArray &c2d, bool start, const QString &machine)
{
    const Computer *c = current();
    if (!c || m_sending || m_switching)
        return false;
    const QString active = c->info.value(QStringLiteral("activeMachine")).toString();
    if (!machine.isEmpty() && machine != active) {
        // Someone switched machines there (or another pad did) while the
        // user was asked: what they said yes to is not what would get it.
        const QString now = activeMachineName(c->info);
        emit sent(false, false, QStringLiteral("Not sent: %1 switched to %2 meanwhile. Check the machine there "
                                               "and try again.")
                                    .arg(c->name(), now.isEmpty() ? QStringLiteral("another machine") : now));
        return true;
    }
    if (c2d.size() > SendToDialog::kMaxDesign) {
        emit sent(false, false, QStringLiteral("Not sent: the design is %1 MB; at most %2 MB can be sent.")
                                    .arg(c2d.size() >> 20).arg(SendToDialog::kMaxDesign >> 20));
        return true;
    }
    const QString host = c->name();
    // To the machine in use there; a pick in the list switches it first.
    const QJsonObject req{{QStringLiteral("op"), QStringLiteral("open")},
                          {QStringLiteral("name"), name},
                          {QStringLiteral("c2d"), QString::fromLatin1(c2d.toBase64())},
                          {QStringLiteral("machine"), machine.isEmpty() ? active : machine},
                          {QStringLiteral("start"), start},
                          {QStringLiteral("from"), QSysInfo::machineHostName()}};
    m_sending = true;
    emit changed();
    SendToDialog::call(this, c->address, c->port, req, kSendTimeoutMs, [this, host, start](const QJsonObject &r) {
        m_sending = false;
        if (r.value(QStringLiteral("ok")).toBool()) {
            const bool started = r.value(QStringLiteral("started")).toBool();
            // As sendWords() in Android/lib/ui/send_dialog.dart.
            QString msg = start && started ? QStringLiteral("Cutting started on %1.").arg(host)
                                           : r.value(QStringLiteral("message")).toString();
            if (msg.isEmpty())
                msg = QStringLiteral("Sent to %1.").arg(host);
            QStringList notes;
            for (const QJsonValue &v : r.value(QStringLiteral("notes")).toArray())
                notes << v.toString();
            if (!notes.isEmpty())
                msg += QLatin1Char(' ') + notes.join(QStringLiteral("; "));
            emit sent(true, started, msg);
        } else if (r.value(QStringLiteral("unanswered")).toBool()) {
            // It may have got there and be waiting on someone at the
            // computer: sending again could open (and start) it twice.
            emit sent(false, false, QStringLiteral("No answer from %1 (%2). It may still open%3 it there; "
                                                   "check the computer before sending again.")
                                        .arg(host, r.value(QStringLiteral("error")).toString(),
                                             start ? QStringLiteral(" and start") : QString()));
        } else {
            emit sent(false, false, QStringLiteral("Not sent: %1").arg(r.value(QStringLiteral("error")).toString()));
        }
        pollNow();
        emit changed();
    });
    return true;
}

// ---- the page ------------------------------------------------------------

namespace {

QLabel *caption(const QString &text)
{
    auto *l = new QLabel(text);
    l->setTextFormat(Qt::PlainText);   // the steps heading names a machine from the network
    l->setObjectName(QStringLiteral("caption"));
    return l;
}

QLabel *wrapped(const QString &objectName = QString())
{
    auto *l = new QLabel;
    l->setWordWrap(true);
    l->setTextFormat(Qt::PlainText);
    if (!objectName.isEmpty())
        l->setObjectName(objectName);
    return l;
}

} // namespace

RemoteMachinePanel::RemoteMachinePanel(const QString &ownId, Design design, QWidget *parent)
    : QWidget(parent), m_design(std::move(design))
{
    m_client = new RemoteMachine(ownId, this);
    setObjectName(QStringLiteral("machinePage"));
    setAttribute(Qt::WA_StyledBackground);
    // The desktop machine panel's look (machinepanel.cpp).
    setStyleSheet(QStringLiteral(
        "#machinePage { background: #16181d; }"
        "QFrame#card { background: #1e2127; border-radius: 12px; }"
        "QLabel#caption { color: #8b93a1; font-size: 11px; letter-spacing: 1px; }"
        "QLabel#sub { color: #8b93a1; }"
        "QLabel#warn { color: #f2a536; }"
        "QLabel#statePill { border-radius: 10px; padding: 8px 14px; background: #16181d; "
        "  border: 1px solid #2a2e36; font-weight: bold; }"
        "QLabel#job { font-size: 19px; font-weight: bold; }"
        "QListWidget { font-size: 15px; }"
        "QListWidget::item { padding: 8px 4px; }"
        "QLineEdit { font-size: 15px; min-height: 40px; }"
        "QPushButton#start { font-size: 18px; font-weight: bold; border-radius: 12px; "
        "  background: #f2a536; color: #1a1a1a; min-height: 64px; }"
        "QPushButton#start:disabled { background: #4a3413; color: #8b93a1; }"
        "QPushButton#big { font-size: 15px; border-radius: 10px; background: #2a2e36; "
        "  border: 1px solid #3a404b; padding: 0 14px; min-height: 50px; }"
        "QPushButton#big:pressed { background: #3a4150; }"
        "QPushButton#big:disabled { color: #6a717d; border-color: #2a2e36; }"));

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(14, 14, 14, 14);
    row->setSpacing(12);
    auto card = [row] {
        auto *f = new QFrame;
        f->setObjectName(QStringLiteral("card"));
        auto *v = new QVBoxLayout(f);
        v->setContentsMargins(18, 18, 18, 18);
        v->setSpacing(8);
        row->addWidget(f, 1);
        return v;
    };

    // Left: the phobiCCCPs on the network, the one picked with its machines.
    QVBoxLayout *left = card();
    left->addWidget(caption(QStringLiteral("PHOBICCCP ON THIS NETWORK")));
    m_list = new QListWidget;
    m_list->setMinimumHeight(110);
    left->addWidget(m_list, 1);
    m_status = wrapped(QStringLiteral("statePill"));
    left->addWidget(m_status);
    m_machines = new QListWidget;
    m_machines->setMinimumHeight(80);
    m_machines->setMaximumHeight(170);
    left->addWidget(m_machines);
    m_allow = wrapped(QStringLiteral("sub"));
    left->addWidget(m_allow);
    auto *addRow = new QHBoxLayout;
    m_address = new QLineEdit;
    m_address->setPlaceholderText(QStringLiteral("Not listed? Its name or address"));
    auto *add = new QPushButton(QStringLiteral("Add"));
    add->setObjectName(QStringLiteral("big"));
    m_forget = new QPushButton(QStringLiteral("Forget"));
    m_forget->setObjectName(QStringLiteral("big"));
    addRow->addWidget(m_address, 1);
    addRow->addWidget(add);
    addRow->addWidget(m_forget);
    left->addLayout(addRow);

    // Right: the design open here, where it goes, and the computer's job.
    QVBoxLayout *right = card();
    right->addWidget(caption(QStringLiteral("JOB")));
    m_job = wrapped(QStringLiteral("job"));
    right->addWidget(m_job);
    m_toolpaths = wrapped(QStringLiteral("sub"));
    right->addWidget(m_toolpaths);
    m_to = wrapped(QStringLiteral("statePill"));
    right->addWidget(m_to);
    m_warn = wrapped(QStringLiteral("warn"));
    right->addWidget(m_warn);
    m_stepsHead = caption(QString());
    right->addWidget(m_stepsHead);
    m_steps = new QListWidget;
    m_steps->setIconSize(QSize(52, 52));
    m_steps->setSelectionMode(QAbstractItemView::NoSelection);
    m_steps->setFocusPolicy(Qt::NoFocus);
    right->addWidget(m_steps, 1);
    m_result = wrapped();
    right->addWidget(m_result);
    m_start = new QPushButton(QStringLiteral("▶  Start"));
    m_start->setObjectName(QStringLiteral("start"));
    right->addWidget(m_start);
    m_send = new QPushButton(QStringLiteral("Send without starting"));
    m_send->setObjectName(QStringLiteral("big"));
    right->addWidget(m_send);

    connect(m_client, &RemoteMachine::changed, this, &RemoteMachinePanel::refresh);
    connect(m_client, &RemoteMachine::machineSwitched, this, [this](bool, const QString &msg) {
        m_result->setText(msg);
    });
    connect(m_client, &RemoteMachine::sent, this, [this](bool, bool, const QString &msg) {
        m_result->setText(msg);
        refresh();
    });
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int r) {
        if (r >= 0)
            m_client->select(r);
    });
    connect(m_machines, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
        const QString id = it->data(Qt::UserRole).toString();
        const RemoteMachine::Computer *c = m_client->current();
        if (!c || !m_client->online(m_client->selected())
            || id == c->info.value(QStringLiteral("activeMachine")).toString())
            return;
        m_result->setText(QStringLiteral("Switching %1 to %2…").arg(c->name(), it->text()));
        m_client->switchMachine(id);
    });
    connect(add, &QPushButton::clicked, this, &RemoteMachinePanel::addTyped);
    connect(m_address, &QLineEdit::returnPressed, this, &RemoteMachinePanel::addTyped);
    connect(m_forget, &QPushButton::clicked, this, [this] { m_client->forget(m_client->selected()); });
    connect(m_start, &QPushButton::clicked, this, [this] { send(true); });
    connect(m_send, &QPushButton::clicked, this, [this] { send(false); });
    refresh();
}

void RemoteMachinePanel::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    m_client->start();
    refresh();   // the design open here may have changed meanwhile
}

void RemoteMachinePanel::hideEvent(QHideEvent *e)
{
    QWidget::hideEvent(e);
    m_client->stop();
}

void RemoteMachinePanel::addTyped()
{
    const int i = m_client->addAddress(m_address->text());
    if (i < 0)
        return;
    m_client->select(i);
    m_address->clear();
}

void RemoteMachinePanel::refreshComputers()
{
    const QList<RemoteMachine::Computer> &list = m_client->computers();
    QSignalBlocker block(m_list);
    while (m_list->count() > list.size())
        delete m_list->takeItem(m_list->count() - 1);
    for (int i = 0; i < list.size(); ++i) {
        const RemoteMachine::Computer &c = list[i];
        const bool online = m_client->online(i);
        QStringList sub;
        if (c.address != c.name())
            sub << c.address;
        const QString version = c.info.value(QStringLiteral("version")).toString();
        if (!version.isEmpty())
            sub << QStringLiteral("phobiCCCP %1").arg(version);
        if (!online)
            sub << (c.error.isEmpty() || c.answeredAt > 0 ? QStringLiteral("not answering")
                                                          : QStringLiteral("not answering: %1").arg(c.error));
        const QString text = QStringLiteral("%1 %2\n%3")
                                 .arg(online ? QStringLiteral("●") : QStringLiteral("○"), c.name(),
                                      sub.join(QStringLiteral(" · ")));
        if (i >= m_list->count())
            m_list->addItem(text);
        else if (m_list->item(i)->text() != text)
            m_list->item(i)->setText(text);
        m_list->item(i)->setForeground(online ? palette().color(QPalette::Text) : QColor(0x8b, 0x93, 0xa1));
    }
    m_list->setCurrentRow(m_client->selected());
}

void RemoteMachinePanel::refreshMachines(const RemoteMachine::Computer *c)
{
    const QJsonArray machines = c ? c->info.value(QStringLiteral("machines")).toArray() : QJsonArray();
    const QString key = c ? c->key() + c->info.value(QStringLiteral("activeMachine")).toString() : QString();
    if (machines == m_shownMachines && key == m_shownKey)
        return;
    m_shownMachines = machines;
    m_shownKey = key;
    m_machines->clear();
    const QString active = c ? c->info.value(QStringLiteral("activeMachine")).toString() : QString();
    for (const QJsonValue &v : machines) {
        const QJsonObject m = v.toObject();
        const QString id = m.value(QStringLiteral("id")).toString();
        QString text = (id == active ? QStringLiteral("◉  ") : QStringLiteral("○  "))
                       + m.value(QStringLiteral("name")).toString().left(60);
        const QString size = m.value(QStringLiteral("size")).toString();
        if (!size.isEmpty())
            text += QStringLiteral("    ") + size;
        auto *it = new QListWidgetItem(text, m_machines);
        it->setData(Qt::UserRole, id);
        if (id == active) {
            QFont f = it->font();
            f.setBold(true);
            it->setFont(f);
            it->setForeground(QColor(0xf2, 0xa5, 0x36));
        }
    }
}

void RemoteMachinePanel::refreshSteps(const RemoteMachine::Computer *c, const QString &machine)
{
    const QJsonObject status = c ? c->info.value(QStringLiteral("status")).toObject() : QJsonObject();
    const QJsonArray steps = status.value(QStringLiteral("steps")).toArray();
    m_stepsHead->setVisible(!steps.isEmpty());
    m_steps->setVisible(!steps.isEmpty());
    if (!steps.isEmpty())
        m_stepsHead->setText(remotestatus::jobHeading(status, machine.isEmpty() ? c->name() : machine));
    if (steps == m_shownSteps)
        return;
    m_shownSteps = steps;
    m_steps->clear();
    for (const QJsonValue &v : steps) {
        const QJsonObject s = v.toObject();
        const QString state = s.value(QStringLiteral("state")).toString();
        const QString name = s.value(QStringLiteral("name")).toString().left(80);
        const QString detail = s.value(QStringLiteral("detail")).toString().left(80);
        QString text = detail.isEmpty() ? name : QStringLiteral("%1  —  %2").arg(name, detail);
        const QString time = s.value(QStringLiteral("time")).toString().left(20);
        if (!time.isEmpty())
            text += QStringLiteral("   ") + time;
        const bool change = s.value(QStringLiteral("kind")).toString() == QLatin1String("change");
        const double done = qBound(0.0, s.value(QStringLiteral("done")).toDouble(), 1.0);
        if (!change && done > 0 && done < 1)
            text += QStringLiteral("   %1%").arg(int(done * 100));
        auto *it = new QListWidgetItem(text, m_steps);
        // As the pad's job list: done grey, running amber, stopped red.
        it->setForeground(state == QLatin1String("done")      ? QColor(0x6a, 0x71, 0x7d)
                          : state == QLatin1String("run")     ? QColor(0xf2, 0xa5, 0x36)
                          : state == QLatin1String("stopped") ? QColor(0xe0, 0x6c, 0x6c)
                                                              : QColor(0xd6, 0xdb, 0xe3));
        if (state == QLatin1String("run")) {
            QFont f = it->font();
            f.setBold(true);
            it->setFont(f);
        }
        const QString png = s.value(QStringLiteral("preview")).toString();
        if (!change && !png.isEmpty() && png.size() < 2 * 1024 * 1024) {
            QPixmap pm;
            if (pm.loadFromData(QByteArray::fromBase64(png.toLatin1()), "PNG"))
                it->setIcon(QIcon(pm));
        }
    }
}

void RemoteMachinePanel::refresh()
{
    refreshComputers();
    const int sel = m_client->selected();
    const RemoteMachine::Computer *c = m_client->current();
    const bool online = m_client->online(sel);
    const QJsonObject info = c ? c->info : QJsonObject();
    const QJsonObject status = info.value(QStringLiteral("status")).toObject();
    const bool allowStart = info.value(QStringLiteral("allowStart")).toBool();
    const QString machine = activeMachineName(info);
    m_forget->setVisible(c && c->typed);

    if (!c) {
        m_status->setText(QStringLiteral("Looking for phobiCCCP… Open it on a computer on the same network, with "
                                         "File > Receive from Syntexophobic and pads on. Over Tailscale or another "
                                         "subnet, add it by name or address below."));
    } else if (info.isEmpty()) {
        m_status->setText(c->error.isEmpty() ? QStringLiteral("Asking %1…").arg(c->name())
                                             : QStringLiteral("%1 does not answer: %2").arg(c->name(), c->error));
    } else {
        const QString doc = info.value(QStringLiteral("document")).toString();
        m_status->setText(remotestatus::words(status) + (doc.isEmpty() ? QString() : QStringLiteral(" · ") + doc)
                          + (online ? QString() : QStringLiteral(" (not answering)")));
    }
    m_status->setStyleSheet(remotestatus::readyToCut(status) && online ? QStringLiteral("color: #6fcf7f;")
                                                                        : QString());
    m_machines->setVisible(!info.isEmpty());
    m_machines->setEnabled(online && !m_client->isSending() && !m_client->isSwitching());
    refreshMachines(c);
    m_allow->setVisible(!info.isEmpty());
    m_allow->setText(allowStart ? QStringLiteral("Pads may start cutting here.")
                                : QStringLiteral("Starting from a pad is off here (Machine > Pads may start cutting)."));

    const QString name = m_design.name ? m_design.name() : QString();
    const int toolpaths = m_design.toolpaths ? m_design.toolpaths() : 0;
    const bool haveDesign = !name.isEmpty();
    m_job->setText(haveDesign ? name : QStringLiteral("No design open"));
    m_toolpaths->setVisible(haveDesign);
    m_toolpaths->setText(toolpaths == 1 ? QStringLiteral("1 toolpath") : QStringLiteral("%1 toolpaths").arg(toolpaths));
    m_to->setText(!c ? QStringLiteral("To: —")
                     : QStringLiteral("To: %1%2").arg(c->name(), machine.isEmpty() ? QString()
                                                                                    : QStringLiteral(" · ") + machine));
    // As the Flutter pad's warnings, the first that applies.
    QString warn;
    if (!haveDesign)
        warn = QStringLiteral("Open a design first (File > Open).");
    else if (!c || !online)
        warn = QStringLiteral("No phobiCCCP is answering right now.");
    else if (toolpaths == 0)
        warn = QStringLiteral("This design has no toolpaths, so there is nothing to cut.");
    else if (!allowStart)
        warn = QStringLiteral("Start is off on %1 (Machine > Pads may start cutting there); Send still opens it.")
                   .arg(c->name());
    else if (!remotestatus::readyToCut(status))
        warn = QStringLiteral("The machine is not ready (%1); the design will be opened, not started.")
                   .arg(remotestatus::words(status));
    m_warn->setText(warn);
    m_warn->setVisible(!warn.isEmpty());
    refreshSteps(c, machine);

    const bool canSend = haveDesign && c && online && !m_client->isSending()
                         && !m_client->isSwitching();
    m_send->setEnabled(canSend);
    // Honours the computer's gate: with starting from pads off there, only
    // Send is offered (and the computer refuses a start anyway).
    m_start->setEnabled(canSend && toolpaths > 0 && allowStart);
    if (m_client->isSending())
        m_result->setText(QStringLiteral("Sending… (phobiCCCP may ask about unsaved work first)"));
}

void RemoteMachinePanel::send(bool start)
{
    const RemoteMachine::Computer *c = m_client->current();
    if (!c || !m_design.makeCopy || m_client->isSending() || m_client->isSwitching())
        return;
    const QString name = m_design.name ? m_design.name() : QStringLiteral("Design");
    const QString host = c->name(), key = c->key();
    // The machine the user sees now (and is asked about): the design goes
    // to that one or nowhere, whatever the polls bring meanwhile.
    const QString machineId = c->info.value(QStringLiteral("activeMachine")).toString();
    if (start) {
        QString machine = activeMachineName(c->info);
        if (machine.isEmpty())
            machine = QStringLiteral("the machine");
        QMessageBox box(QMessageBox::Warning, QStringLiteral("Start cutting now?"),
                        QStringLiteral("“%1” will start on %2 at %3 as soon as it arrives, and the spindle will "
                                       "spin up. Make sure the stock is clamped, the right bit is in, the zero is "
                                       "set and nobody is near the machine.")
                            .arg(name, machine, host),
                        QMessageBox::NoButton, this);
        box.setTextFormat(Qt::PlainText);   // the computer and machine names come from the network
        QAbstractButton *go = box.addButton(QStringLiteral("Send and start"), QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() != go)
            return;
        // Another computer may have been picked, or this one gone, while the
        // question was up.
        const RemoteMachine::Computer *now = m_client->current();
        if (!now || now->key() != key || !m_client->online(m_client->selected())) {
            m_result->setText(QStringLiteral("Not sent: %1 stopped answering.").arg(host));
            return;
        }
    }
    QString err;
    const QString path = m_design.makeCopy(&err);
    QFile f(path);
    if (path.isEmpty() || !f.open(QIODevice::ReadOnly)) {
        m_result->setText(QStringLiteral("Could not save a copy to send: %1")
                              .arg(err.isEmpty() ? f.errorString() : err));
        return;
    }
    const QByteArray data = f.readAll();
    f.close();
    QFile::remove(path);
    if (!m_client->send(name, data, start, machineId))
        m_result->setText(m_client->isSwitching()
                              ? QStringLiteral("Not sent: the machine is being switched. Try again when it is done.")
                              : QStringLiteral("Not sent: %1 is busy or gone.").arg(host));
    refresh();
}

} // namespace c2d
