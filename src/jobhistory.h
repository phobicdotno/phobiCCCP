#pragma once
#include <QJsonObject>
#include <QString>
#include <QVector>

class QWidget;

// One line per job in AppDataLocation/job-history.jsonl, written when a job
// ends (finished, stopped, alarm or cancelled): what ran, on which machine,
// when, how long it took against the estimate, and how it ended.
namespace c2d::jobhistory {

QString filePath();
// Appends one record (a JSON object) as a single line. False if it could not write.
bool append(const QJsonObject &record);
// The last `max` records, newest first.
QVector<QJsonObject> read(int max = 200);
void clear();

// "finished", "stopped at 42 %", "alarm 2", "cancelled at 10 %" in plain words.
QString endText(const QJsonObject &record);

// The app setting "Sound at job end and tool change" (default on).
bool chimeOn();
void setChimeOn(bool on);
// A short sound and a window alert (flashes the taskbar entry) when the
// setting is on.
void chime(QWidget *window);

// The touch-friendly list: newest first, last 200, with Clear and the sound
// setting.
void showDialog(QWidget *parent);

} // namespace c2d::jobhistory
