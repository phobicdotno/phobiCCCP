#pragma once
#include <QDialog>
#include <QSizeF>
#include <QString>

class QComboBox;
class QDoubleSpinBox;
class QSettings;

namespace c2d {

// Application settings kept in QSettings, read by main() before Qt is up (the
// GUI scale has to be in the environment before QApplication is constructed)
// and by the main window.
namespace appsettings {
// GUI scale in percent: 100, 125, 150, 175 or 200. Anything else reads as 100.
int uiScalePercent(QSettings &s);
QList<int> uiScaleChoices();
// Text size of the Toolpaths panel in percent of the app's text (125 when unset).
int panelTextPercent(QSettings &s);
QList<int> panelTextChoices();

// The machine a blank design is sized to: every Carbide 3D Shapeoko, plus
// "custom" with its size in machine/customWidth and machine/customHeight.
struct Machine {
    QString key;     // "s5pro-4x4", ... — stored under machine/size
    QString label;
    double width;    // X, mm
    double height;   // Y, mm
};
QList<Machine> machines();               // the fixed list, without custom
Machine machine(QSettings &s);           // Shapeoko 5 Pro 4x4 when nothing is set
constexpr double kBlankThickness = 19.0; // mm, for the startup design
} // namespace appsettings

class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget *parent = nullptr);

    // Writes the choices to QSettings. True when the GUI scale changed, which
    // only takes effect after a restart.
    bool save();

private:
    QComboBox *m_scale;
    QComboBox *m_panelText;
    QComboBox *m_machine;
    QDoubleSpinBox *m_customW;
    QDoubleSpinBox *m_customH;
    int m_scaleWas = 100;
};

} // namespace c2d
