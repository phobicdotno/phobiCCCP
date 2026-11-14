#pragma once

class QApplication;
class QDialog;

namespace c2d {

// Gives every popup of the application the look of the Machine page's own
// questions (shopdialog.cpp): no window-manager title bar and no X, an amber
// edge, the title as a small caption inside, big buttons.
//
// Done from one event filter rather than at every call site, so stock message
// boxes, input boxes, progress boxes and the application's own dialogs all
// follow, and a popup added later does too. File dialogs keep their title
// bar: they are windows to move and resize. A popup with no button at all
// gets a big X in its top right corner, so there is always a way to close it.
void installPopupLook(QApplication &app);

// What the filter does to one dialog, before it is first shown. Idempotent.
void applyPopupLook(QDialog *dlg);

} // namespace c2d
