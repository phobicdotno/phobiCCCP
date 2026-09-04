#pragma once
#include "post_grbl.h"
#include <QElapsedTimer>
#include <QVector>
#include <QWidget>

class QComboBox;
class QLabel;
class QSlider;
class QTimer;
class QToolButton;

// Isometric, animated 3D preview of a machining program: stock wireframe,
// rapids dashed grey, cuts colored by depth (same palette as the 2D canvas
// overlay), a tool glyph that travels the route in (scaled) real time, and an
// optional second marker for the live machine position. Pure QPainter.
namespace c2d {

class IsoView;

class IsoPreview : public QWidget
{
    Q_OBJECT
public:
    explicit IsoPreview(QWidget *parent = nullptr);

    // Program to show (ops as returned by exportGcode()). Stock box spans
    // X 0..stockW, Y 0..stockH (Y up, CC convention), Z -stockT..0.
    void setJob(const QVector<Op> &ops, double stockW, double stockH, double stockT);

    // Real machine work position (drawn as an orange crosshair); valid=false hides it.
    void setLivePosition(double x, double y, double z, bool valid);

    void play();
    void pause();
    bool isPlaying() const;
    void setProgress(double fraction);   // 0..1 of total job time
    double progress() const;
    void resetView();

    QSize sizeHint() const override { return {360, 520}; }

private:
    void tick();
    void updateStatus();
    void syncSlider();

    IsoView *m_view;
    QToolButton *m_playBtn;
    QSlider *m_slider;
    QComboBox *m_speed;
    QLabel *m_status;
    QTimer *m_timer;
    QElapsedTimer m_clock;
};

} // namespace c2d
