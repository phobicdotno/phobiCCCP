#pragma once
#include "c2ddocument.h"
#include "canvas.h"
#include <QMainWindow>

class QAction;
class QLabel;
class QPlainTextEdit;

namespace c2d {

class MachinePanel;
class PropertiesPanel;
class ToolpathPanel;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    void openFile(const QString &path);
    void showToolpathPreview();   // enable the overlay (used by --shot)

private slots:
    void onOpen();
    void onSave();
    void onSaveAs();
    void onExportGcode();
    void refreshPreview();

private:
    void refreshInfo();

    void updateTitle();

    Canvas *m_canvas;
    QPlainTextEdit *m_info;      // params + item summary sidebar
    QLabel *m_cursorLabel;       // live mm position, right side of status bar
    QLabel *m_zoomLabel;         // zoom percentage
    PropertiesPanel *m_props;    // numeric editor for the selection
    ToolpathPanel *m_tp;         // toolpath list + parameter editor
    MachinePanel *m_machine;     // GRBL serial control + streaming
    QAction *m_previewAct;       // toolpath overlay toggle
    bool m_dirty = false;
    Document m_doc;
};

} // namespace c2d
