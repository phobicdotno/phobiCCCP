#pragma once
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QStringList>

class QAction;
class QToolBar;
class QToolButton;

namespace c2d {

// Drag-to-rearrange for an icon toolbar. Drag an icon sideways and drop it
// where it should go; the order is kept in QSettings under `settingsKey` and
// put back at the next start. Right-click the bar to reset it.
//
// Actions are identified by objectName, so every action on the bar needs a
// stable one. QToolBar makes a new button when an action moves, so the
// press-and-hold popup (QToolButton::DelayedPopup, for actions that carry a
// menu) is set up here, for every button the bar ever makes.
class ToolbarOrder : public QObject
{
    Q_OBJECT
public:
    ToolbarOrder(QToolBar *bar, const QString &settingsKey);

    // Call once the bar is fully populated: records the default order, then
    // applies the saved one.
    void restore();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void prepareButtons();
    void moveAction(QAction *a, int toIndex);
    int dropIndex(const QPoint &barPos) const;
    void applyOrder(const QStringList &names);
    void save() const;
    void showDropMarker(int index);

    QToolBar *m_bar;
    QString m_key;
    QStringList m_default;
    QPoint m_pressPos;
    QPointer<QToolButton> m_pressed;
    QWidget *m_marker = nullptr;     // insertion line shown while dragging
};

} // namespace c2d
