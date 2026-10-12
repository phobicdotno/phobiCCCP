#pragma once
#include "gamepad.h"

#include <QDialog>
#include <QHash>
#include <QPointer>

class QComboBox;
class QFrame;
class QHBoxLayout;
class QLabel;
class QPushButton;

namespace c2d {

class PadCanvas;

// Edits the gamepad button mapping on a big drawing of the pad: every button
// has a line out to a picker for what it does, and buttons light up while
// they are pressed on the real pad. A press also selects that button's
// picker, so a clone with unknown numbering is mapped by pressing each
// button and choosing. Buttons the drawing does not know (a clone's extras)
// get pickers in the "Other buttons" row underneath.
//
// Which pad number is which drawn button (A, B, Select…) is per pad style
// (NES or SNES) and stored under "gamepadLayout/<style>/<name>"; the
// defaults are the numbering of the clones on the shop PC.
//
// The owner must not act on pad presses while this is open: mapping Stop
// should not stop the machine.
class GamepadMapDialog : public QDialog
{
    Q_OBJECT
public:
    GamepadMapDialog(const GamepadMap &map, Gamepad *pad, QWidget *parent = nullptr);

    GamepadMap mapping() const;
    QString padStyle() const { return m_style; }   // "nes" or "snes"

    // Select (adding a picker if needed) the button `number`. What a pad
    // press does; public so the tests can drive it without a pad.
    void selectButton(int number);
    // The action picked for `number`: "none" when nothing, "" when the
    // dialog shows no picker for it at all.
    QString actionFor(int number) const;
    void setAction(int number, const QString &action);
    // Light a button (as a press on the pad does) or a D-pad direction:
    // axis 0 is X, 1 is Y; dir -1, 0 or +1.
    void showPressed(int number, bool on) { setPressed(number, on); }
    void showAxis(int axis, int dir);

private:
    struct Slot {
        int number = -1;
        QString control;           // "A", "SELECT"…, empty for "other" buttons
        QPointer<QFrame> box;
        QPointer<QComboBox> combo;
    };
    void build(const GamepadMap &map);
    Slot makeSlot(int number, const QString &control, const QString &action, QWidget *parent);
    int slotFor(int number) const;
    void setPressed(int number, bool on);

    PadCanvas *m_canvas = nullptr;
    QWidget *m_others = nullptr;
    QHBoxLayout *m_othersLay = nullptr;
    QLabel *m_hint = nullptr;
    QPushButton *m_nesBtn = nullptr, *m_snesBtn = nullptr;
    QList<Slot> m_slots;
    QString m_style;
};

} // namespace c2d
