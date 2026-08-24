#pragma once
#include "c2ddocument.h"
#include <QGraphicsView>

class QGraphicsPathItem;

// Renders a Document's board and elements, and hosts the vector tools. CC uses
// Y-up with the origin at the board's bottom-left; Qt scene coordinates are
// Y-down, so the view is flipped vertically — scene coordinates ARE CC
// millimetre coordinates.
namespace c2d {

class Canvas : public QGraphicsView
{
    Q_OBJECT
public:
    enum Tool { Select, DrawCircle, DrawRect, DrawPolygon };

    explicit Canvas(QWidget *parent = nullptr);
    void setDocument(Document *doc);   // non-owning; nullptr clears
    void setTool(Tool t);
    Tool tool() const { return m_tool; }
    void setPolygonSides(int n) { m_polySides = qBound(3, n, 64); }
    void rebuild();                    // re-sync the scene from the document

signals:
    void documentChanged();            // element added / moved / deleted
    void statusHint(const QString &msg);

protected:
    void wheelEvent(QWheelEvent *event) override;        // scroll to zoom
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;       // Del removes selection

private:
    QPainterPath previewPath(const QPointF &cur) const;

    QGraphicsScene *m_scene;
    Document *m_doc = nullptr;
    Tool m_tool = Select;
    int m_polySides = 6;
    bool m_drawing = false;
    QPointF m_anchor;                       // scene coords (CC mm)
    QGraphicsPathItem *m_preview = nullptr; // live outline while dragging
    bool m_fitted = false;                  // fitInView only on first load
};

} // namespace c2d
