#pragma once
#include "post_grbl.h"

#include <QObject>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>

// Surface map: the stock top measured at a grid of points with the BitZero
// (or by touching conductive stock directly), so a program can follow a table
// or a board that is not flat or not square to the gantry.
//
// Heights are kept as the MACHINE Z of the stock top at each point. What a
// program gets is the difference to the height at work X0 Y0, where the
// corner probe sets Z0: so the map survives re-zeroing Z at the corner and
// every tool change (both shift all points alike), and needs no work offset
// stored with it. XY are work coordinates, so X0 Y0 must not move after
// mapping.
namespace c2d {

class GrblStreamer;

struct SurfaceMap {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // grid corners, work XY (mm)
    int cols = 0, rows = 0;
    QVector<double> z;                       // row-major from (x0, y0); machine Z

    bool isValid() const;
    double xAt(int col) const;
    double yAt(int row) const;
    // Bilinear between the four surrounding points; outside the grid the edge
    // value is held (no extrapolation: a tilt extended past the probed area is
    // a guess, and a wrong guess cuts air or digs in).
    double heightAt(double x, double y) const;
    // What a program's Z gets at (x, y): height there minus height at X0 Y0.
    double offsetAt(double x, double y) const;
    // Smallest and largest offset over the grid points.
    void offsetRange(double *lo, double *hi) const;
    bool covers(double x, double y, double tol = 0.5) const;

    QString toJson() const;
    static SurfaceMap fromJson(const QString &json);   // invalid map on any error
    QString summary() const;                          // "3 × 3, −0.21 … +0.35 mm"
};

// Probe order over a cols × rows grid spanning (x0, y0)–(x1, y1): row by row,
// alternating direction so the head never crosses the board empty. Each entry
// is the point and its index into SurfaceMap::z.
struct GridPoint { double x, y; int index; };
QVector<GridPoint> surfaceMapProbeOrder(double x0, double y0, double x1, double y1,
                                       int cols, int rows);

// The program with every Z raised by the map's offset at its XY. Feed moves
// are cut into pieces no longer than `maxSegment` so Z follows the surface
// between their ends; arcs become chords (within 0.005 mm of the arc) for the
// same reason. Rapids keep their single move but travel at their Z plus the
// highest offset anywhere under the move, then drop to the offset at their
// end. After a tool change the position is not known (the tool-change macro
// moved the head), so the first move after it is taken whole, and a rapid
// there clears the highest point of the map.
QVector<Op> applySurfaceMap(const QVector<Op> &ops, const SurfaceMap &map,
                           double maxSegment = 3.0);

// How the map is measured.
struct SurfaceMapPlan {
    double x0 = 10, y0 = 10, x1 = 90, y1 = 90;   // work XY
    int cols = 3, rows = 3;
    double plate = 13.0;        // plate top above the stock; 0 = touching the stock itself
    bool pauseEachPoint = true; // stop over each point for the plate to be put under the tool
    double clearance = 5.0;     // travel height above the plate top (work Z)
    double maxDeviation = 5.0;  // how far below the expected plate top a probe may reach
    double fastFeed = 300.0, slowFeed = 50.0, retract = 2.0;

    double travelZ() const { return plate + clearance; }
};

// The two macros for one point. `moveLines` lifts to travel height and goes
// over the point; `probeLines` touches twice (fast, back off, slow) and lifts
// back to travel height. Work coordinates throughout.
QStringList surfaceMapMoveLines(const SurfaceMapPlan &plan, double x, double y);
QStringList surfaceMapProbeLines(const SurfaceMapPlan &plan);

// Runs a plan point by point on a connected controller. Between points it
// either goes straight on, or (pauseEachPoint) stops over the next point and
// emits waitingForPlate() until probeNext() is called.
class SurfaceMapProber : public QObject
{
    Q_OBJECT
public:
    explicit SurfaceMapProber(GrblStreamer *grbl, QObject *parent = nullptr);
    bool isRunning() const { return m_stage != Stage::Idle; }
    bool isWaiting() const { return m_stage == Stage::Waiting; }
    void start(const SurfaceMapPlan &plan);
    void probeNext();           // the plate is under the tool: touch it
    void cancel();              // forget the run; the controller is not touched

signals:
    void waitingForPlate(int point, int total);   // 1-based
    void probing(int point, int total);
    void pointMeasured(int point, int total, double x, double y, double stockZ);
    void finished(const c2d::SurfaceMap &map);
    void failed(const QString &why);

private:
    enum class Stage { Idle, Moving, Waiting, Probing };
    void onProbe(double mx, double my, double mz, bool ok);
    void onMacroFinished(bool ok);
    void moveToCurrent();
    void fail(const QString &why);

    GrblStreamer *m_grbl;
    SurfaceMapPlan m_plan;
    QVector<GridPoint> m_order;
    SurfaceMap m_map;
    int m_current = 0;
    QVector<double> m_hits;
    double m_hitX = 0, m_hitY = 0;   // last touch, work XY
    bool m_hitValid = false;
    Stage m_stage = Stage::Idle;
};

} // namespace c2d
