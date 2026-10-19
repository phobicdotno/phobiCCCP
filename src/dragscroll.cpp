#include "dragscroll.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QMouseEvent>
#include <QScrollBar>

namespace c2d {

bool dragScrollBy(QWidget *w, int dy)
{
    for (QWidget *p = w; p && dy; p = p->parentWidget()) {
        auto *a = qobject_cast<QAbstractScrollArea *>(p);
        if (!a)
            continue;
        QScrollBar *bar = a->verticalScrollBar();
        const int before = bar->value();
        bar->setValue(before + dy);
        if (bar->value() != before)
            return true;
    }
    return false;
}

void DragScroll::install(QAbstractScrollArea *area)
{
    area->viewport()->installEventFilter(new DragScroll(area));
}

DragScroll::DragScroll(QAbstractScrollArea *area) : QObject(area), m_area(area) {}

// A press reaches the viewport only when nothing under it took it (labels,
// card backgrounds, gaps), so buttons and fields keep working as before.
bool DragScroll::eventFilter(QObject *, QEvent *e)
{
    switch (e->type()) {
    case QEvent::MouseButtonPress: {
        auto *me = static_cast<QMouseEvent *>(e);
        if (me->button() != Qt::LeftButton)
            return false;
        m_down = true;
        m_scrolling = false;
        m_origin = m_last = me->globalPosition().toPoint();
        e->accept();
        return true;
    }
    case QEvent::MouseMove: {
        if (!m_down)
            return false;
        auto *me = static_cast<QMouseEvent *>(e);
        const QPoint p = me->globalPosition().toPoint();
        if (!m_scrolling && (p - m_origin).manhattanLength() < QApplication::startDragDistance())
            return true;
        m_scrolling = true;
        dragScrollBy(m_area->viewport(), m_last.y() - p.y());
        m_last = p;
        return true;
    }
    case QEvent::MouseButtonRelease:
        if (!m_down)
            return false;
        m_down = false;
        return true;
    default:
        return false;
    }
}

} // namespace c2d
