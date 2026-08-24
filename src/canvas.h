#pragma once
#include "c2ddocument.h"
#include <QGraphicsView>

// Renders a Document's board and elements. CC uses Y-up with the origin at the
// board's bottom-left; Qt scene coordinates are Y-down, so the view is flipped
// vertically and framed on the board rectangle.
//
// Editing: elements are selectable (click / rubber band) and movable (drag);
// a finished drag emits elementMoved() per item in scene mm. Middle-button
// drag pans, wheel zooms.
namespace c2d {

class Canvas : public QGraphicsView
{
    Q_OBJECT
public:
    explicit Canvas(QWidget *parent = nullptr);
    void setDocument(const Document *doc);   // rebuild scene and refit the view
    void refresh();                          // rebuild scene, keep the view transform
    QList<int> selectedElements() const;     // indices into Document::elements()

signals:
    // One drag gesture, all moved items at once (parallel lists, mm, Y-up).
    void elementsMoved(const QList<int> &indices, const QList<QPointF> &deltas);

protected:
    void wheelEvent(QWheelEvent *event) override;        // scroll to zoom
    void mousePressEvent(QMouseEvent *event) override;   // middle button pans
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override; // commit item drags

private:
    void rebuild(bool refit);

    QGraphicsScene *m_scene;
    const Document *m_doc = nullptr;
    bool m_panning = false;
    QPoint m_panStart;
};

} // namespace c2d
