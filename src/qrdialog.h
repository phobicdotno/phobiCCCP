#pragma once

class QWidget;

namespace c2d {

class Canvas;

// Create > QR code: asks for the text and the size and draws the code on the
// centre of the stock as closed vectors, one undo step. No toolpaths are
// added. Returns true when the code was drawn.
bool runQrDialog(QWidget *parent, Canvas *canvas);

} // namespace c2d
