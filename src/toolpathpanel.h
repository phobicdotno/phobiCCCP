#pragma once
#include <QWidget>

class QLabel;
class QTableWidget;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace c2d {

class Canvas;
class Document;

// Toolpath browser + parameter editor. The list shows the document's
// toolpaths in machining order with an enable checkbox per row (double-click
// the name to rename); New ▾ / Duplicate / Delete / Move up / Move down
// manage the lifecycle, all undoable. The table flattens the selected
// toolpath's scalar parameters (top level plus speeds.* and tool.*) into
// editable key/value rows. Edits keep each value's original JSON type (CC
// stores depths as strings, rates as numbers) and go through
// Canvas::editToolpath so they are undoable and saved back in place.
class ToolpathPanel : public QWidget
{
    Q_OBJECT
public:
    explicit ToolpathPanel(Canvas *canvas, QWidget *parent = nullptr);
    void setDocument(Document *doc);
    QString currentUuid() const { return m_uuid; }

public slots:
    void refresh();               // re-read list + fields from the document
    void newToolpath(const QString &type);   // create of CC type / engrave_toolpath
    void duplicateCurrent();
    void deleteCurrent();
    void renameCurrent();
    void moveCurrent(int delta);  // -1 up, +1 down (machining order)

private:
    void showToolpath(const QString &uuid);
    void onCurrentChanged();
    void onItemChanged(QTreeWidgetItem *item, int column);
    void onCellEdited(int row, int col);
    void createInlayMale();       // mirrored male from the selected v-carve
    void pickTool();              // tool library -> selected toolpath's tool/speeds
    void updateButtons();

    Canvas *m_canvas;
    Document *m_doc = nullptr;
    bool m_loading = false;

    QTreeWidget *m_list;
    QTableWidget *m_table;
    QLabel *m_hint;
    QToolButton *m_dupBtn, *m_delBtn, *m_upBtn, *m_downBtn;
    QString m_uuid;               // toolpath shown in the table
};

} // namespace c2d
