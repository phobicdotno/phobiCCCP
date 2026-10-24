#pragma once
#include <QIcon>
#include <QPainterPath>
#include <QSet>
#include <QWidget>
#include <functional>

class QLabel;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace c2d {

class Canvas;
class Document;
class ToolpathForm;
struct Toolpath;

// Toolpath browser + parameter editor. The list shows the document's
// toolpaths in machining order with an enable checkbox per row (double-click
// the name to rename); New ▾ / Duplicate / Delete / Move up / Move down
// manage the lifecycle, all undoable. Below the list, ToolpathForm shows the
// selected toolpath's settings in grouped cards; its edits go through
// Canvas::editToolpath so they are undoable and saved back in place.
class ToolpathPanel : public QWidget
{
    Q_OBJECT
public:
    explicit ToolpathPanel(Canvas *canvas, QWidget *parent = nullptr);
    void setDocument(Document *doc);
    QString currentUuid() const { return m_uuid; }
    void applyTextSize();         // File > Settings > Toolpaths text size
    // The toolpath-type glyphs of the Toolpaths bar, for the list's cards.
    void setTypeIcons(std::function<QIcon(const QString &)> f);

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
    void createInlayMale();       // mirrored male from the selected v-carve
    // A number typed in the form (no Enter yet) is committed, and its queued
    // edit run, before an action reads the toolpath: run after it, the edit's
    // snapshot put back what the action changed.
    void flushForm();
    void pickTool();              // tool library -> selected toolpath's tool/speeds
    void updateButtons();
    // "which shapes does this toolpath cut?" and its reverse. The first paints
    // an amber halo on the canvas and describes the vectors in the list's
    // tooltip; the second bolds every row that machines the current canvas
    // selection, so a shape can be traced back to its toolpaths.
    QStringList vectorIdsOf(const QString &uuid) const;
    void updateHighlight();           // halo follows the list's selection
    QString describeVectors(const QStringList &ids) const;
    void markUsage(const QStringList &selectedElementIds);
    // Each card's "v" opens a thumbnail per shape it cuts, each with a bin
    // that takes the shape out of the toolpath. Where toolpaths sit on top of
    // each other, the thumbnails and the canvas halo show only what is left
    // for this one to cut after the toolpaths above (see clearedAbove).
    void toggleShapes(const QString &uuid);
    void removeShape(const QString &uuid, const QString &elementId);
    void setThumbnails(QTreeWidgetItem *it, const Toolpath &t);
    QPainterPath regionOf(const Toolpath &t) const;
    QPainterPath clearedAbove(const QString &uuid, bool *applies) const;
    void updateListHeight();
    QSet<QString> m_expanded;         // toolpaths whose shapes are shown

    QStringList m_selectedElements;   // last canvas selection, for markUsage

    Canvas *m_canvas;
    Document *m_doc = nullptr;
    bool m_loading = false;

    QTreeWidget *m_list;
    ToolpathForm *m_form;
    QLabel *m_hint;
    QToolButton *m_dupBtn, *m_delBtn, *m_upBtn, *m_downBtn;
    QString m_uuid;               // toolpath shown in the settings
    std::function<QIcon(const QString &)> m_typeIcon;
};

} // namespace c2d
