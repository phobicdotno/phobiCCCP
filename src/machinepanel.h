#pragma once
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

namespace c2d {

class Document;
class GrblStreamer;

// Machine control dock: connect to a GRBL controller over serial, jog, zero,
// and stream the document's generated g-code — a Carbide-Motion replacement.
class MachinePanel : public QWidget
{
    Q_OBJECT
public:
    explicit MachinePanel(QWidget *parent = nullptr);
    void setDocument(Document *doc) { m_doc = doc; }

private:
    void refreshPorts();
    void toggleConnect();
    void jog(double dx, double dy, double dz);
    void runProgram();

    Document *m_doc = nullptr;
    GrblStreamer *m_grbl;

    QComboBox *m_ports;
    QComboBox *m_step;
    QCheckBox *m_airCut;         // rehearse: spindle stripped, Z lifted
    QDoubleSpinBox *m_airLift;   // air-cut Z lift (mm)
    QPushButton *m_connectBtn;
    QPushButton *m_runBtn, *m_pauseBtn, *m_stopBtn;
    QLabel *m_status;
    QProgressBar *m_progress;
    QPlainTextEdit *m_console;
};

} // namespace c2d
