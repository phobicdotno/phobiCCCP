#pragma once
#include "c2ddocument.h"
#include <QGraphicsView>

class QGraphicsPathItem;
class QUndoStack;

// Renders a Document's board and elements, and hosts the vector tools. CC uses
// Y-up with the origin at the board's bottom-left; Qt scene coordinates are
// Y-down, so the view is flipped vertically — scene coordinates ARE CC
// millimetre coordinates. Board surface, grid and origin axes are painted in
// drawBackground; elements are scene items keyed by element id in data(0).
namespace c2d {

class Canvas : public QGraphicsView
{
    Q_OBJECT
public:
    enum Tool { Select, DrawCircle, DrawRect, DrawPolygon, DrawPath };

    explicit Canvas(QWidget *parent = nullptr);
    void setDocument(Document *doc);   // non-owning; nullptr clears
    void setTool(Tool t);
    Tool tool() const { return m_tool; }
    void setPolygonSides(int n) { m_polySides = qBound(3, n, 64); }
    void setSnapEnabled(bool on) { m_snap = on; }
    bool snapEnabled() const { return m_snap; }
    QUndoStack *undoStack() const { return m_undo; }
    void zoomFit();
    void rebuild();                    // re-sync the scene from the document

    // Numeric edits from the properties panel (undoable).
    void editElement(const QString &id, const QHash<QString, double> &params);
    void moveElementBy(const QString &id, double dx, double dy);

signals:
    void documentChanged();            // element added / moved / deleted
    void statusHint(const QString &msg);
    void cursorMoved(QPointF ccPos);   // live mm position under the mouse
    void zoomChanged(double percent);
    void selectionChangedIds(const QStringList &ids);

protected:
    void drawBackground(QPainter *painter, const QRectF &rect) override;
    void wheelEvent(QWheelEvent *event) override;        // scroll to zoom
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;       // Del removes selection

private:
    QPainterPath previewPath(const QPointF &cur) const;
    QPointF snap(QPointF p) const;
    double gridSpacing() const;
    void finishPath(bool closed);
    void cancelDrawing();
    void onSelectionChanged();
    void emitZoom();

    QGraphicsScene *m_scene;
    Document *m_doc = nullptr;
    QUndoStack *m_undo;
    Tool m_tool = Select;
    int m_polySides = 6;
    bool m_snap = false;
    bool m_drawing = false;
    QPointF m_anchor;                       // scene coords (CC mm)
    QVector<QPointF> m_pathPts;             // committed vertices (path tool)
    QGraphicsPathItem *m_preview = nullptr; // live outline while dragging
    bool m_fitted = false;                  // fitInView only on first load
    bool m_panning = false;                 // middle-mouse pan
    QPoint m_panLast;                       // viewport coords during pan
};

} // namespace c2d
