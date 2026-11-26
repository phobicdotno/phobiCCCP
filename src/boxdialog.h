#pragma once
#include "boxgen.h"

class QWidget;

namespace c2d {

class Canvas;

// Create > Box with Finger Joints: asks for the box and draws its parts on
// the stock as closed vectors, one undo step. No toolpaths are added.
// `stockThickness` and `stockWidth` (mm, 0 = unknown) seed the form.
// Returns true when parts were drawn.
bool runBoxDialog(QWidget *parent, Canvas *canvas, double stockThickness, double stockWidth);

} // namespace c2d
