#include "toolbarorder.h"

#include <QAction>
#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QSettings>
#include <QToolBar>
#include <QToolButton>
#include <QTimer>

namespace c2d {

namespace {
const char *kMime = "application/x-phobicccp-toolbar-action";
}

ToolbarOrder::ToolbarOrder(QToolBar *bar, const QString &settingsKey)
    : QObject(bar), m_bar(bar), m_key(settingsKey)
{
    m_bar->setAcceptDrops(true);
    m_bar->installEventFilter(this);
    m_bar->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_bar, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QMenu menu;
        menu.addAction(QStringLiteral("Drag icons sideways to rearrange them"))->setEnabled(false);
        menu.addAction(QStringLiteral("Reset tool order"), this, [this] {
            applyOrder(m_default);
            QSettings().remove(m_key);
        });
        menu.exec(m_bar->mapToGlobal(pos));
    });

    m_marker = new QWidget(m_bar);
    m_marker->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_marker->setStyleSheet(QStringLiteral("background: #f2a536;"));
    m_marker->hide();
}

void ToolbarOrder::restore()
{
    m_default.clear();
    for (QAction *a : m_bar->actions())
        m_default << a->objectName();
    const QStringList saved = QSettings().value(m_key).toStringList();
    if (!saved.isEmpty())
        applyOrder(saved);
    prepareButtons();
}

// Saved names first, in their order; anything the saved list does not know
// (a tool added since) keeps its default place at the end.
void ToolbarOrder::applyOrder(const QStringList &names)
{
    QList<QAction *> all = m_bar->actions();
    QList<QAction *> ordered;
    for (const QString &n : names)
        for (QAction *a : all)
            if (a->objectName() == n && !ordered.contains(a))
                ordered << a;
    for (QAction *a : all)
        if (!ordered.contains(a))
            ordered << a;
    for (QAction *a : ordered) {
        m_bar->removeAction(a);
        m_bar->addAction(a);
    }
    prepareButtons();
}

void ToolbarOrder::prepareButtons()
{
    for (QAction *a : m_bar->actions()) {
        auto *btn = qobject_cast<QToolButton *>(m_bar->widgetForAction(a));
        if (!btn)
            continue;
        if (a->menu())
            btn->setPopupMode(QToolButton::DelayedPopup);
        btn->removeEventFilter(this);
        btn->installEventFilter(this);
    }
}

void ToolbarOrder::save() const
{
    QStringList names;
    for (QAction *a : m_bar->actions())
        names << a->objectName();
    QSettings().setValue(m_key, names);
}

int ToolbarOrder::dropIndex(const QPoint &barPos) const
{
    const QList<QAction *> acts = m_bar->actions();
    for (int i = 0; i < acts.size(); ++i) {
        const QWidget *w = m_bar->widgetForAction(acts[i]);
        if (w && barPos.x() < w->geometry().center().x())
            return i;
    }
    return int(acts.size());
}

void ToolbarOrder::showDropMarker(int index)
{
    const QList<QAction *> acts = m_bar->actions();
    if (acts.isEmpty())
        return;
    int x;
    QRect ref = m_bar->widgetForAction(acts.value(qMin(index, int(acts.size()) - 1)))->geometry();
    x = index < acts.size() ? ref.left() - 2 : ref.right() + 1;
    m_marker->setGeometry(x, ref.top(), 3, ref.height());
    m_marker->show();
    m_marker->raise();
}

void ToolbarOrder::moveAction(QAction *a, int toIndex)
{
    QList<QAction *> acts = m_bar->actions();
    const int from = int(acts.indexOf(a));
    if (from < 0)
        return;
    if (toIndex > from)
        --toIndex;                    // the gap it leaves closes up
    if (toIndex == from)
        return;
    acts.removeAt(from);
    acts.insert(toIndex, a);
    QStringList names;
    for (QAction *x : acts)
        names << x->objectName();
    applyOrder(names);
    save();
}

bool ToolbarOrder::eventFilter(QObject *watched, QEvent *event)
{
    // The bar: accept and place drops.
    if (watched == m_bar) {
        switch (event->type()) {
        case QEvent::DragEnter:
        case QEvent::DragMove: {
            auto *e = static_cast<QDropEvent *>(event);
            if (!e->mimeData()->hasFormat(QString::fromLatin1(kMime)))
                return false;
            e->acceptProposedAction();
            showDropMarker(dropIndex(e->position().toPoint()));
            return true;
        }
        case QEvent::DragLeave:
            m_marker->hide();
            return true;
        case QEvent::Drop: {
            auto *e = static_cast<QDropEvent *>(event);
            m_marker->hide();
            if (!e->mimeData()->hasFormat(QString::fromLatin1(kMime)))
                return false;
            const QString name = QString::fromUtf8(e->mimeData()->data(QString::fromLatin1(kMime)));
            const int index = dropIndex(e->position().toPoint());
            e->acceptProposedAction();
            // Moving an action makes QToolBar delete its old button, which is
            // the one the drag started from and is still on the stack. Do it
            // once everything has unwound.
            QTimer::singleShot(0, this, [this, name, index] {
                for (QAction *a : m_bar->actions())
                    if (a->objectName() == name) {
                        moveAction(a, index);
                        break;
                    }
            });
            return true;
        }
        default:
            return false;
        }
    }

    // A button: a press followed by a sideways drag starts a move. Anything
    // shorter stays a click (or the press-and-hold menu).
    auto *btn = qobject_cast<QToolButton *>(watched);
    if (!btn)
        return false;
    if (event->type() == QEvent::MouseButtonPress) {
        auto *e = static_cast<QMouseEvent *>(event);
        if (e->button() == Qt::LeftButton) {
            m_pressPos = e->position().toPoint();
            m_pressed = btn;
        }
    } else if (event->type() == QEvent::MouseButtonRelease) {
        m_pressed = nullptr;
    } else if (event->type() == QEvent::MouseMove && m_pressed == btn) {
        auto *e = static_cast<QMouseEvent *>(event);
        if (!(e->buttons() & Qt::LeftButton))
            return false;
        if ((e->position().toPoint() - m_pressPos).manhattanLength()
            < QApplication::startDragDistance() * 2)
            return false;
        m_pressed = nullptr;
        QAction *a = btn->defaultAction();
        if (!a)
            return false;
        // Let go of the button first: a press that is still down when the
        // popup delay runs out would open the variants menu mid-drag.
        btn->setDown(false);
        auto *mime = new QMimeData;
        mime->setData(QString::fromLatin1(kMime), a->objectName().toUtf8());
        auto *drag = new QDrag(m_bar);
        drag->setMimeData(mime);
        drag->setPixmap(a->icon().pixmap(m_bar->iconSize()));
        drag->setHotSpot(QPoint(m_bar->iconSize().width() / 2, m_bar->iconSize().height() / 2));
        drag->exec(Qt::MoveAction);
        m_marker->hide();
        return true;
    }
    return false;
}

} // namespace c2d
