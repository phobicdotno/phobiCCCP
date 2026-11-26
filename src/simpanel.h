#pragma once
#include "gcodeexport.h"
#include "post_grbl.h"
#include "simulation.h"
#include <QHash>
#include <QImage>
#include <QVector>
#include <QWidget>

class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;

// "Simulation" dock: Carbide Create style material-removal view of the current
// program (see simulation.h). Simulate runs on a worker thread with progress
// and cancel; the view zooms with the wheel, pans by dragging and reads the
// X/Y/Z of the stock surface under the cursor.
namespace c2d {

class SimView;

class SimPanel : public QWidget
{
    Q_OBJECT
public:
    explicit SimPanel(QWidget *parent = nullptr);

    // Program + tool table + stock (X 0..W, Y 0..H, Z -T..0), and the
    // document's 3D relief (if any) for the gouge check. Cheap: nothing runs
    // until simulate().
    void setJob(const QVector<Op> &ops, const QHash<int, ToolGeom> &tools,
                double stockW, double stockH, double stockT,
                const HeightModel *part = nullptr);
    // Where the 2D toolpaths mean to leave a flat floor (intendedFloors()),
    // for "Show what's left". Cheap: nothing is computed until it is on.
    void setIntendedFloors(const QVector<IntendedFloor> &floors);
    void setShowLeftovers(bool on);
    bool showLeftovers() const;
    const LeftoverResult &leftovers() const { return m_left; }
    void simulate();            // asynchronous (the Simulate button)
    void simulateBlocking();    // synchronous, on this thread (used by --shot)
    void cancel();
    bool isRunning() const;

    const QImage &image() const { return m_image; }
    const HeightMap &heightMap() const { return m_result.map; }
    const SimResult &result() const { return m_result; }

    QSize sizeHint() const override { return {360, 520}; }

signals:
    void simulationFinished();

private:
    SimSettings settings() const;
    void takeResult(const SimResult &r);
    void setBusy(bool on);
    void updateStatus();
    void refreshImage();   // base render + the leftover tint when it is on

    QVector<Op> m_ops;
    QHash<int, ToolGeom> m_tools;
    HeightModel m_part;
    double m_w = 0, m_h = 0, m_t = 0;
    bool m_stale = false;   // program changed since the last run
    // Set when setJob() cancels a run that belongs to the previous program:
    // its result must be dropped, not adopted as the current one.
    bool m_discardResult = false;

    SimulationJob *m_job;
    SimResult m_result;
    QImage m_image;
    QImage m_baseImage;    // renderHeightMap() without the leftover tint
    QVector<IntendedFloor> m_floors;
    LeftoverResult m_left;

    SimView *m_view;
    QPushButton *m_simBtn;
    QPushButton *m_cancelBtn;
    QPushButton *m_leftBtn;
    QLabel *m_leftLabel;
    QComboBox *m_res;
    QProgressBar *m_progress;
    QLabel *m_readout;
    QLabel *m_status;
};

} // namespace c2d
