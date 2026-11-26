#pragma once
#include <QByteArray>
#include <QDate>
#include <QDateTime>
#include <QDialog>
#include <QMap>
#include <QString>
#include <QStringList>

class QTableWidget;
class QLabel;
class QPushButton;

namespace c2d {

// The controller's own `$` settings: what each GRBL 1.1 number means, the
// lines that change them, and the backup file kept of them.
namespace grblsettings {

struct Info {
    QString name;          // "X max rate"; "unknown" for a number GRBL 1.1 has not
    QString unit;          // "mm/min", "" for a switch
    QString desc;          // one line, plain words
    bool known = false;
};
Info info(int n);
// 6000, 0.5, 1250.25: no trailing zeros.
QString formatValue(double v);
bool sameValue(double a, double b);
// Limits/homing ($20-$22), steps/mm ($100-$102) and travel ($130-$132).
bool isRisky(int n);
// "$n=v" for every setting in `to` that differs from `from`, by number.
QStringList changeLines(const QMap<int, double> &from, const QMap<int, double> &to);
// "$110 X max rate: 5000 → 6000" for each of them.
QStringList describeChanges(const QMap<int, double> &from, const QMap<int, double> &to);

QByteArray toJson(const QString &banner, const QDateTime &when, const QMap<int, double> &settings);
// False with `why` set when the file is not such a backup.
bool fromJson(const QByteArray &data, QMap<int, double> *settings, QString *banner = nullptr,
              QString *why = nullptr);
// The daily automatic copy: <dir>/<profile>-<yyyy-MM-dd>.json, written when
// it is not there yet. Returns its path when written, empty otherwise.
QString autoBackup(const QString &dir, const QString &profile, const QDate &day,
                   const QString &banner, const QMap<int, double> &settings);

} // namespace grblsettings

// Every $n the controller reported, tap one to change it, Save sends the
// changed ones (sendRequested: the Machine page runs them one by one and
// reads $$ again, then calls setSettings). Save to file / Restore keep and
// bring back a JSON copy.
class GrblSettingsDialog : public QDialog
{
    Q_OBJECT
public:
    GrblSettingsDialog(const QMap<int, double> &settings, const QString &banner,
                       QWidget *parent = nullptr);

    // A fresh $$ after a save: the edits are dropped, the list shows it.
    void setSettings(const QMap<int, double> &settings);
    void setEdited(int n, double v);
    QMap<int, double> edited() const { return m_edit; }
    QStringList pendingLines() const { return grblsettings::changeLines(m_orig, m_edit); }
    int rowOf(int n) const;
    QString cellText(int row, int column) const;
    // Asks first when a risky setting is among the changes; then emits.
    void save();
    // The backup's values as edits; true when they differ from the controller.
    bool loadBackup(const QMap<int, double> &backup);
    void setBusy(bool busy, const QString &why = QString());

signals:
    void sendRequested(const QStringList &lines);

private:
    void rebuild();
    void editRow(int row);
    void saveToFile();
    void restoreFromFile();

    QMap<int, double> m_orig;
    QMap<int, double> m_edit;    // changed values only
    QString m_banner;
    bool m_busy = false;
    QTableWidget *m_table = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_saveBtn = nullptr;
    QPushButton *m_restoreBtn = nullptr;
};

} // namespace c2d
