#pragma once
#include <QString>
#include <functional>

class QWidget;

// GUI glue for the SVG/DXF importers: file dialogs, the undoable "import"
// command on the canvas' undo stack, and drag-and-drop onto the main window.
namespace c2d {

class Canvas;
class Document;

// Import `path` (or ask for one with a file dialog when empty; `filter` is the
// dialog's name filter) into `doc`, undoable via the canvas' undo stack.
// Reports the outcome in the status bar of `parent` (when it is a QMainWindow)
// and in a message box when something was skipped or failed.
void importVectorFile(QWidget *parent, Canvas *canvas, Document *doc,
                      const QString &path = QString(), const QString &filter = QString());

// Accept .svg/.dxf drops on `target` (imported) and .c2d drops (handed to
// `openC2d`). Also filters the canvas so the drop lands regardless of where
// on the window it happens.
void installImportDropHandler(QWidget *target, Canvas *canvas, Document *doc,
                              std::function<void(const QString &)> openC2d);

} // namespace c2d
