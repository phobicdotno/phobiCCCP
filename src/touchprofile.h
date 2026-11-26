#pragma once
#include <QSize>
#include <QString>
#include <QStringList>

class QApplication;
class QStyle;
class QDialog;
class QSettings;

namespace c2d {

// Finger-sized controls: one place that decides how big buttons, icons, menu
// rows, tabs, list rows, scroll bars, check boxes and spin box arrows are when
// the program runs on a touch screen. On a pad it is on by default; on a
// computer it is off unless Settings asks for it (a touch-screen CNC PC).
// Off, every size is the desktop one, so nothing changes there.
//
// Sizes are logical pixels. On Android Qt makes a logical pixel one dp, so the
// 48 px target is the platform's 48 dp, about 7.5 mm on a 10-inch pad.
namespace touch {

// What Settings stores under ui/touch.
enum class Mode { Automatic, On, Off };
Mode mode(QSettings &s);
void setMode(QSettings &s, Mode m);
QStringList modeLabels();             // in the order of Mode
bool wantedBy(Mode m);                // Automatic: on for a pad, off on a computer

// Switched once at start-up, before the main window is built: the window's
// bars ask for their sizes while they are made.
void setEnabled(bool on);
bool enabled();

constexpr int kTarget = 48;           // smallest finger target, px (= dp on a pad)
constexpr int kRow = 44;              // menu, list and tab rows
constexpr int kField = 40;            // style sheet min-height of a field (44+ with its frame)

// The icon size a bar or tab uses: the desktop size, or a third larger.
QSize iconSize(int desktopPx);
// A button's smallest side: the desktop size, or the touch target.
int minSide(int desktopPx);
// A field's "min-height" in a style sheet (the text area, without its frame):
// the desktop value, or one that makes the field a finger high.
int minFieldHeight(int desktopPx);

// The profile as a style around a base style (scroll bars, indicators, tabs,
// spin box buttons, row heights), and as style sheet rules for what the
// application's own style sheet sizes (menus, tool buttons). Off, wrap()
// gives back base and styleSheet() is empty. Wrap the style before a style
// sheet is set: Qt puts the style sheet around the application's style.
QStyle *wrap(QStyle *base);
QString styleSheet();

// Dialogs never bigger than the screen they open on: a size asked for in
// code is cut to the screen, and a dialog whose contents still do not fit
// gets them in a scroll area, with its OK / Cancel row left outside it. It
// only acts when a dialog would not fit, touch or not.
void fitToScreen(QDialog *dlg);
// The application-wide event filter: fitToScreen for every dialog, and with
// the profile on, room in every spin box for its finger-sized buttons.
void install(QApplication &app);

} // namespace touch
} // namespace c2d
