#pragma once
#include "model3d.h"

#include <QStringList>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QTimer;
class QToolButton;

namespace c2d {

class Document;
class ModelJob;
class ModelView;

// The "Model" tab: Carbide Create Pro's 3D modelling. A list of relief
// components (from vectors, image heightmaps, STL meshes, textures) with a
// property editor, the composite rendered as a shaded top-down relief, and a
// worker-thread rebuild. Owns the document's ModelStore, so heightModelFor()
// hands the 3D toolpaths this panel's composite.
class ModelPanel : public QWidget
{
    Q_OBJECT
public:
    explicit ModelPanel(QWidget *parent = nullptr);
    ~ModelPanel() override;

    // (Re)load the model stored in doc->filePath() and attach the store.
    void setDocument(Document *doc);
    // Persist the components (and their embedded image/STL bytes) into a .c2d.
    bool saveTo(const QString &c2dPath, QString *error = nullptr) const;
    // Current canvas selection — "From vectors" / "Texture" use it as the region.
    void setSelection(const QStringList &ids);
    // Vectors were edited: the composite is stale; rebuilt when the tab shows.
    void documentChanged();
    // Synchronous rebuild + view refresh (used by --shot … model).
    void rebuildBlocking();

    ModelStore *store() { return &m_store; }
    const Model3D &model() const { return m_store.model; }

signals:
    void modelChanged();    // components edited: the document is dirty
    void modelRebuilt();    // the composite changed (3D toolpaths may re-plan)

protected:
    void showEvent(QShowEvent *e) override;

private:
    void addFromVectors(ModelComponent::Shape shape);
    void addImage();
    void addTexture();
    void addStl();
    void appendComponent(ModelComponent c);
    void removeSelected();
    void moveSelected(int delta);

    void refreshList();
    void refreshProps();
    void applyProps();          // widgets -> selected component
    int selectedRow() const;
    ModelComponent *selected();

    void touch();               // model edited: dirty + rebuild
    void scheduleRebuild();
    void startRebuild();
    void cancelRebuild();
    void takeResult(const HeightModel &hm);
    void updateStatus();
    void setBusy(bool on);
    QString uniqueName(const QString &base) const;

    Document *m_doc = nullptr;
    ModelStore m_store;
    QStringList m_selection;
    bool m_stale = false;        // composite out of date while the tab was hidden
    bool m_pending = false;      // a rebuild was requested while one was running
    bool m_updating = false;     // populating the widgets: ignore their signals

    ModelJob *m_job = nullptr;
    QTimer *m_debounce;

    // list + list actions
    QTableWidget *m_list;
    QToolButton *m_addVec;
    QPushButton *m_addImg, *m_addTex, *m_addStl, *m_remove, *m_up, *m_down;

    // properties
    QGroupBox *m_propBox;
    QLineEdit *m_name;
    QComboBox *m_shape, *m_combine, *m_units;
    QDoubleSpinBox *m_height, *m_base, *m_angle, *m_blur, *m_x, *m_y, *m_width, *m_tile;
    QCheckBox *m_invert;
    QLabel *m_source;
    QVector<QWidget *> m_propRows;   // label+field pairs toggled per kind

    // build + view
    QComboBox *m_res;
    QPushButton *m_rebuild, *m_cancel;
    QProgressBar *m_progress;
    ModelView *m_view;
    QLabel *m_readout, *m_status;
    QImage m_image;
};

} // namespace c2d
