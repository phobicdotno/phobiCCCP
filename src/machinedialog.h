#pragma once
#include "grblsender.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSerialPort;

// Streams a generated g-code program to a GRBL controller over USB serial
// (115200 8N1, the GRBL default). Pick the port, Connect, Start; one line is
// in flight at a time and progress tracks GRBL's ok responses. Abort sends a
// soft-reset (0x18) so the controller stops immediately.
namespace c2d {

class MachineDialog : public QDialog
{
    Q_OBJECT
public:
    explicit MachineDialog(const QString &gcode, QWidget *parent = nullptr);

private slots:
    void refreshPorts();
    void onConnect();
    void onStart();
    void onAbort();
    void onReadyRead();

private:
    void log(const QString &line);
    void updateUi();

    QString m_gcode;
    QComboBox *m_ports;
    QPushButton *m_connectBtn;
    QPushButton *m_startBtn;
    QPushButton *m_abortBtn;
    QProgressBar *m_progress;
    QLabel *m_status;
    QPlainTextEdit *m_console;
    QSerialPort *m_port = nullptr;
    GrblSender m_sender;
};

} // namespace c2d
