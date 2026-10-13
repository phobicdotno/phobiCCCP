#pragma once
#include "toollibrary.h"
#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QToolButton;

namespace c2d { class ToolDiagram; }

namespace c2d {

// Tool library picker: searchable cutter list, material selector and a
// preview of the numbers that will be written into the toolpath. Returns the
// chosen LibraryTool + ToolFeeds through selectedTool()/selectedFeeds().
//
// A row of bit icons along the top (end mill, ball, V-bit, ...) sorts the
// list: press one or more to show only those kinds, in the order they were
// pressed; none pressed shows every bit, grouped by kind. Kept across
// restarts.
class ToolLibraryDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ToolLibraryDialog(const ToolLibrary &lib, const QString &materialId,
                               int currentToolNumber, QWidget *parent = nullptr);

    const LibraryTool *selectedTool() const;
    QString selectedMaterialId() const;
    ToolFeeds selectedFeeds() const;   // invalid() when the tool has none

private:
    void rebuildList();
    void kindToggled(const QString &kind, bool on);
    void updatePreview();

    const ToolLibrary &m_lib;
    QLineEdit *m_search;
    QComboBox *m_material;
    QListWidget *m_list;
    QLabel *m_preview;
    ToolDiagram *m_diagram;
    QList<QToolButton *> m_kindButtons;
    QStringList m_kindOrder;           // every kind, in the row's order
    QStringList m_marked;              // pressed kinds, in the order pressed
};

} // namespace c2d
