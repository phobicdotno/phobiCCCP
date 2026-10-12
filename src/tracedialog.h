#pragma once
#include "imagetrace.h"
#include <QDialog>
#include <QImage>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QSlider;
class QSpinBox;
class QTimer;

namespace c2d {

class Canvas;
class Document;
class TracePreview;

// "Trace image…": threshold / invert / blur / despeckle / target width /
// simplify / smoothing controls with a live preview of the traced outlines
// drawn over the picture. elements() returns the closed CC path elements
// (outer contours and holes separately) for the current settings.
class TraceDialog : public QDialog
{
    Q_OBJECT
public:
    explicit TraceDialog(QWidget *parent = nullptr);

    // Picture to trace; `widthMm` is the initial target width, `origin` the
    // initial insert position (CC mm of the image's bottom-left corner).
    void setImage(const QImage &img, double widthMm, QPointF origin);

    TraceOptions options() const;
    const TraceResult &result() const { return m_result; }
    QVector<Element> elements(const QJsonObject &layer) const;

    // Push the elements onto the canvas undo stack as one "trace image"
    // command (undo removes them all again).
    static void insertUndoable(Canvas *canvas, Document *doc,
                               const QVector<Element> &els);

private:
    void scheduleTrace();
    void runTrace();
    void updateHeightLabel();

    QImage m_image;
    TraceResult m_result;
    TracePreview *m_preview;
    QSlider *m_threshold;
    QSpinBox *m_thresholdSpin;
    QCheckBox *m_invert;
    QSpinBox *m_blur;
    QDoubleSpinBox *m_minArea;
    QDoubleSpinBox *m_width;
    QLabel *m_heightLabel;
    QDoubleSpinBox *m_tolerance;
    QSlider *m_toleranceSlider;
    QCheckBox *m_smooth;
    QDoubleSpinBox *m_cornerAngle;
    QDoubleSpinBox *m_posX, *m_posY;
    QCheckBox *m_showMask;
    QLabel *m_status;
    QTimer *m_debounce;
};

} // namespace c2d
