#pragma once
#include "c2ddocument.h"
#include "canvas.h"
#include <QMainWindow>

class QPlainTextEdit;

namespace c2d {

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    void openFile(const QString &path);

private slots:
    void onOpen();
    void onSave();
    void onSaveAs();
    void onElementMoved(int index, QPointF delta);
    void onScaleSelected();
    void onAddCircle();
    void onAddRectangle();
    void onDeleteSelected();

private:
    void refreshInfo();
    void setDirty(bool dirty);
    bool requireSelection(QList<int> *indices);

    Canvas *m_canvas;
    QPlainTextEdit *m_info;   // params + item summary sidebar
    Document m_doc;
    bool m_dirty = false;
};

} // namespace c2d
