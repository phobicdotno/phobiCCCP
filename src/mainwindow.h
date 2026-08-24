#pragma once
#include "c2ddocument.h"
#include "canvas.h"
#include <QMainWindow>

class QLabel;
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

private:
    void refreshInfo();

    Canvas *m_canvas;
    QPlainTextEdit *m_info;   // params + item summary sidebar
    QLabel *m_cursorLabel;    // live mm position, right side of status bar
    Document m_doc;
};

} // namespace c2d
