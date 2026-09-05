#pragma once
#include "toollibrary.h"
#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;

namespace c2d {

// Tool library picker: searchable cutter list, material selector and a
// preview of the numbers that will be written into the toolpath. Returns the
// chosen LibraryTool + ToolFeeds through selectedTool()/selectedFeeds().
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
    void updatePreview();

    const ToolLibrary &m_lib;
    QLineEdit *m_search;
    QComboBox *m_material;
    QListWidget *m_list;
    QLabel *m_preview;
};

} // namespace c2d
