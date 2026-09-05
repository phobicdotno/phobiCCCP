#pragma once
#include <QWidget>

class QListWidget;
class QTableWidget;

namespace c2d {

class Canvas;
class Document;

// Toolpath browser + parameter editor. Lists the document's toolpaths; the
// table flattens the selected toolpath's scalar parameters (top level plus
// speeds.* and tool.*) into editable key/value rows. Edits keep each value's
// original JSON type (CC stores depths as strings, rates as numbers) and go
// through Canvas::editToolpath so they are undoable and saved back in place.
class ToolpathPanel : public QWidget
{
    Q_OBJECT
public:
    explicit ToolpathPanel(Canvas *canvas, QWidget *parent = nullptr);
    void setDocument(Document *doc);

public slots:
    void refresh();               // re-read list + fields from the document

private:
    void showToolpath(int row);
    void onCellEdited(int row, int col);
    void createInlayMale();       // mirrored male from the selected v-carve
    void pickTool();              // tool library -> selected toolpath's tool/speeds

    Canvas *m_canvas;
    Document *m_doc = nullptr;
    bool m_loading = false;

    QListWidget *m_list;
    QTableWidget *m_table;
    QString m_uuid;               // toolpath shown in the table
};

} // namespace c2d
