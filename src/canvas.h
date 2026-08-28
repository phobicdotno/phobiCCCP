#pragma once
#include "c2ddocument.h"
#include "post_grbl.h"
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
    enum Tool { Select, DrawCircle, DrawRect, DrawPolygon, DrawPath, DrawText };

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
    // Replace a toolpath's JSON payload (undoable; from the toolpath panel).
    void editToolpath(const QString &uuid, const QJsonObject &newJson);
    // Insert generated elements plus a new toolpath in one shot (inlay male
    // generator). Not undoable — delete the pieces manually to revert.
    void insertGenerated(const QVector<Element> &els, const Toolpath &tp);
    QStringList selectedElementIds() const;

    // On-canvas g-code preview: rapids dashed, cuts colored by depth.
    void setToolpathPreview(const QVector<Op> &ops);
    void clearToolpathPreview();
    bool hasToolpathPreview() const { return !m_previewOps.isEmpty(); }

signals:
    void documentChanged();            // element added / moved / deleted
    void statusHint(const QString &msg);
    void cursorMoved(QPointF ccPos);   // live mm position under the mouse
    void zoomChanged(double percent);
    void selectionChangedIds(const QStringList &ids);

protected:
    void drawBackground(QPainter *painter, const QRectF &rect) override;
    void drawForeground(QPainter *painter, const QRectF &rect) override;
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
    void renderPreviewOps();

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

    QVector<Op> m_previewOps;               // g-code overlay (empty = off)

    // Resize-handle drag state (single selected circle/rect/polygon).
    bool resizeHandle(QString *id, QPointF *pos, QString *type) const;
    bool m_resizing = false;
    QString m_resizeId, m_resizeType;
    QPointF m_resizeCenter;
};

} // namespace c2d
