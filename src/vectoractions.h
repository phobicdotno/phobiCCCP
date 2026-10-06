#pragma once
#include "vectorops.h"
#include <QObject>
#include <QStringList>
#include <QVector>

class QAction;
class QMenu;
class QMainWindow;

namespace c2d {

class Canvas;

// Edit → Vectors: Carbide Create's Booleans (Union / Subtract / Intersect),
// Offset… and Alignment (align to selection, center on stock, distribute),
// Mirror and grid / circular arrays, and Fusion's Modify tools (rotate,
// scale, move/copy, fillet, chamfer), plus a small icon toolbar for the
// common ones. Every operation is a single
// QUndoStack command on the canvas' stack.
//
// Selection order matters for Subtract (first selected minus the others):
// the order is tracked from the canvas' selection signal — Ctrl/Shift-click
// order is honoured; when several elements arrive at once (rubber band,
// Ctrl+A) the one with the largest bounding box is treated as first.
class VectorActions : public QObject
{
    Q_OBJECT
public:
    VectorActions(Canvas *canvas, QMenu *editMenu, QMainWindow *window);

    // The dialog-free halves of Mirror and the arrays, on the current
    // selection (public for the tests). Mirror flips about the selection's
    // own center. The arrays add copies as one undo step; with
    // `joinToolpaths` every toolpath that machines an original machines its
    // copies too. See vec::gridTransforms / vec::circularTransforms.
    void mirror(vec::Axis axis);
    void gridArray(int cols, int rows, double gapX, double gapY, bool joinToolpaths);
    void circularArray(QPointF center, int count, double spanDeg, bool rotate,
                       bool joinToolpaths);
    // Fusion's Modify tools, dialog-free. Rotate and scale work about the
    // selection's center; moveCopy moves it by (dx, dy), or with copies > 0
    // leaves it in place and adds that many copies, each one (dx, dy)
    // further on. corners() fillets or chamfers every sharp line-line
    // corner of the selected vectors (vec::cornerElement).
    void rotate(double deg);
    void scale(double sx, double sy);
    void moveCopy(double dx, double dy, int copies, bool joinToolpaths);
    void corners(vec::CornerStyle style, double size);

private:
    void onSelection(const QStringList &ids);
    QVector<Element> orderedSelection() const;
    void updateEnabled();

    void boolean(vec::BoolOp op);
    void offset();
    void align(vec::Align mode);
    void centerOnStock(vec::Center mode);
    void distribute(vec::Axis axis);
    void gridArrayDialog();
    void circularArrayDialog();
    void rotateDialog();
    void scaleDialog();
    void moveCopyDialog();
    void cornersDialog(vec::CornerStyle style);
    void pushReshape(const QVector<Element> &before, const QVector<Element> &after,
                     const QString &text);
    void pushCopies(const QVector<Element> &originals, const QVector<QTransform> &placements,
                    bool joinToolpaths, const QString &text);
    void pushMoves(const QStringList &ids, const QVector<QPointF> &deltas, const QString &text);
    void pushReplace(const QVector<Element> &inputs, const QVector<Element> &results,
                     bool keepInputs, const QString &text);

    Canvas *m_canvas;
    QStringList m_order;               // selected ids, first-selected first
    QVector<QAction *> m_needOne, m_needTwo, m_needThree;
};

} // namespace c2d
