#pragma once
#include <QWidget>

class QCheckBox;
class QDoubleSpinBox;
class QFontComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace c2d {

class Canvas;
class Document;

// Numeric editor for the selected element: position for everything, plus
// radius (circle/polygon), width/height (rectangle) and sides (polygon).
// Text elements get their string, font (family / height / bold / italic) and
// the text-on-arc settings (arc on/off, radius, angle offset, on bottom).
// Edits go through Canvas::editElement / moveElementBy / editText so they
// are undoable.
class PropertiesPanel : public QWidget
{
    Q_OBJECT
public:
    explicit PropertiesPanel(Canvas *canvas, QWidget *parent = nullptr);
    void setDocument(Document *doc);

public slots:
    void setSelection(const QStringList &ids);
    void refresh();               // re-read fields from the document

private:
    void apply();
    void applyText();
    QDoubleSpinBox *spin(double max);

    Canvas *m_canvas;
    Document *m_doc = nullptr;
    QString m_id;                 // single selected element ("" = none/multi)
    QString m_familyShown;        // combo family after refresh (may be a substitute)
    bool m_loading = false;       // guard: setValue must not trigger apply

    QLabel *m_typeLabel;
    QDoubleSpinBox *m_x, *m_y, *m_radius, *m_width, *m_height, *m_rotation;
    QSpinBox *m_sides;
    QWidget *m_radiusRow, *m_sizeRow, *m_sidesRow, *m_rotationRow;

    // Text rows.
    QLineEdit *m_text;
    QFontComboBox *m_family;
    QDoubleSpinBox *m_fontHeight, *m_arcRadius, *m_arcAngle;
    QCheckBox *m_bold, *m_italic, *m_arcOn, *m_arcBottom;
    QPushButton *m_convert;
    QVector<QWidget *> m_textRows, m_arcRows;
    QVector<QWidget *> m_textLabels, m_arcLabels;
};

} // namespace c2d
