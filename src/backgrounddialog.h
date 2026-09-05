#pragma once
#include "backgroundimage.h"
#include <QDialog>

class QCheckBox;
class QDoubleSpinBox;
class QMenu;
class QSlider;

namespace c2d {

class Canvas;
class Document;

// File → Background image → Adjust…: X/Y position, width (aspect kept),
// rotation, opacity, visibility and lock. Edits apply live to the canvas and
// are reverted on Cancel.
class BackgroundAdjustDialog : public QDialog
{
    Q_OBJECT
public:
    BackgroundAdjustDialog(BackgroundImage *bg, Canvas *canvas, QWidget *parent = nullptr);

private:
    void apply();
    void syncLock();

    BackgroundImage *m_bg;
    BackgroundImage m_backup;
    Canvas *m_canvas;
    QDoubleSpinBox *m_x, *m_y, *m_width, *m_rotation;
    QSlider *m_opacity;
    QCheckBox *m_visible, *m_locked;
    bool m_updating = false;
};

// Adds "Background image ▸ Set… / Clear / Adjust…" and "Trace image…" to the
// File menu and wires them to the canvas, the document and the window's
// BackgroundImage. Marks the document dirty through Canvas::documentChanged.
void installImageMenus(QMenu *fileMenu, QWidget *window, Canvas *canvas,
                       Document *doc, BackgroundImage *bg);

} // namespace c2d
