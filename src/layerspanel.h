#pragma once
#include <QJsonObject>
#include <QVector>
#include <QWidget>

class QListWidget;
class QPushButton;

namespace c2d {

class Canvas;

// Design mode: the drawing's layers (Carbide Create's, kept in the .c2d).
// Each row has Show / Hide and Lock / Unlock; a tap on a row selects its
// shapes. Buttons below: add a layer, rename, delete an empty one, and move
// the selected shapes onto the picked layer. Every change is one undo step.
class LayersPanel : public QWidget
{
    Q_OBJECT
public:
    explicit LayersPanel(Canvas *canvas, QWidget *parent = nullptr);

    // The key (layers::Layer::key) of the picked row, or empty.
    QString currentKey() const;
    void setCurrentKey(const QString &key);

    // The panel's actions, by layer key (also what the buttons do).
    void addLayer(const QString &name);
    void renameLayer(const QString &key, const QString &name);
    void setVisible(const QString &key, bool on);
    void setLocked(const QString &key, bool on);
    bool deleteLayer(const QString &key);       // only an empty one
    void moveSelectionTo(const QString &key);
    void selectLayer(const QString &key);

public slots:
    void reload();
    // A new or reopened design: layers added here with nothing on them go.
    void documentReplaced();

private:
    void buildRow(int row);
    void updateButtons();
    QJsonObject layerObject(const QString &key) const;

    Canvas *m_canvas;
    QListWidget *m_list;
    QPushButton *m_add, *m_rename, *m_delete, *m_move;
    QVector<QJsonObject> m_extra;   // added here, nothing on them yet
    bool m_busy = false;
};

} // namespace c2d
