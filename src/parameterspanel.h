#pragma once
#include <QWidget>

class QLabel;
class QListWidget;
class QPushButton;
class QTableWidget;
class QTableWidgetItem;

namespace c2d {

class Canvas;
class Document;

// Fusion's Change Parameters, as a tab beside Document and Model: the user
// parameters (name, expression, value, comment) with the stock built-ins
// above them, the driving dimensions (d1, d2 ...) with their expressions,
// and the geometric constraints. A chip at the top says whether the sketch
// is fully constrained, how much freedom it has left, or what fails.
// Every change goes through Canvas::applyParametric, so it is one undo step
// and the drawing and the bound toolpath fields follow at once.
class ParametersPanel : public QWidget
{
    Q_OBJECT
public:
    explicit ParametersPanel(Canvas *canvas, QWidget *parent = nullptr);
    void setDocument(Document *doc);

public slots:
    void refresh();
    void showConstraint(const QString &uid);   // select its row

signals:
    void message(const QString &text);         // for the status bar

private:
    void addParameter();
    void removeParameter();
    void paramEdited(QTableWidgetItem *it);
    void dimensionEdited(QTableWidgetItem *it);
    bool rename(const QString &from, const QString &to);
    QString describe(int constraintIndex) const;

    Canvas *m_canvas;
    Document *m_doc = nullptr;
    bool m_updating = false;
    QLabel *m_chip, *m_counts;
    QTableWidget *m_params, *m_dims;
    QListWidget *m_cons;
    QPushButton *m_remove, *m_delCon;
};

} // namespace c2d
