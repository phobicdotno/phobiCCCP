#pragma once

class QApplication;

namespace c2d {

// The application's look: Fusion with a dark palette, a small style sheet for
// spacing and accents, and thicker scroll bars and splitters than Fusion's.
// When the touch profile is on (touchprofile.h: switch it before this call)
// its finger sizes go on top. In the UI library, so the tests see the look
// people get.
void applyAppTheme(QApplication &app);

} // namespace c2d
