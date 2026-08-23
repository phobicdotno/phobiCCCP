#pragma once
#include "c2ddocument.h"
#include <QGraphicsView>

// Renders a Document's board and elements. CC uses Y-up with the origin at the
// board's bottom-left; Qt scene coordinates are Y-down, so the view is flipped
// vertically and framed on the board rectangle.
namespace c2d {

class Canvas : public QGraphicsView
{
    Q_OBJECT
public:
    explicit Canvas(QWidget *parent = nullptr);
    void setDocument(const Document *doc);

protected:
    void wheelEvent(QWheelEvent *event) override;   // scroll to zoom

private:
    QGraphicsScene *m_scene;
};

} // namespace c2d
