#pragma once
#include "surfacemap.h"

#include <QWidget>

class QButtonGroup;
class QCheckBox;
class QSpinBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QWidget;

namespace c2d {

// Carbide's two BitZero touch plates.
//
// V2: a thin plate that sits on the lower-left corner of the stock with its
// locating lips against both sides. A circular bore is centred exactly over
// the stock corner, so probing the bore walls finds the corner whatever the
// pin diameter; the plate top is a known height above the stock (13 mm by
// owners' measurements), so one touch on it gives Z.
//
// V1 (the original 60 × 60 block): lips 7 mm wide hook over the two stock
// edges, the top is 22 mm over the stock. Carbide Motion probes it from the
// OUTSIDE - Z on the top, then down beside the block for its left side and
// its front side - so the tool's diameter and the lip width enter the sum:
// stock edge = block side + lip width.
struct BitZeroConfig {
    bool v1 = false;
    double thickness = 13.0;   // plate top above the stock top, lips over the edge
    double lipDrop = 2.8;      // how far the lips hang below the base: Z only stands on them
    double fastFeed = 300.0;   // first touch, mm/min
    double slowFeed = 50.0;    // measuring touch
    double retract = 2.0;      // back-off between the two touches
    double zTravel = 25.0;     // how far the Z probe may descend
    // V2
    double search = 30.0;      // how far a wall probe may travel in X/Y
    double zSpotX = 20.0;      // where to touch the plate for Z, from the bore centre
    double zSpotY = 20.0;
    // V1
    double lipWidth = 7.0;     // lip, block side to stock edge
    double toolDiameter = 6.35;
    double sideClearance = 40.0;  // sideways from the start before going down
    double sideDepth = 5.0;       // below the block top when probing its side
};

// The "Probe zero (BitZero)" page of the Machine screen's right-hand card:
// pick the plate and what to find, see that the probe circuit works before
// anything moves, then Begin. The probing itself runs in MachinePanel, which
// owns the GRBL link. Styled by MachinePanel's sheet (#step toggles, #start).
class BitZeroPanel : public QWidget
{
    Q_OBJECT
public:
    // Map: the plate top touched at a grid of points over the stock, for a
    // surface map the job's Z then follows (surfacemap.h).
    enum Mode { Corner, ZOnly, XOnly, YOnly, Map };
    explicit BitZeroPanel(QWidget *parent = nullptr);
    Mode mode() const;
    BitZeroConfig config() const;
    // The grid over the stock (work X0 Y0 at its lower-left corner), with the
    // plate's numbers and feeds from config().
    SurfaceMapPlan mapPlan() const;
    void setStockSize(double width, double height);
    // Map run feedback: over point `point` of `total`, waiting for the plate
    // (Begin then probes it); or 0 when no map is running.
    void setMapWaiting(int point, int total);
    void setMapSummary(const QString &text);
    void setContact(bool closed, bool known);   // live probe-pin state
    void setBusy(bool on);                      // a probe sequence is running
    void setConnected(bool on);

signals:
    void beginRequested();
    void backRequested();                       // "← Job"
    void cancelMapRequested();

private:
    void load();
    void save();
    void updateHelp();
    void updateRows();
    QString plateKey() const;

    QButtonGroup *m_plates;
    QButtonGroup *m_modes;
    QLabel *m_help;
    QLabel *m_contact;
    QPushButton *m_begin;
    QDoubleSpinBox *m_thick, *m_search, *m_spotX, *m_spotY, *m_zTravel,
                   *m_fast, *m_slow, *m_retract,
                   *m_lip, *m_toolDia, *m_clearance, *m_sideDepth, *m_lipDrop;
    QSpinBox *m_mapCols, *m_mapRows;
    QDoubleSpinBox *m_mapW, *m_mapH, *m_mapMargin, *m_mapClear, *m_mapDev;
    QCheckBox *m_mapDirect;
    QWidget *m_mapBox = nullptr;
    QLabel *m_mapSummary = nullptr;
    QPushButton *m_mapCancel = nullptr;
    int m_mapPoint = 0, m_mapTotal = 0;
    void updateBegin();
    QWidget *m_v2Rows[2] = {};
    QWidget *m_v1Rows[4] = {};
    QWidget *m_v2Labels[2] = {};
    QWidget *m_v1Labels[4] = {};
    bool m_loading = false;
    bool m_connected = false;
    bool m_busy = false;
};

} // namespace c2d
