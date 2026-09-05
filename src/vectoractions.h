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
// plus a small icon toolbar for the common ones. Every operation is a single
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

private:
    void onSelection(const QStringList &ids);
    QVector<Element> orderedSelection() const;
    void updateEnabled();

    void boolean(vec::BoolOp op);
    void offset();
    void align(vec::Align mode);
    void centerOnStock(vec::Center mode);
    void distribute(vec::Axis axis);
    void pushMoves(const QStringList &ids, const QVector<QPointF> &deltas, const QString &text);
    void pushReplace(const QVector<Element> &inputs, const QVector<Element> &results,
                     bool keepInputs, const QString &text);

    Canvas *m_canvas;
    QStringList m_order;               // selected ids, first-selected first
    QVector<QAction *> m_needOne, m_needTwo, m_needThree;
};

} // namespace c2d
