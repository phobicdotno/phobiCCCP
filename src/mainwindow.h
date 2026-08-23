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

private:
    void refreshInfo();

    Canvas *m_canvas;
    QPlainTextEdit *m_info;   // params + item summary sidebar
    Document m_doc;
};

} // namespace c2d
