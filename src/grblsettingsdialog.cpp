#include "grblsettingsdialog.h"

#include "shopdialog.h"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QStandardPaths>
#include <QTableWidget>
#include <QVBoxLayout>

#include <cmath>

namespace c2d {
namespace grblsettings {

Info info(int n)
{
    struct Row { int n; const char *name, *unit, *desc; };
    static const Row rows[] = {
        {0, "Step pulse time", "µs", "How long each step pulse lasts for the stepper drivers."},
        {1, "Step idle delay", "ms", "How long the motors stay powered after a move; 255 keeps them on."},
        {2, "Step pulse invert", "mask", "Flips the step signal per axis (bit 0 X, 1 Y, 2 Z)."},
        {3, "Direction invert", "mask", "Flips the direction an axis moves (bit 0 X, 1 Y, 2 Z)."},
        {4, "Step enable invert", "on/off", "Flips the motor enable signal."},
        {5, "Limit pins invert", "on/off", "Flips the limit switch inputs (normally open or closed)."},
        {6, "Probe pin invert", "on/off", "Flips the probe input (BitSetter, BitZero)."},
        {10, "Status report", "mask", "What the controller puts in its status reports."},
        {11, "Junction deviation", "mm", "How fast it takes corners; smaller is slower and gentler."},
        {12, "Arc tolerance", "mm", "How closely arcs are split into short lines."},
        {13, "Report in inches", "on/off", "Positions reported in inches instead of mm."},
        {20, "Soft limits", "on/off", "Refuse moves past the travel set in $130-$132 (needs homing)."},
        {21, "Hard limits", "on/off", "Stop at once when a limit switch is hit."},
        {22, "Homing cycle", "on/off", "Turns homing ($H) on; most machines lock until homed."},
        {23, "Homing direction invert", "mask", "Which end of each axis homing looks for (bit 0 X, 1 Y, 2 Z)."},
        {24, "Homing locate feed", "mm/min", "Slow speed for the final touch on each switch."},
        {25, "Homing search seek", "mm/min", "Fast speed while looking for each switch."},
        {26, "Homing debounce", "ms", "Wait for the switch signal to settle."},
        {27, "Homing pull-off", "mm", "How far it backs off the switches after homing."},
        {30, "Max spindle speed", "rpm", "The speed that gives full spindle output (S at 100 %)."},
        {31, "Min spindle speed", "rpm", "The speed at the lowest spindle output."},
        {32, "Laser mode", "on/off", "Moves do not stop for spindle changes; off for routers."},
        {100, "X steps/mm", "steps/mm", "Motor steps for one mm of X; wrong values cut the wrong size."},
        {101, "Y steps/mm", "steps/mm", "Motor steps for one mm of Y; wrong values cut the wrong size."},
        {102, "Z steps/mm", "steps/mm", "Motor steps for one mm of Z; wrong values cut the wrong depth."},
        {110, "X max rate", "mm/min", "Fastest X speed, also for rapid moves."},
        {111, "Y max rate", "mm/min", "Fastest Y speed, also for rapid moves."},
        {112, "Z max rate", "mm/min", "Fastest Z speed, also for rapid moves."},
        {120, "X acceleration", "mm/s²", "How hard X speeds up and slows down."},
        {121, "Y acceleration", "mm/s²", "How hard Y speeds up and slows down."},
        {122, "Z acceleration", "mm/s²", "How hard Z speeds up and slows down."},
        {130, "X max travel", "mm", "Length of X travel; soft limits and jog limits use it."},
        {131, "Y max travel", "mm", "Length of Y travel; soft limits and jog limits use it."},
        {132, "Z max travel", "mm", "Length of Z travel; soft limits and jog limits use it."},
    };
    for (const Row &r : rows)
        if (r.n == n)
            return {QString::fromUtf8(r.name), QString::fromUtf8(r.unit),
                    QString::fromUtf8(r.desc), true};
    return {QStringLiteral("unknown"), QString(),
            QStringLiteral("Not a GRBL 1.1 setting (grblHAL or another firmware): see its manual."),
            false};
}

QString formatValue(double v)
{
    QString s = QString::number(v, 'f', 3);
    if (s.contains(QLatin1Char('.'))) {
        while (s.endsWith(QLatin1Char('0')))
            s.chop(1);
        if (s.endsWith(QLatin1Char('.')))
            s.chop(1);
    }
    return s == QLatin1String("-0") ? QStringLiteral("0") : s;
}

bool sameValue(double a, double b)
{
    return std::abs(a - b) < 0.0005;
}

bool isRisky(int n)
{
    return (n >= 20 && n <= 22) || (n >= 100 && n <= 102) || (n >= 130 && n <= 132);
}

QStringList changeLines(const QMap<int, double> &from, const QMap<int, double> &to)
{
    QStringList out;
    for (auto it = to.cbegin(); it != to.cend(); ++it)
        if (!from.contains(it.key()) || !sameValue(from.value(it.key()), it.value()))
            out << QStringLiteral("$%1=%2").arg(it.key()).arg(formatValue(it.value()));
    return out;
}

QStringList describeChanges(const QMap<int, double> &from, const QMap<int, double> &to)
{
    QStringList out;
    for (auto it = to.cbegin(); it != to.cend(); ++it) {
        if (from.contains(it.key()) && sameValue(from.value(it.key()), it.value()))
            continue;
        const Info i = info(it.key());
        out << QStringLiteral("$%1 %2: %3 → %4")
                   .arg(it.key())
                   .arg(i.name,
                        from.contains(it.key()) ? formatValue(from.value(it.key()))
                                                : QStringLiteral("(not reported)"),
                        formatValue(it.value()));
    }
    return out;
}

QByteArray toJson(const QString &banner, const QDateTime &when, const QMap<int, double> &settings)
{
    QJsonObject s;
    for (auto it = settings.cbegin(); it != settings.cend(); ++it)
        s.insert(QString::number(it.key()), it.value());
    QJsonObject o;
    o.insert(QStringLiteral("firmware"), banner);
    o.insert(QStringLiteral("date"), when.toString(Qt::ISODate));
    o.insert(QStringLiteral("settings"), s);
    return QJsonDocument(o).toJson(QJsonDocument::Indented);
}

bool fromJson(const QByteArray &data, QMap<int, double> *settings, QString *banner, QString *why)
{
    auto fail = [why](const QString &w) {
        if (why)
            *why = w;
        return false;
    };
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject())
        return fail(QStringLiteral("The file is not a settings backup (not JSON)."));
    const QJsonObject s = doc.object().value(QStringLiteral("settings")).toObject();
    if (s.isEmpty())
        return fail(QStringLiteral("The file has no settings in it."));
    QMap<int, double> out;
    for (auto it = s.constBegin(); it != s.constEnd(); ++it) {
        bool ok = false;
        const int n = it.key().toInt(&ok);
        if (!ok || n < 0 || !it.value().isDouble())
            return fail(QStringLiteral("The file has a setting that cannot be read: $%1.").arg(it.key()));
        out.insert(n, it.value().toDouble());
    }
    if (settings)
        *settings = out;
    if (banner)
        *banner = doc.object().value(QStringLiteral("firmware")).toString();
    return true;
}

QString autoBackup(const QString &dir, const QString &profile, const QDate &day,
                   const QString &banner, const QMap<int, double> &settings)
{
    if (settings.isEmpty())
        return QString();
    QString name = profile.trimmed();
    for (QChar &c : name)
        if (!c.isLetterOrNumber() && c != QLatin1Char('-') && c != QLatin1Char('_'))
            c = QLatin1Char('_');
    if (name.isEmpty())
        name = QStringLiteral("machine");
    const QString path = QDir(dir).filePath(
        QStringLiteral("%1-%2.json").arg(name, day.toString(Qt::ISODate)));
    if (QFile::exists(path) || !QDir().mkpath(dir))
        return QString();
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return QString();
    f.write(toJson(banner, QDateTime::currentDateTime(), settings));
    return path;
}

} // namespace grblsettings

using namespace grblsettings;

GrblSettingsDialog::GrblSettingsDialog(const QMap<int, double> &settings, const QString &banner,
                                       QWidget *parent)
    : QDialog(parent), m_orig(settings), m_banner(banner)
{
    setWindowTitle(QStringLiteral("Controller settings"));
    setObjectName(QStringLiteral("grblDialog"));
    setAttribute(Qt::WA_StyledBackground);
    setStyleSheet(QStringLiteral(
        "#grblDialog { background: #16181d; }"
        "QLabel { color: #e6e9ef; }"
        "QLabel#caption { color: #8b93a1; font-size: 11px; letter-spacing: 1px; }"
        "QLabel#headline { font-size: 22px; font-weight: bold; }"
        "QLabel#hint { color: #a3abb8; font-size: 14px; }"
        "QTableWidget { background: #1e2127; color: #e6e9ef; font-size: 15px; "
        "  gridline-color: #2a2e36; border: 1px solid #3a404b; border-radius: 10px; }"
        "QTableWidget::item:selected { background: #3a4150; }"
        "QHeaderView::section { background: #16181d; color: #8b93a1; border: 0; padding: 6px; }"
        "QPushButton { min-height: 48px; padding: 0 20px; font-size: 15px; border-radius: 10px; "
        "  background: #2a2e36; color: #e6e9ef; border: 1px solid #3a404b; }"
        "QPushButton:pressed { background: #3a4150; }"
        "QPushButton:disabled { color: #6b7380; }"
        "QPushButton#save { font-weight: bold; background: #f2a536; color: #1a1a1a; border: 0; "
        "  min-width: 140px; }"
        "QPushButton#save:disabled { background: #5a4a2a; }"));
    resize(980, 640);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(24, 18, 24, 18);
    lay->setSpacing(10);
    auto *cap = new QLabel(QStringLiteral("CONTROLLER"), this);
    cap->setObjectName(QStringLiteral("caption"));
    auto *headline = new QLabel(QStringLiteral("Controller settings ($$)"), this);
    headline->setObjectName(QStringLiteral("headline"));
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("hint"));
    m_status->setWordWrap(true);
    lay->addWidget(cap);
    lay->addWidget(headline);
    lay->addWidget(m_status);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels({QStringLiteral("$"), QStringLiteral("Name"),
                                        QStringLiteral("Value"), QStringLiteral("Unit"),
                                        QStringLiteral("What it does")});
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setDefaultSectionSize(46);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->setWordWrap(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    lay->addWidget(m_table, 1);
    connect(m_table, &QTableWidget::cellClicked, this, [this](int row, int) { editRow(row); });

    auto *foot = new QHBoxLayout;
    auto *toFile = new QPushButton(QStringLiteral("Save to file…"), this);
    toFile->setToolTip(QStringLiteral("Keep a copy of every setting in a file"));
    m_restoreBtn = new QPushButton(QStringLiteral("Restore…"), this);
    m_restoreBtn->setToolTip(QStringLiteral("Load a saved copy, see what differs, and send those"));
    auto *close = new QPushButton(QStringLiteral("Close"), this);
    m_saveBtn = new QPushButton(QStringLiteral("Save"), this);
    m_saveBtn->setObjectName(QStringLiteral("save"));
    m_saveBtn->setToolTip(QStringLiteral("Send the changed settings to the controller, one by one"));
    foot->addWidget(toFile);
    foot->addWidget(m_restoreBtn);
    foot->addStretch(1);
    foot->addWidget(close);
    foot->addWidget(m_saveBtn);
    lay->addLayout(foot);
    connect(toFile, &QPushButton::clicked, this, [this] { saveToFile(); });
    connect(m_restoreBtn, &QPushButton::clicked, this, [this] { restoreFromFile(); });
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_saveBtn, &QPushButton::clicked, this, [this] { save(); });
    rebuild();
}

void GrblSettingsDialog::setSettings(const QMap<int, double> &settings)
{
    m_orig = settings;
    m_edit.clear();
    rebuild();
}

void GrblSettingsDialog::setEdited(int n, double v)
{
    if (m_orig.contains(n) && sameValue(m_orig.value(n), v))
        m_edit.remove(n);
    else
        m_edit.insert(n, v);
    rebuild();
}

int GrblSettingsDialog::rowOf(int n) const
{
    for (int r = 0; r < m_table->rowCount(); ++r)
        if (m_table->item(r, 0)->data(Qt::UserRole).toInt() == n)
            return r;
    return -1;
}

QString GrblSettingsDialog::cellText(int row, int column) const
{
    const QTableWidgetItem *it = m_table->item(row, column);
    return it ? it->text() : QString();
}

void GrblSettingsDialog::rebuild()
{
    QMap<int, double> all = m_orig;
    for (auto it = m_edit.cbegin(); it != m_edit.cend(); ++it)
        all.insert(it.key(), it.value());
    m_table->setRowCount(0);
    m_table->setRowCount(all.size());
    int r = 0;
    for (auto it = all.cbegin(); it != all.cend(); ++it, ++r) {
        const Info i = info(it.key());
        const bool changed = m_edit.contains(it.key());
        auto *num = new QTableWidgetItem(QStringLiteral("$%1").arg(it.key()));
        num->setData(Qt::UserRole, it.key());
        auto *val = new QTableWidgetItem(formatValue(it.value()) + (changed ? QStringLiteral("  ●") : QString()));
        if (changed)
            val->setForeground(QColor(0xf2, 0xa5, 0x36));
        auto *name = new QTableWidgetItem(i.name);
        if (!i.known)
            name->setForeground(QColor(0x8b, 0x93, 0xa1));
        m_table->setItem(r, 0, num);
        m_table->setItem(r, 1, name);
        m_table->setItem(r, 2, val);
        m_table->setItem(r, 3, new QTableWidgetItem(i.unit));
        auto *desc = new QTableWidgetItem(i.desc);
        desc->setToolTip(i.desc);
        m_table->setItem(r, 4, desc);
    }
    m_table->resizeColumnsToContents();
    m_table->horizontalHeader()->setStretchLastSection(true);
    const int n = pendingLines().size();
    m_saveBtn->setText(n ? QStringLiteral("Save %1 change%2").arg(n).arg(n == 1 ? "" : "s")
                         : QStringLiteral("Save"));
    m_saveBtn->setEnabled(n > 0 && !m_busy);
    if (!m_busy)
        m_status->setText(QStringLiteral("%1. Tap a row to change it; Save sends only the changed ones.")
                              .arg(m_banner.isEmpty() ? QStringLiteral("%1 settings").arg(all.size())
                                                      : m_banner));
}

void GrblSettingsDialog::setBusy(bool busy, const QString &why)
{
    m_busy = busy;
    m_restoreBtn->setEnabled(!busy);
    m_table->setEnabled(!busy);
    rebuild();
    if (busy)
        m_status->setText(why);
}

void GrblSettingsDialog::editRow(int row)
{
    if (row < 0 || row >= m_table->rowCount())
        return;
    const int n = m_table->item(row, 0)->data(Qt::UserRole).toInt();
    const Info i = info(n);
    const double now = m_edit.contains(n) ? m_edit.value(n) : m_orig.value(n);
    bool ok = false;
    const double v = QInputDialog::getDouble(
        this, QStringLiteral("$%1 %2").arg(n).arg(i.name),
        QStringLiteral("%1%2\n%3").arg(i.name, i.unit.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(i.unit), i.desc),
        now, -1e9, 1e9, 3, &ok);
    if (ok)
        setEdited(n, v);
}

void GrblSettingsDialog::save()
{
    const QStringList lines = pendingLines();
    if (lines.isEmpty())
        return;
    QStringList risky;
    for (auto it = m_edit.cbegin(); it != m_edit.cend(); ++it)
        if (isRisky(it.key()) && (!m_orig.contains(it.key()) || !sameValue(m_orig.value(it.key()), it.value())))
            risky << QStringLiteral("$%1 %2: %3 → %4")
                         .arg(it.key())
                         .arg(info(it.key()).name, formatValue(m_orig.value(it.key())),
                              formatValue(it.value()));
    if (!risky.isEmpty()) {
        ShopAsk a;
        a.title = QStringLiteral("Controller settings");
        a.headline = QStringLiteral("Change how the machine moves and stops?");
        a.body = risky.join(QLatin1Char('\n'));
        a.note = QStringLiteral(
            "Limits and homing ($20-$22) decide whether the machine stops at its ends; "
            "steps/mm ($100-$102) set how far a motor turn moves, so a wrong value cuts the "
            "wrong size; travel ($130-$132) sets where soft limits and jogs stop. Home again "
            "after the change and check a move with a ruler.");
        a.noteDanger = true;
        a.okText = QStringLiteral("Send them");
        a.okDanger = true;
        a.defaultOk = false;
        if (!shopAsk(this, a))
            return;
    }
    emit sendRequested(lines);
}

bool GrblSettingsDialog::loadBackup(const QMap<int, double> &backup)
{
    m_edit.clear();
    for (auto it = backup.cbegin(); it != backup.cend(); ++it)
        if (!m_orig.contains(it.key()) || !sameValue(m_orig.value(it.key()), it.value()))
            m_edit.insert(it.key(), it.value());
    rebuild();
    return !m_edit.isEmpty();
}

void GrblSettingsDialog::saveToFile()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save controller settings"),
        QDir(dir).filePath(QStringLiteral("grbl-settings-%1.json")
                               .arg(QDate::currentDate().toString(Qt::ISODate))),
        QStringLiteral("Settings backup (*.json)"));
    if (path.isEmpty())
        return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        shopTell(this, QStringLiteral("Controller settings"), QStringLiteral("Could not save"),
                 f.errorString());
        return;
    }
    f.write(toJson(m_banner, QDateTime::currentDateTime(), m_orig));
}

void GrblSettingsDialog::restoreFromFile()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                  + QStringLiteral("/grbl-backups");
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Restore controller settings"), dir,
        QStringLiteral("Settings backup (*.json)"));
    if (path.isEmpty())
        return;
    QFile f(path);
    QMap<int, double> backup;
    QString banner, why;
    if (!f.open(QIODevice::ReadOnly) || !fromJson(f.readAll(), &backup, &banner, &why)) {
        shopTell(this, QStringLiteral("Controller settings"), QStringLiteral("Could not restore"),
                 why.isEmpty() ? f.errorString() : why);
        return;
    }
    const QStringList diff = describeChanges(m_orig, backup);
    if (diff.isEmpty()) {
        shopTell(this, QStringLiteral("Controller settings"),
                 QStringLiteral("Nothing to restore"),
                 QStringLiteral("The controller has every setting of that file already."));
        return;
    }
    ShopAsk a;
    a.title = QStringLiteral("Restore settings");
    a.headline = QStringLiteral("%1 setting%2 differ%3 from the file")
                     .arg(diff.size()).arg(diff.size() == 1 ? "" : "s")
                     .arg(diff.size() == 1 ? "s" : "");
    a.body = diff.join(QLatin1Char('\n'));
    if (!banner.isEmpty() && !m_banner.isEmpty() && banner != m_banner)
        a.note = QStringLiteral("The file came from \"%1\"; this controller is \"%2\".")
                     .arg(banner, m_banner);
    a.okText = QStringLiteral("Restore");
    if (!shopAsk(this, a))
        return;
    loadBackup(backup);
    save();
}

} // namespace c2d
