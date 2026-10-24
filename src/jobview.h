#pragma once
#include <QElapsedTimer>
#include <QString>
#include <QVector>
#include <QWidget>

class QLabel;
class QProgressBar;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

namespace c2d {

class Document;
struct Op;

// One depth pass of a toolpath (or the whole toolpath when it has no
// distinct passes: drilling, a single-level cut), as a stretch of the
// streamed program: queue indices [qStart, qEnd) of the lines it sends.
struct JobPass {
    QString what;          // "to Z -6.0 mm", "peck drill 16 holes"...
    double z = 0;          // the floor it reaches (work Z)
    int qStart = 0, qEnd = 0;
    double estSec = 0;     // feed-rate estimate, no acceleration model
};

// A toolpath section of the program (the post's "(name)" comment to the
// next one), or a tool change pause.
struct JobSection {
    enum Kind { Toolpath, ToolChange } kind = Toolpath;
    QString name;          // toolpath name, or "Tool change"
    QString detail;        // "Pocket · Ø3 mm · 4 mm deep", "T3  3 mm flat endmill"
    int qStart = 0, qEnd = 0;
    double estSec = 0;
    QVector<JobPass> passes;
};

// The streamed program read back the way the operator thinks of it:
// toolpaths, their passes, what each does and how long it should take.
// Built from the exact lines handed to the streamer (after every transform),
// so queue indices match the streamer's progress counts: a line the
// streamer never queues (a comment, a blank) takes no index.
struct JobPlan {
    QVector<JobSection> sections;
    QVector<double> cumSec;   // cumSec[q] = estimated seconds before queue line q; size total + 1
    int total = 0;            // queued lines
    double totalSec() const { return cumSec.isEmpty() ? 0 : cumSec.last(); }
    bool isEmpty() const { return sections.isEmpty(); }
};

// `ops` supplies the full toolpath names (the post truncates its comments to
// 12 characters) and `doc` the toolpath type, tool and depth for the detail
// line; both may be empty/null, the plan then says what the program says.
// `rapidRate` in mm/min for the rapid moves' time.
JobPlan planJob(const QStringList &lines, const QVector<Op> &ops, const Document *doc,
                double rapidRate = 5000);

// The middle of the Machine page while a job runs: every toolpath and pass
// of the program with its state (done, running, to come) and the time left
// on each, in place of the jog pad and the zeroing buttons, which are not in
// use while the machine cuts.
class JobView : public QWidget
{
    Q_OBJECT
public:
    explicit JobView(QWidget *parent = nullptr);

    void setPlan(const JobPlan &plan, const QString &jobName);
    void clear();
    // The streamer's progress: `acked` lines answered of `total` queued. Lines
    // the sender inserted on the way (the rise and return at a tool change)
    // are not in the plan: tell how many, so the counts line up again.
    void setProgress(int acked, int total);
    void linesInserted(int n) { m_inserted += n; refresh(); }
    // The controller's feed override, percent: the feed part of every
    // estimate is scaled by it.
    void setOverride(int percent);
    // The sender stands at a tool change: the clock of the running step
    // waits, nothing cuts.
    void setParked(bool parked);

    bool hasPlan() const { return !m_plan.isEmpty(); }
    int running() const { return m_running; }      // section index, -1 before start

private:
    void refresh();
    double leftFrom(int q) const;                  // seconds from queue line q to the end
    double spanSec(int a, int b) const;            // estimate for queue lines [a, b)
    static QString clock(double sec);
    static QString eta(double sec);

    JobPlan m_plan;
    QString m_jobName;
    int m_acked = 0, m_inserted = 0;
    int m_override = 100;
    bool m_parked = false;
    int m_running = -1;
    int m_runningPass = -1;
    // Wall-clock bookkeeping: when each section (and the pass within it)
    // was reached, so finished ones show how long they really took.
    QVector<qint64> m_sectionStart, m_sectionDone;      // ms since m_clock start, -1 = not yet
    QVector<QVector<qint64>> m_passStart, m_passDone;
    QElapsedTimer m_clock;
    qint64 m_parkedAt = -1;
    qint64 m_parkedTotal = 0;

    QLabel *m_title = nullptr;
    QLabel *m_summary = nullptr;
    QLabel *m_current = nullptr;
    QProgressBar *m_passBar = nullptr;
    QTreeWidget *m_tree = nullptr;
    QVector<QTreeWidgetItem *> m_sectionItems;
    QVector<QVector<QTreeWidgetItem *>> m_passItems;
    QTimer *m_tick = nullptr;
};

} // namespace c2d
