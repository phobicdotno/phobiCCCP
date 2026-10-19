#pragma once
#include <QObject>
#include <QPoint>

class QAbstractScrollArea;
class QWidget;

namespace c2d {

// How long a toolpath card must be held still before it can be dragged to a
// new place. Moving the finger (or mouse) restarts the wait, so time spent
// scrolling never counts towards it.
constexpr int kHoldToMoveMs = 500;

// Scrolls the nearest scroll area at or above w by dy pixels, handing what is
// left to the next one up when it is already at its end. True if anything
// moved.
bool dragScrollBy(QWidget *w, int dy);

// Press and drag on the open background of a scroll area (anywhere a control
// does not take the press itself) scrolls it, as on a touch screen.
class DragScroll : public QObject
{
public:
    static void install(QAbstractScrollArea *area);

protected:
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    explicit DragScroll(QAbstractScrollArea *area);
    QAbstractScrollArea *m_area;
    QPoint m_origin, m_last;
    bool m_down = false;
    bool m_scrolling = false;
};

} // namespace c2d
