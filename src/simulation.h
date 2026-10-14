#pragma once
#include "gcodeexport.h"
#include "heightmodel.h"
#include "post_grbl.h"
#include <QHash>
#include <QImage>
#include <QObject>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <functional>

class QThread;

// Material-removal simulation ("3D Simulation" in Carbide Create): the stock is
// a heightmap, every cutting move stamps the tool's swept footprint into it
// with min(), and the result is rendered as a shaded top-down image. No GUI
// dependency; runs on a worker thread via SimulationJob and is cancellable.
namespace c2d {

// Stock top surface as a grid of Z values (mm, 0 = uncut top, negative =
// cut). Cell (ix, iy) covers X [ox + ix*cell, +cell), Y [oy + iy*cell, +cell)
// — Y up like the document, so row 0 is the bottom edge of the stock.
class HeightMap
{
public:
    HeightMap() = default;
    HeightMap(int w, int h, double cell, double ox = 0, double oy = 0)
        : m_w(w), m_h(h), m_cell(cell), m_ox(ox), m_oy(oy), m_z(qsizetype(w) * h, 0.0f) {}

    bool isNull() const { return m_w <= 0 || m_h <= 0; }
    int width() const { return m_w; }
    int height() const { return m_h; }
    double cellSize() const { return m_cell; }
    double originX() const { return m_ox; }
    double originY() const { return m_oy; }

    float at(int ix, int iy) const { return m_z[qsizetype(iy) * m_w + ix]; }
    float &ref(int ix, int iy) { return m_z[qsizetype(iy) * m_w + ix]; }
    float *row(int iy) { return m_z.data() + qsizetype(iy) * m_w; }
    const float *row(int iy) const { return m_z.constData() + qsizetype(iy) * m_w; }
    const QVector<float> &data() const { return m_z; }

    double cellCenterX(int ix) const { return m_ox + (ix + 0.5) * m_cell; }
    double cellCenterY(int iy) const { return m_oy + (iy + 0.5) * m_cell; }
    // Cell containing world point (x, y); false when outside the map.
    bool cellOf(double x, double y, int *ix, int *iy) const;
    // Z of the cell under (x, y); NaN outside the map.
    double sample(double x, double y) const;

private:
    int m_w = 0, m_h = 0;
    double m_cell = 1, m_ox = 0, m_oy = 0;
    QVector<float> m_z;
};

struct SimSettings {
    double stockW = 0, stockH = 0, stockT = 0;   // mm; stock spans X 0..W, Y 0..H, Z -T..0
    double minCell = 0.1;      // finest cell size (mm); coarser presets raise this
    int maxCells = 4000000;    // cell budget; cell size grows to respect it
    double chord = 0.2;        // arc tessellation tolerance (max chord length, mm)
    double maxDz = 0.05;       // sloped moves are split so each piece drops ≤ this
    // The part, for the gouge check: the document's 3D relief (heightmodel.h
    // convention, z <= 0 from the stock top). Invalid = no model, no check.
    HeightModel part;
    double gougeTol = 0.05;    // mm below the part before a cell counts (+ maxDz, see simulate())
    double rapidTol = 0.05;    // mm a rapid may graze the stock before it counts as a hit
};

// A rapid (G0) that passes through material: the deepest point of that move,
// where the tool's underside is `depth` mm below the stock as it was then.
struct RapidHit {
    QString op;                  // toolpath name (the last Comment op before it)
    double x = 0, y = 0, z = 0;  // tool tip at the deepest point
    double depth = 0;            // mm into the stock
};

struct SimResult {
    HeightMap map;
    bool throughCut = false;     // some cell reached the stock bottom
    double minZ = 0;             // deepest cell
    int segments = 0;            // stamped pieces (after tessellation/splitting)
    int cutOps = 0;              // Feed/Arc ops that cut
    QList<int> missingTools;     // tool numbers not in the table (default tool used)
    double elapsedMs = 0;
    bool cancelled = false;

    // Gouge check against SimSettings::part (only when one was given): cells
    // inside the relief's footprint where the simulated surface lies below
    // the part by more than the tolerance. gougeMask is width*height, 1 =
    // gouged, empty when there are none.
    int gougeCells = 0;
    double gougeArea = 0;        // mm²
    double gougeDepth = 0;       // deepest, mm into the part along its normal (0 = none)
    QVector<quint8> gougeMask;

    // Rapids into material: every G0 is checked against the stock as cut so
    // far. `rapidHitCount` counts the offending moves; `rapidHits` keeps the
    // first 100 of them, and `worstRapid` the deepest.
    int rapidHitCount = 0;
    QList<RapidHit> rapidHits;
    RapidHit worstRapid;
};

using SimProgress = std::function<void(int)>;   // percent 0..100

// Run the simulation synchronously. `cancel` (optional) is polled between
// ops; a cancelled run returns the partial map with cancelled = true. Rapids
// are checked against the stock as they happen; the gouge check runs at the
// end when settings.part is valid (not on a cancelled run).
SimResult simulate(const QVector<Op> &ops, const QHash<int, ToolGeom> &tools,
                   const SimSettings &settings, std::atomic<bool> *cancel = nullptr,
                   const SimProgress &progress = {});

// Shaded top-down rendering: one pixel per cell, Y up (row 0 = top of the
// stock), light from the upper left, uncut stock light, deeper darker,
// through-cuts in a distinct background colour, gouged cells (a non-empty
// `gouges` mask, SimResult::gougeMask) in red.
QImage renderHeightMap(const HeightMap &map, double stockT,
                       const QVector<quint8> &gouges = {});

// Colours used by renderHeightMap, exposed for the panel's legend.
QColor simColorUncut();
QColor simColorDeep();
QColor simColorThrough();
QColor simColorGouge();

// Worker-thread wrapper: start() returns immediately, progress()/finished()
// are emitted on the job's thread (the GUI thread), result() is valid after
// finished(). Destroying a running job cancels and waits.
class SimulationJob : public QObject
{
    Q_OBJECT
public:
    explicit SimulationJob(QObject *parent = nullptr);
    ~SimulationJob() override;

    bool isRunning() const { return m_running; }
    void start(const QVector<Op> &ops, const QHash<int, ToolGeom> &tools,
               const SimSettings &settings);
    void cancel() { m_cancel = true; }
    const SimResult &result() const { return m_result; }

signals:
    void progress(int percent);
    void finished(bool cancelled);

private:
    QThread *m_thread = nullptr;
    std::atomic<bool> m_cancel{false};
    SimResult m_result;
    bool m_running = false;
};

} // namespace c2d
