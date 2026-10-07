#pragma once
#include "gamepad.h"

#include <QDialog>

class QLabel;
class QTableWidget;

namespace c2d {

// Edits the gamepad button mapping. Pressing a button on the pad selects its
// row (adding one if the button is not mapped yet), so a clone with unknown
// numbering is mapped by pressing each button and picking what it should do,
// rather than by reading numbers off the console and editing settings.
//
// The owner must not act on pad presses while this is open: mapping Stop
// should not stop the machine.
class GamepadMapDialog : public QDialog
{
    Q_OBJECT
public:
    GamepadMapDialog(const GamepadMap &map, Gamepad *pad, QWidget *parent = nullptr);

    GamepadMap mapping() const;

    // Select (adding if needed) the row for `number`. What a pad press does;
    // public so the tests can drive it without a pad.
    void selectButton(int number);

private:
    void setMapping(const GamepadMap &map);
    int addRow(int number, const QString &action);
    int rowFor(int number) const;

    QTableWidget *m_table;
    QLabel *m_hint;
};

} // namespace c2d
