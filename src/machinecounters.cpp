#include "machinecounters.h"
#include "shopdialog.h"
#include "toollibrary.h"

#include <QDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QScroller>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QtMath>
#include <QVBoxLayout>
#include <algorithm>

namespace c2d::machinecounters {

void Counters::markDone(int i)
{
    if (i >= 0 && i < reminders.size())
        reminders[i].doneAtSec = basisSec(reminders[i]);
}

QVector<Reminder> defaultReminders()
{
    return {
        {QStringLiteral("brushes"), QStringLiteral("Router brushes"), true, 50, 0},
        {QStringLiteral("wheels"), QStringLiteral("V-wheels / belts check"), false, 100, 0},
        {QStringLiteral("screws"), QStringLiteral("Lubricate lead screws"), false, 40, 0},
    };
}

bool spindleRunning(const Sample &s)
{
    if (s.spindleReported)
        return s.spindleRpm > 0;
    return s.jobSpindleOn;
}

bool moving(const QString &state)
{
    return state == QLatin1String("Run") || state == QLatin1String("Jog");
}

void accumulate(Counters &c, double dtSec, const Sample &s)
{
    const double dt = std::clamp(dtSec, 0.0, 5.0);
    if (dt <= 0)
        return;
    const bool spin = spindleRunning(s);
    if (spin)
        c.spindleSec += dt;
    if (moving(s.state))
        c.motionSec += dt;
    if (spin && s.state == QLatin1String("Run") && !s.bitId.isEmpty()) {
        c.bitSec[s.bitId] += dt;
        if (!s.bitName.isEmpty())
            c.bitName[s.bitId] = s.bitName;
    }
}

QString dueLine(const Counters &c)
{
    QStringList parts;
    for (const Reminder &r : c.reminders) {
        if (!c.due(r))
            continue;
        QString n = r.name;
        if (!n.isEmpty())
            n[0] = n.at(0).toLower();
        parts << QStringLiteral("%1 (%2 h)").arg(n).arg(qFloor(c.sinceHours(r)));
    }
    return parts.isEmpty() ? QString() : QStringLiteral("Due: ") + parts.join(QStringLiteral(" · "));
}

static QString group(const QString &profileId)
{
    return QStringLiteral("counters/%1").arg(profileId.isEmpty() ? QStringLiteral("m1") : profileId);
}

Counters load(QSettings &s, const QString &profileId)
{
    Counters c;
    s.beginGroup(group(profileId));
    c.spindleSec = s.value(QStringLiteral("spindleSec"), 0.0).toDouble();
    c.motionSec = s.value(QStringLiteral("motionSec"), 0.0).toDouble();
    const QVariantMap bits = s.value(QStringLiteral("bits")).toMap();
    for (auto it = bits.cbegin(); it != bits.cend(); ++it)
        c.bitSec.insert(it.key(), it.value().toDouble());
    const QVariantMap names = s.value(QStringLiteral("bitNames")).toMap();
    for (auto it = names.cbegin(); it != names.cend(); ++it)
        c.bitName.insert(it.key(), it.value().toString());
    const QVariantList rem = s.value(QStringLiteral("reminders")).toList();
    s.endGroup();
    c.reminders = defaultReminders();
    // Stored ones keep their interval and last Done; a default not stored yet
    // starts from zero.
    for (const QVariant &v : rem) {
        const QVariantMap m = v.toMap();
        const QString key = m.value(QStringLiteral("key")).toString();
        auto it = std::find_if(c.reminders.begin(), c.reminders.end(),
                               [&](const Reminder &r) { return r.key == key; });
        if (it == c.reminders.end())
            continue;
        it->everyHours = m.value(QStringLiteral("every"), it->everyHours).toDouble();
        it->doneAtSec = m.value(QStringLiteral("doneAt"), 0.0).toDouble();
    }
    return c;
}

void save(QSettings &s, const QString &profileId, const Counters &c)
{
    s.beginGroup(group(profileId));
    s.setValue(QStringLiteral("spindleSec"), c.spindleSec);
    s.setValue(QStringLiteral("motionSec"), c.motionSec);
    QVariantMap bits, names;
    for (auto it = c.bitSec.cbegin(); it != c.bitSec.cend(); ++it)
        bits.insert(it.key(), it.value());
    for (auto it = c.bitName.cbegin(); it != c.bitName.cend(); ++it)
        names.insert(it.key(), it.value());
    s.setValue(QStringLiteral("bits"), bits);
    s.setValue(QStringLiteral("bitNames"), names);
    QVariantList rem;
    for (const Reminder &r : c.reminders)
        rem << QVariantMap{{QStringLiteral("key"), r.key},
                           {QStringLiteral("every"), r.everyHours},
                           {QStringLiteral("doneAt"), r.doneAtSec}};
    s.setValue(QStringLiteral("reminders"), rem);
    s.endGroup();
}

void forget(QSettings &s, const QString &profileId)
{
    s.remove(group(profileId));
}

QString hoursText(double sec)
{
    if (sec < 3600)
        return QStringLiteral("%1 min").arg(qRound(sec / 60));
    return QStringLiteral("%1 h").arg(sec / 3600, 0, 'f', 1);
}

static QLabel *bold(const QString &text, QWidget *parent, double grow = 3)
{
    auto *l = new QLabel(text, parent);
    QFont f = l->font();
    f.setBold(true);
    f.setPointSizeF(f.pointSizeF() + grow);
    l->setFont(f);
    return l;
}

bool showDialog(QWidget *parent, Counters &c, const QString &machineName)
{
    bool changed = false;
    QDialog dlg(parent);
    dlg.setWindowTitle(QStringLiteral("Counters"));
    dlg.resize(1000, 700);
    auto *v = new QVBoxLayout(&dlg);
    v->addWidget(bold(machineName.isEmpty() ? QStringLiteral("Machine hours")
                                            : QStringLiteral("%1 — hours").arg(machineName), &dlg));
    auto *totals = new QLabel(QStringLiteral("Spindle on: %1      Moving: %2")
                                  .arg(hoursText(c.spindleSec), hoursText(c.motionSec)), &dlg);
    QFont tf = totals->font();
    tf.setPointSizeF(tf.pointSizeF() + 2);
    totals->setFont(tf);
    v->addWidget(totals);

    // Service reminders: hours since Done against the interval.
    v->addWidget(bold(QStringLiteral("Service"), &dlg, 1));
    auto *grid = new QGridLayout;
    QVector<QLabel *> since(c.reminders.size());
    auto sinceText = [&c](int i) {
        const Reminder &r = c.reminders.at(i);
        return QStringLiteral("%1 h since done (%2 hours)")
            .arg(c.sinceHours(r), 0, 'f', 1)
            .arg(r.spindleBased ? QStringLiteral("spindle") : QStringLiteral("moving"));
    };
    auto paint = [&](int i) {
        since[i]->setText(sinceText(i));
        since[i]->setStyleSheet(c.due(c.reminders.at(i)) ? QStringLiteral("color: #f2a536; font-weight: bold;")
                                                          : QString());
    };
    for (int i = 0; i < c.reminders.size(); ++i) {
        auto *name = new QLabel(c.reminders.at(i).name, &dlg);
        since[i] = new QLabel(&dlg);
        auto *every = new QSpinBox(&dlg);
        every->setRange(1, 5000);
        every->setPrefix(QStringLiteral("every "));
        every->setSuffix(QStringLiteral(" h"));
        every->setValue(qRound(c.reminders.at(i).everyHours));
        every->setMinimumSize(170, 52);
        every->setToolTip(QStringLiteral("How many hours between services"));
        auto *done = new QPushButton(QStringLiteral("Done"), &dlg);
        done->setObjectName(QStringLiteral("big"));
        done->setMinimumSize(120, 52);
        done->setToolTip(QStringLiteral("Serviced now: the count starts again from zero"));
        QObject::connect(every, &QSpinBox::valueChanged, &dlg, [&, i](int h) {
            c.reminders[i].everyHours = h;
            changed = true;
            paint(i);
        });
        QObject::connect(done, &QPushButton::clicked, &dlg, [&, i] {
            c.markDone(i);
            changed = true;
            paint(i);
        });
        paint(i);
        grid->addWidget(name, i, 0);
        grid->addWidget(since[i], i, 1);
        grid->addWidget(every, i, 2);
        grid->addWidget(done, i, 3);
    }
    grid->setColumnStretch(1, 1);
    v->addLayout(grid);

    // Per-bit cutting time, the 20 most used, named from the tool library.
    v->addWidget(bold(QStringLiteral("Cutting time per bit"), &dlg, 1));
    QVector<QPair<QString, double>> bits;
    for (auto it = c.bitSec.cbegin(); it != c.bitSec.cend(); ++it)
        bits.append({it.key(), it.value()});
    std::sort(bits.begin(), bits.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
    if (bits.size() > 20)
        bits.resize(20);
    ToolLibrary lib;
    lib.loadDefault();
    auto *t = new QTableWidget(int(bits.size()), 2, &dlg);
    t->setObjectName(QStringLiteral("bitTable"));
    t->setHorizontalHeaderLabels({QStringLiteral("Bit"), QStringLiteral("Cutting")});
    t->verticalHeader()->setVisible(false);
    t->verticalHeader()->setDefaultSectionSize(44);
    t->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    t->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    t->setSelectionMode(QAbstractItemView::NoSelection);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    QScroller::grabGesture(t->viewport(), QScroller::LeftMouseButtonGesture);
    for (int r = 0; r < bits.size(); ++r) {
        QString name = c.bitName.value(bits.at(r).first, bits.at(r).first);
        for (const LibraryTool &lt : lib.tools())
            if (lt.id == bits.at(r).first) {
                name = lt.name.isEmpty() ? lt.model : lt.name;
                break;
            }
        t->setItem(r, 0, new QTableWidgetItem(name));
        t->setItem(r, 1, new QTableWidgetItem(hoursText(bits.at(r).second)));
    }
    v->addWidget(t, 1);
    if (bits.isEmpty())
        v->addWidget(new QLabel(QStringLiteral("No cutting counted yet: time is added while a job cuts."), &dlg));

    auto *row = new QHBoxLayout;
    auto *closeBtn = new QPushButton(QStringLiteral("Close"), &dlg);
    closeBtn->setObjectName(QStringLiteral("big"));
    closeBtn->setMinimumSize(140, 56);
    QObject::connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    row->addStretch(1);
    row->addWidget(closeBtn);
    v->addLayout(row);
    dlg.exec();
    return changed;
}

} // namespace c2d::machinecounters
