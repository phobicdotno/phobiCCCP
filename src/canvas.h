#pragma once
#include "c2ddocument.h"
#include "post_grbl.h"
#include <QGraphicsView>
#include <QSet>
#include <functional>

class QGraphicsPathItem;
class QUndoCommand;
class QUndoStack;
class QLabel;
class QLineEdit;

// Renders a Document's board and elements, and hosts the vector tools. CC uses
// Y-up with the origin at the board's bottom-left; Qt scene coordinates are
// Y-down, so the view is flipped vertically — scene coordinates ARE CC
// millimetre coordinates. Board surface, grid and origin axes are painted in
// drawBackground; elements are scene items keyed by element id in data(0).
namespace c2d {

struct BackgroundImage;

// A constraint's symbol (the canvas badge glyph) as a picture, `size` logical
// px square, for toolbar buttons and lists. Dimensions get a dimension icon.
QPixmap constraintPixmap(Constraint::Type t, const QColor &ink, int size, qreal dpr);

class Canvas : public QGraphicsView
{
    Q_OBJECT
public:
    enum Tool { Select, DrawCircle, DrawRect, DrawPolygon, DrawPath, DrawText, NodeEdit,
                DrawEllipse, DrawSlot, DrawArc, Trim, Extend, Break, Measure,
                DrawPoint, DrawSpline, DrawConic, Pan, SelectArea,
                Constrain, Dimension };
    // The three-click tools (slot, 3-point arc): the first
    // two clicks fix two points, the third finishes the shape.
    static bool isClickTool(Tool t) { return t == DrawSlot || t == DrawArc || t == DrawConic; }
    // Trim / Extend / Break: hover shows what a click would do to the curve
    // under the cursor, bounded by every other curve in the drawing.
    static bool isModifyTool(Tool t) { return t == Trim || t == Extend || t == Break; }
    // Fusion's sketch variants of the Circle and Rectangle tools.
    enum CircleMode { CircleCenterDiameter, Circle2Point, Circle3Point,
                      Circle2Tangent, Circle3Tangent };
    enum RectMode { Rect2Point, Rect3Point, RectCenter };
    // ... and of the Arc, Polygon and Slot tools.
    enum ArcMode { Arc3Point, ArcCenter, ArcTangent };
    enum PolygonMode { PolyInscribed, PolyCircumscribed, PolyEdge };
    enum SlotMode { SlotCenterToCenter, SlotOverall, SlotCenterPoint, SlotArc3Point };

    explicit Canvas(QWidget *parent = nullptr);
    ~Canvas() override;
    void setDocument(Document *doc);   // non-owning; nullptr clears
    void setTool(Tool t);
    Tool tool() const { return m_tool; }
    void setPolygonSides(int n) { m_polySides = qBound(3, n, 64); }
    // The conic tool's rho (0.5 parabola, lower flatter, higher fuller).
    void setConicRho(double rho) { m_conicRho = qBound(0.05, rho, 0.95); }
    double conicRho() const { return m_conicRho; }
    void setCircleMode(CircleMode m);
    void setRectMode(RectMode m);
    void setArcMode(ArcMode m);
    void setPolygonMode(PolygonMode m);
    void setSlotMode(SlotMode m);
    // Fusion's construction toggle: marks the vectors construction geometry
    // (dashed, never machined), or back, as one undo step. Mixed selections
    // all become construction.
    void toggleConstruction(const QStringList &ids);
    void deleteSelected();                 // Edit > Delete, and the Delete key
    void setSnapEnabled(bool on) { m_snap = on; }
    bool snapEnabled() const { return m_snap; }
    QUndoStack *undoStack() const { return m_undo; }
    void zoomFit();
    void zoomBy(double factor);   // zoom about the middle of the view
    // The - / + zoom buttons with the zoom % and the live X / Y readout. It floats
    // bottom right on the canvas until the owner puts it in a layout of its own.
    QWidget *zoomBar() const { return m_hud; }
    void rebuild();                    // re-sync the scene from the document
    // Tracing background picture, painted under the grid (non-owning; nullptr clears).
    void setBackgroundImage(const BackgroundImage *bg);

    // Numeric edits from the properties panel (undoable).
    void editElement(const QString &id, const QHash<QString, double> &params);
    void moveElementBy(const QString &id, double dx, double dy);
    // Text edits (string, font, arc settings; see Element::regenText). Undoable.
    void editText(const QString &id, const QJsonObject &changes);
    // "Convert to path": replace the elements by editable path elements. Undoable.
    void convertToPaths(const QStringList &ids);
    // Replace a toolpath's JSON payload (undoable; from the toolpath panel).
    void editToolpath(const QString &uuid, const QJsonObject &newJson);
    // Insert generated elements plus a new toolpath in one shot (inlay male
    // generator), as one undo step.
    void insertGenerated(const QVector<Element> &els, const Toolpath &tp);
    QStringList selectedElementIds() const;
    Document *document() const { return m_doc; }
    void selectIds(const QStringList &ids);   // replace the selection (vector ops)
    void selectElements(const QStringList &ids);
    // Select the closed shape around p (innermost) and its islands.
    void selectAreaAt(const QPointF &p, bool add = false);

    // Amber halo over the vectors a toolpath machines, so selecting a row in
    // the toolpath list answers "which shapes does this one cut?".
    void setVectorHighlight(const QStringList &ids);
    void clearVectorHighlight() { setVectorHighlight({}); }
    const QStringList &vectorHighlight() const { return m_highlightIds; }

    // On-canvas g-code preview: rapids dashed, cuts colored by depth.
    void setToolpathPreview(const QVector<Op> &ops);
    void clearToolpathPreview();
    bool hasToolpathPreview() const { return !m_previewOps.isEmpty(); }

    // ---- parametric sketching (canvasparametric.cpp) -----------------------
    // The Constrain tool's constraint (Coincident ... Symmetric).
    void setConstraintType(Constraint::Type t);
    Constraint::Type constraintType() const { return m_conType; }
    // Push a geometry edit. With constraints in the document the solver runs
    // right after it, in the same undo step, moving `touched` least.
    // `moved`: the transform the edit carried the touched elements through
    // (a mirror, rotation or scale); their constraints follow the geometry
    // through it, and after a mirror which side each holds on is taken
    // afresh. A shape locked whole by a Fix is put back where it was.
    void pushEdit(QUndoCommand *cmd, const QStringList &touched, const QTransform &moved = QTransform());
    // The warning the last pushEdit put in the status bar (constraints that
    // can't hold, shapes held back by Fix, constraints dropped), or empty. An
    // action that reports its own result appends it so it stays visible.
    QString editWarning() const { return m_editWarning; }
    // Swap in new parameters / constraints as one undo step: re-solve, then
    // re-evaluate the toolpath fields bound to expressions. When the result
    // cannot hold, nothing changes and false comes back - unless
    // allowFailing (a parameter edit), which keeps it and marks the
    // constraints that fail in red. Constraints already red before the
    // edit are left out of the solve, so they neither block it nor bend
    // the rest to a compromise.
    // `mutate` runs on the document after the swap, before the solve (a
    // parameter rename also rewrites the toolpath bindings that use it).
    bool applyParametric(const Parametric &after, const QString &text,
                         bool allowFailing = false, const QSet<QString> &keep = {},
                         const std::function<void(Document &)> &mutate = {});
    // What the last applyParametric's toolpath bindings reported: fields
    // whose expression can't be worked out (kept) or that were clamped.
    const QStringList &bindingProblems() const { return m_bindProblems; }
    void editDimension(const QString &uid);       // the inline value editor
    void deleteConstraint(const QString &uid);
    // Constraint glyphs and dimensions on the canvas: the user's toggle,
    // and whether the page shows them at all (Design does, Toolpaths not).
    void setShowConstraints(bool on);
    bool showConstraints() const { return m_showConstraints; }
    void setParametricVisible(bool on);
    QString selectedConstraint() const { return m_selConstraint; }
    void selectConstraint(const QString &uid);
    // Elements nothing can move any more (drawn in the constrained colour)
    // and the degrees of freedom left, as of the last rebuild.
    const QSet<QString> &fullyConstrained() const { return m_full; }
    int freedomLeft() const { return m_dof; }
    const QStringList &failingConstraints() const { return m_failing; }

signals:
    void toolChanged(Canvas::Tool t);  // active tool switched (setTool)
    void documentChanged();            // element added / moved / deleted
    void statusHint(const QString &msg);
    void cursorMoved(QPointF ccPos);   // live mm position under the mouse
    void zoomChanged(double percent);
    void selectionChangedIds(const QStringList &ids);
    void constraintSelected(const QString &uid);   // "" = none

protected:
    void resizeEvent(QResizeEvent *event) override;
    void drawBackground(QPainter *painter, const QRectF &rect) override;
    void drawForeground(QPainter *painter, const QRectF &rect) override;
    void wheelEvent(QWheelEvent *event) override;        // scroll to zoom
    bool viewportEvent(QEvent *event) override;          // two-finger pinch zooms and pans
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;       // Del removes selection
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    QPainterPath previewPath(const QPointF &cur) const;
    QPointF snap(QPointF p) const;
    double gridSpacing() const;
    double pxToMm(double px) const;     // screen tolerance in scene units
    void finishPath(bool closed);
    void cancelDrawing();
    void onSelectionChanged();
    void emitZoom();
    void placeHud();
    QWidget *m_hud = nullptr;           // bottom-right: zoom - / + and the live readout
    QLabel *m_hudText = nullptr;
    QPointF m_hudPos;
    bool m_hudHasPos = false;
    void updateHud();
    void renderPreviewOps();
    void removePreviewItems();          // drop the g-code overlay items only
    QList<QGraphicsItem *> m_previewItems;
    void clearPreview();                // drop the live outline (m_preview)
    void syncHitTolerance();            // re-size the items' hit corridor to the zoom
    double m_hitScale = 0;              // view scale the corridor was sized for
    QGraphicsPathItem *itemFor(const QString &id) const;
    // The items of all of `ids` in one walk of the scene (itemFor per id
    // walked it per id: seconds with thousands of shapes).
    QList<QGraphicsPathItem *> itemsFor(const QStringList &ids) const;

    // Node editor.
    enum NodeGrab { GrabNone, GrabAnchor, GrabIn, GrabOut };
    void restoreEditItem();             // undo a dropped drag's live item path
    void syncEditTarget();              // pick the node-edit target from the selection
    bool hitNode(const QPointF &scenePos, int *sub, int *node, NodeGrab *what) const;
    bool hitSegment(const QPointF &scenePos, int *sub, int *seg, double *t) const;
    void commitModel(const QString &what);   // push EditCmd(before, model) if changed
    void previewModel();                     // live item update during a drag
    void drawNodes(QPainter *p) const;

    QGraphicsScene *m_scene;
    Document *m_doc = nullptr;
    QUndoStack *m_undo;
    Tool m_tool = Select;
    int m_polySides = 6;
    double m_conicRho = 0.5;
    bool m_snap = false;
    bool m_drawing = false;
    QPointF m_anchor;                       // scene coords (CC mm)

    // Sketch circle/rectangle modes: points clicked so far and, for the
    // tangent circles, the edges picked so far. One shape per completed set.
    CircleMode m_circleMode = CircleCenterDiameter;
    RectMode m_rectMode = Rect2Point;
    QVector<QPointF> m_clicks;
    QVector<QLineF> m_tanLines;
    bool sketchTool() const { return m_tool == DrawCircle || m_tool == DrawRect; }
    int sketchPointsNeeded() const;
    int sketchLinesNeeded() const;
    void sketchHint();
    bool pickEdge(const QPointF &scenePos, QLineF *edge) const;
    QPainterPath sketchPath(const QPointF &cur) const;
    void finishSketch(const QPointF &cur);
    void resetSketch();
    QGraphicsPathItem *m_preview = nullptr; // live outline while dragging
    QStringList m_highlightIds;             // vectors of the selected toolpath
    bool m_fitted = false;                  // fitInView only on first load
    bool m_panning = false;                 // middle-mouse pan
    QPoint m_panLast;                       // viewport coords during pan
    // Select band (viewport coords). Dragged left to right it is a window
    // (only what lies wholly inside); right to left a crossing (whatever
    // line it touches). Ctrl adds to the selection it started with.
    bool m_band = false;
    QPoint m_bandStart, m_bandEnd;
    QSet<QGraphicsItem *> m_bandKeep;
    void applyBand();

    // Click tools (slot, arc, edge polygon) reuse m_clicks (the points
    // clicked so far, CC mm); how many points finish a shape depends on
    // the tool's mode. clickShape builds the shape from a full set.
    bool clickTool() const;
    int clicksNeeded() const;
    QString clickHint() const;
    void clickToolPress(const QPointF &pos);
    QPainterPath clickPreview(const QPointF &cur) const;
    bool clickShape(const QVector<QPointF> &pts, Element *out) const;
    ArcMode m_arcMode = Arc3Point;
    PolygonMode m_polyMode = PolyInscribed;
    SlotMode m_slotMode = SlotCenterToCenter;
    // Tangent arc: where it leaves the picked open end, and which way.
    bool pickOpenEnd(const QPointF &q, QPointF *end, QPointF *dir) const;
    QPointF m_tanDir;
    // Center-point arc: the turn swept so far from the start ray (radians,
    // + counter-clockwise), followed with the mouse so the arc goes the way
    // the cursor went.
    double m_arcTurn = 0, m_arcLastAng = 0;
    void trackArcTurn(const QPointF &cur);
    // Fit-point spline: the points clicked so far.
    QVector<QPointF> m_splinePts;
    void finishSpline(bool closed);
    bool m_fromCenter = false;              // Ctrl held: rectangle / ellipse from the center

    // Trim / Extend / Break.
    bool modifyTarget(const QPointF &q, QString *id, PathModel *model,
                      QVector<PathModel> *others) const;
    QPainterPath modifyPreview(const QPointF &q) const;
    void modifyAt(const QPointF &q);

    // Pen (path) tool: click = corner, click-drag = symmetric handles.
    QVector<PathNode> m_penNodes;
    bool m_penDrag = false;

    // Node editor state: the element being edited, its decoded model, the
    // selected node and the drag in progress (one undo step per drag).
    QString m_editId;
    PathModel m_model;
    int m_selSub = -1, m_selNode = -1;
    NodeGrab m_grab = GrabNone;
    bool m_grabBreak = false;               // Alt/Shift held when the handle was grabbed

    QVector<Op> m_previewOps;               // g-code overlay (empty = off)
    const BackgroundImage *m_bg = nullptr;  // background picture (backgroundimage.h)

    // Parametric overlay and tools (canvasparametric.cpp).
    struct DimDraw;
    void drawParametric(QPainter *p);
    bool dimGeometry(const Constraint &c, DimDraw *out) const;
    bool refShape(const GeoRef &r, param::RefShape *out) const;
    bool pickGeo(const QPointF &pos, unsigned mask, param::RefShape *out) const;
    unsigned pickMask() const;
    void constrainClick();
    void dimensionClick(const QPointF &pos, Qt::KeyboardModifiers mods);
    void updateDimDraft(const QPointF &pos);
    void resetPicks();
    QString hitConstraint(const QPoint &viewPos) const;
    void openDimEditor(const Constraint &c, bool isNew);
    void commitDimEditor(bool keepMeasured);
    void closeDimEditor();
    void placeDimEditor();                        // over its dimension, for the current view
    bool parametricPress(QMouseEvent *event);     // true = handled
    bool parametricMove(QMouseEvent *event);
    bool parametricRelease(QMouseEvent *event);
    bool parametricKey(QKeyEvent *event);
    bool parametricMenu(QContextMenuEvent *event);
    void refreshParametricState();                // m_full, m_dof, m_failing
    bool overlayOn() const { return m_showConstraints && m_parametricVisible; }

    Constraint::Type m_conType = Constraint::Coincident;
    QVector<param::RefShape> m_picks;             // geometry picked so far
    param::RefShape m_hover;                      // geometry under the cursor
    bool m_hasHover = false;
    QString m_hoverConstraint;                    // glyph / label under the cursor
    QString m_selConstraint;
    QString m_editWarning;                        // see editWarning()
    bool m_dimPlacing = false;                    // a new dimension follows the mouse
    Constraint m_dimDraft;
    QLineEdit *m_dimEdit = nullptr;
    Constraint m_dimEditing;                      // what the editor edits
    bool m_dimEditingNew = false;
    QPointF m_dimEditAt;                          // scene fallback when it has no geometry
    QStringList m_bindProblems;                   // bindingProblems()
    QString m_dragDim;                            // dimension label being dragged
    QPointF m_dragDimFrom;
    QPointF m_dragDimLabel0;
    bool m_showConstraints = true, m_parametricVisible = true;
    QSet<QString> m_full;
    int m_dof = 0;
    QStringList m_failing;
    mutable QHash<QString, QVector<param::RefShape>> m_shapeCache;
    struct Hit { QRectF rect; QString uid; };
    QVector<Hit> m_hits;                          // glyph / label rects, view px

    // Resize-handle drag state (single selected circle/rect/polygon).
    bool resizeHandle(QString *id, QPointF *pos, QString *type) const;
    // The size the handle at `cur` asks for (width / height or radius).
    QHash<QString, double> resizeTo(QPointF cur) const;
    bool m_resizing = false;
    QString m_resizeId, m_resizeType;
    QPointF m_resizeCenter;
    double m_resizeRot = 0;     // a turned rectangle's rotation, degrees
};

} // namespace c2d
