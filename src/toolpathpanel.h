#pragma once
#include "c2ddocument.h"
#include <QTreeWidget>

// Dock content: one top-level row per toolpath ("name  [type]"), children are
// its editable parameters. Scalars (bool/number/string) edit in place and are
// written back into Toolpath::json with their original JSON type preserved -
// depths stay strings (their sign convention is build-dependent, see the
// format notes), cut_depth stays a number. Nested objects (tool, speeds,
// *_pocket, finish_speeds) recurse; arrays and structural keys are skipped.
namespace c2d {

class ToolpathPanel : public QTreeWidget
{
    Q_OBJECT
public:
    explicit ToolpathPanel(QWidget *parent = nullptr);
    void setDocument(Document *doc);   // non-const: edits mutate Toolpath::json
    void reload();

signals:
    void aboutToEdit();   // a valid change is about to be applied (undo hook)
    void edited();        // a parameter was changed (and applied to the document)

private slots:
    void onItemChanged(QTreeWidgetItem *item, int column);

private:
    void addObjectRows(QTreeWidgetItem *parent, const QJsonObject &obj,
                       int tpIndex, const QStringList &prefix);

    Document *m_doc = nullptr;
    bool m_loading = false;
};

} // namespace c2d
