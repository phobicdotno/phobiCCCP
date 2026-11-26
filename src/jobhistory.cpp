#include "jobhistory.h"
#include "shopdialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QScroller>
#include <QSettings>
#include <QStandardPaths>
#include <QTableWidget>
#include <QVBoxLayout>

namespace c2d::jobhistory {

QString filePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + QStringLiteral("/job-history.jsonl");
}

bool append(const QJsonObject &record)
{
    const QString path = filePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append))
        return false;
    QByteArray line = QJsonDocument(record).toJson(QJsonDocument::Compact);
    line.append('\n');
    return f.write(line) == line.size();
}

QVector<QJsonObject> read(int max)
{
    QVector<QJsonObject> out;
    QFile f(filePath());
    if (!f.open(QIODevice::ReadOnly))
        return out;
    const QList<QByteArray> lines = f.readAll().split('\n');
    for (auto it = lines.crbegin(); it != lines.crend() && out.size() < max; ++it) {
        if (it->trimmed().isEmpty())
            continue;
        const QJsonDocument d = QJsonDocument::fromJson(*it);
        if (d.isObject())
            out.append(d.object());
    }
    return out;
}

void clear()
{
    QFile::remove(filePath());
}

QString endText(const QJsonObject &r)
{
    const QString how = r.value(QStringLiteral("end")).toString();
    const int pct = r.value(QStringLiteral("percent")).toInt();
    if (how == QLatin1String("finished"))
        return QStringLiteral("✓ Finished");
    if (how == QLatin1String("alarm"))
        return QStringLiteral("Alarm %1 at %2 %").arg(r.value(QStringLiteral("code")).toInt()).arg(pct);
    if (how == QLatin1String("error"))
        return QStringLiteral("Error %1 at %2 %").arg(r.value(QStringLiteral("code")).toInt()).arg(pct);
    if (how == QLatin1String("cancelled"))
        return QStringLiteral("Cancelled at %1 %").arg(pct);
    return QStringLiteral("Stopped at %1 %").arg(pct);
}

bool chimeOn()
{
    return QSettings().value(QStringLiteral("jobChime"), true).toBool();
}

void setChimeOn(bool on)
{
    QSettings().setValue(QStringLiteral("jobChime"), on);
}

void chime(QWidget *window)
{
    if (!chimeOn())
        return;
    QApplication::beep();
    if (window)
        QApplication::alert(window->window(), 0);
}

static QString minutes(double sec)
{
    if (sec <= 0)
        return QStringLiteral("–");
    const int m = qRound(sec / 60.0);
    if (m < 1)
        return QStringLiteral("< 1 min");
    return m >= 60 ? QStringLiteral("%1 h %2 min").arg(m / 60).arg(m % 60)
                   : QStringLiteral("%1 min").arg(m);
}

static void fill(QTableWidget *t)
{
    const QVector<QJsonObject> recs = read(200);
    t->setRowCount(recs.size());
    for (int i = 0; i < recs.size(); ++i) {
        const QJsonObject &r = recs.at(i);
        const QDateTime start = QDateTime::fromString(r.value(QStringLiteral("start")).toString(), Qt::ISODate);
        const QStringList cells = {
            start.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")),
            r.value(QStringLiteral("job")).toString(),
            r.value(QStringLiteral("machine")).toString(),
            minutes(r.value(QStringLiteral("estSec")).toDouble()),
            minutes(r.value(QStringLiteral("realSec")).toDouble()),
            endText(r),
            QString::number(r.value(QStringLiteral("toolpaths")).toInt()),
            QString::number(r.value(QStringLiteral("toolChanges")).toInt()),
        };
        for (int c = 0; c < cells.size(); ++c) {
            auto *it = new QTableWidgetItem(cells.at(c));
            it->setFlags(Qt::ItemIsEnabled);
            t->setItem(i, c, it);
        }
    }
}

void showDialog(QWidget *parent)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(QStringLiteral("Job history"));
    dlg.resize(1100, 640);
    auto *v = new QVBoxLayout(&dlg);
    auto *head = new QLabel(QStringLiteral("Last jobs, newest first"), &dlg);
    QFont hf = head->font();
    hf.setBold(true);
    hf.setPointSizeF(hf.pointSizeF() + 3);
    head->setFont(hf);
    v->addWidget(head);
    auto *t = new QTableWidget(0, 8, &dlg);
    t->setObjectName(QStringLiteral("historyTable"));
    t->setHorizontalHeaderLabels({QStringLiteral("Started"), QStringLiteral("Job"), QStringLiteral("Machine"),
                                  QStringLiteral("Estimated"), QStringLiteral("Took"), QStringLiteral("Ended"),
                                  QStringLiteral("Toolpaths"), QStringLiteral("Tool changes")});
    t->verticalHeader()->setVisible(false);
    t->verticalHeader()->setDefaultSectionSize(44);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    t->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    t->setSelectionMode(QAbstractItemView::NoSelection);
    t->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    QScroller::grabGesture(t->viewport(), QScroller::LeftMouseButtonGesture);
    fill(t);
    v->addWidget(t, 1);
    auto *empty = new QLabel(QStringLiteral("No jobs yet: a line is added each time a job ends."), &dlg);
    empty->setVisible(t->rowCount() == 0);
    v->addWidget(empty);

    auto *row = new QHBoxLayout;
    auto *sound = new QCheckBox(QStringLiteral("Sound at job end and tool change"), &dlg);
    sound->setChecked(chimeOn());
    sound->setMinimumHeight(52);
    QObject::connect(sound, &QCheckBox::toggled, [](bool on) { setChimeOn(on); });
    auto *clearBtn = new QPushButton(QStringLiteral("Clear"), &dlg);
    clearBtn->setObjectName(QStringLiteral("big"));
    clearBtn->setMinimumSize(140, 56);
    auto *closeBtn = new QPushButton(QStringLiteral("Close"), &dlg);
    closeBtn->setObjectName(QStringLiteral("big"));
    closeBtn->setMinimumSize(140, 56);
    QObject::connect(clearBtn, &QPushButton::clicked, &dlg, [&] {
        ShopAsk a;
        a.title = QStringLiteral("Job history");
        a.headline = QStringLiteral("Clear the job history?");
        a.body = QStringLiteral("Every line in the list goes. This cannot be undone.");
        a.okText = QStringLiteral("Clear");
        a.okDanger = true;
        a.defaultOk = false;
        if (!shopAsk(&dlg, a))
            return;
        clear();
        fill(t);
        empty->setVisible(true);
    });
    QObject::connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    row->addWidget(sound);
    row->addStretch(1);
    row->addWidget(clearBtn);
    row->addWidget(closeBtn);
    v->addLayout(row);
    dlg.exec();
}

} // namespace c2d::jobhistory
