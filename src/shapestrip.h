#pragma once
#include <QListWidget>
#include <QPoint>
#include <QTimer>

namespace c2d {

class Canvas;

// Design mode: a square preview tile for every shape in the drawing, in the
// order the document holds them. A click selects the shape, a drag reorders
// (one undo step), and a right click or a press-and-hold opens Duplicate /
// Delete.
class ShapeStrip : public QListWidget
{
    Q_OBJECT
public:
    explicit ShapeStrip(Canvas *canvas, QWidget *parent = nullptr);

public slots:
    void reload();

protected:
    void dropEvent(QDropEvent *e) override;
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    QStringList idsInOrder() const;
    QStringList selectedIds() const;
    void showMenu(const QPoint &viewportPos);
    void fromCanvas(const QStringList &ids);
    void toCanvas();

    Canvas *m_canvas;
    QTimer m_hold;
    QPoint m_pressAt;
    bool m_busy = false;
};

} // namespace c2d
