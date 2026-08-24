#pragma once
#include <QWidget>

class QDoubleSpinBox;
class QLabel;
class QSpinBox;

namespace c2d {

class Canvas;
class Document;

// Numeric editor for the selected element: position for everything, plus
// radius (circle/polygon), width/height (rectangle) and sides (polygon).
// Edits go through Canvas::editElement / moveElementBy so they are undoable.
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
    QDoubleSpinBox *spin(double max);

    Canvas *m_canvas;
    Document *m_doc = nullptr;
    QString m_id;                 // single selected element ("" = none/multi)
    bool m_loading = false;       // guard: setValue must not trigger apply

    QLabel *m_typeLabel;
    QDoubleSpinBox *m_x, *m_y, *m_radius, *m_width, *m_height, *m_rotation;
    QSpinBox *m_sides;
    QWidget *m_radiusRow, *m_sizeRow, *m_sidesRow, *m_rotationRow;
};

} // namespace c2d
