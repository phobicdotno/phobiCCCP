#include "backgrounddialog.h"
#include "canvas.h"
#include "c2ddocument.h"
#include "tracedialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

namespace c2d {

static QString imageFilter()
{
    QStringList exts;
    for (const QByteArray &f : QImageReader::supportedImageFormats())
        exts << QStringLiteral("*.") + QString::fromLatin1(f);
    return QStringLiteral("Images (%1);;All files (*)").arg(exts.join(QChar(' ')));
}

// ---- Adjust dialog ---------------------------------------------------------

BackgroundAdjustDialog::BackgroundAdjustDialog(BackgroundImage *bg, Canvas *canvas, QWidget *parent)
    : QDialog(parent), m_bg(bg), m_backup(*bg), m_canvas(canvas)
{
    setWindowTitle(tr("Adjust background image"));
    auto *form = new QFormLayout;

    auto mm = [this](double lo, double hi) {
        auto *s = new QDoubleSpinBox(this);
        s->setRange(lo, hi);
        s->setDecimals(2);
        s->setSuffix(tr(" mm"));
        return s;
    };
    m_x = mm(-10000, 10000);
    m_y = mm(-10000, 10000);
    m_width = mm(0.1, 10000);
    m_rotation = new QDoubleSpinBox(this);
    m_rotation->setRange(-360, 360);
    m_rotation->setDecimals(1);
    m_rotation->setSuffix(tr("°"));
    m_opacity = new QSlider(Qt::Horizontal, this);
    m_opacity->setRange(0, 100);
    auto *opLabel = new QLabel(this);
    auto *opRow = new QHBoxLayout;
    opRow->addWidget(m_opacity, 1);
    opRow->addWidget(opLabel);
    m_visible = new QCheckBox(tr("Visible"), this);
    m_locked = new QCheckBox(tr("Lock position and size"), this);

    form->addRow(tr("X (left edge)"), m_x);
    form->addRow(tr("Y (bottom edge)"), m_y);
    form->addRow(tr("Width (keeps aspect)"), m_width);
    form->addRow(tr("Rotation"), m_rotation);
    form->addRow(tr("Opacity"), opRow);
    form->addRow(QString(), m_visible);
    form->addRow(QString(), m_locked);

    auto *info = new QLabel(tr("%1 × %2 px").arg(bg->image.width()).arg(bg->image.height()), this);
    form->addRow(tr("Image"), info);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *lay = new QVBoxLayout(this);
    lay->addLayout(form);
    lay->addWidget(buttons);

    m_updating = true;
    m_x->setValue(bg->x);
    m_y->setValue(bg->y);
    m_width->setValue(bg->widthMm());
    m_rotation->setValue(bg->rotationDeg);
    m_opacity->setValue(qRound(bg->opacity * 100));
    opLabel->setText(QStringLiteral("%1%").arg(m_opacity->value()));
    m_visible->setChecked(bg->visible);
    m_locked->setChecked(bg->locked);
    m_updating = false;
    syncLock();

    connect(m_x, &QDoubleSpinBox::valueChanged, this, &BackgroundAdjustDialog::apply);
    connect(m_y, &QDoubleSpinBox::valueChanged, this, &BackgroundAdjustDialog::apply);
    connect(m_width, &QDoubleSpinBox::valueChanged, this, &BackgroundAdjustDialog::apply);
    connect(m_rotation, &QDoubleSpinBox::valueChanged, this, &BackgroundAdjustDialog::apply);
    connect(m_opacity, &QSlider::valueChanged, this, [this, opLabel](int v) {
        opLabel->setText(QStringLiteral("%1%").arg(v));
        apply();
    });
    connect(m_visible, &QCheckBox::toggled, this, &BackgroundAdjustDialog::apply);
    connect(m_locked, &QCheckBox::toggled, this, [this] { syncLock(); apply(); });

    // Cancel restores the state the dialog opened with.
    connect(this, &QDialog::rejected, this, [this] {
        *m_bg = m_backup;
        if (m_canvas)
            m_canvas->viewport()->update();
    });
}

void BackgroundAdjustDialog::syncLock()
{
    const bool locked = m_locked->isChecked();
    m_x->setEnabled(!locked);
    m_y->setEnabled(!locked);
    m_width->setEnabled(!locked);
    m_rotation->setEnabled(!locked);
}

void BackgroundAdjustDialog::apply()
{
    if (m_updating)
        return;
    m_bg->x = m_x->value();
    m_bg->y = m_y->value();
    m_bg->setWidthMm(m_width->value());
    m_bg->rotationDeg = m_rotation->value();
    m_bg->opacity = m_opacity->value() / 100.0;
    m_bg->visible = m_visible->isChecked();
    m_bg->locked = m_locked->isChecked();
    if (m_canvas)
        m_canvas->viewport()->update();
}

// ---- menu wiring ------------------------------------------------------------

void installImageMenus(QMenu *fileMenu, QWidget *window, Canvas *canvas,
                       Document *doc, BackgroundImage *bg)
{
    auto markDirty = [canvas] {
        canvas->viewport()->update();
        emit canvas->documentChanged();
    };
    auto needDoc = [window, doc](const QString &title) {
        if (doc->filePath().isEmpty()) {
            QMessageBox::information(window, title, QObject::tr("Open a .c2d file first."));
            return false;
        }
        return true;
    };

    QMenu *bgMenu = fileMenu->addMenu(QObject::tr("&Background image"));
    QAction *setAct = bgMenu->addAction(QObject::tr("&Set…"));
    QAction *clearAct = bgMenu->addAction(QObject::tr("&Clear"));
    QAction *adjustAct = bgMenu->addAction(QObject::tr("&Adjust…"));
    QAction *traceAct = fileMenu->addAction(QObject::tr("&Trace image…"));
    traceAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T));

    QObject::connect(bgMenu, &QMenu::aboutToShow, bgMenu, [bg, clearAct, adjustAct] {
        clearAct->setEnabled(!bg->isNull());
        adjustAct->setEnabled(!bg->isNull());
    });

    QObject::connect(setAct, &QAction::triggered, window, [=] {
        if (!needDoc(QObject::tr("Background image")))
            return;
        const QString path = QFileDialog::getOpenFileName(
            window, QObject::tr("Set background image"), {}, imageFilter());
        if (path.isEmpty())
            return;
        QString err;
        if (!bg->setImageFile(path, &err)) {
            QMessageBox::warning(window, QObject::tr("Background image"), err);
            return;
        }
        // Default placement: natural 96-dpi size, shrunk to fit the board,
        // anchored at the origin.
        bg->x = bg->y = 0;
        bg->scale = 1.0;
        bg->rotationDeg = 0;
        const double bw = doc->boardWidth(), bh = doc->boardHeight();
        if (bw > 0 && bg->widthMm() > bw)
            bg->setWidthMm(bw);
        if (bh > 0 && bg->heightMm() > bh)
            bg->setWidthMm(bg->widthMm() * bh / bg->heightMm());
        bg->visible = true;
        markDirty();
        emit canvas->statusHint(QObject::tr("Background image set: %1 (%2 × %3 mm)")
                                    .arg(path).arg(bg->widthMm(), 0, 'f', 1)
                                    .arg(bg->heightMm(), 0, 'f', 1));
    });

    QObject::connect(clearAct, &QAction::triggered, window, [=] {
        if (bg->isNull())
            return;
        bg->clear();
        markDirty();
        emit canvas->statusHint(QObject::tr("Background image cleared"));
    });

    QObject::connect(adjustAct, &QAction::triggered, window, [=] {
        if (bg->isNull()) {
            QMessageBox::information(window, QObject::tr("Background image"),
                                     QObject::tr("No background image is set."));
            return;
        }
        BackgroundAdjustDialog dlg(bg, canvas, window);
        if (dlg.exec() == QDialog::Accepted)
            markDirty();
    });

    QObject::connect(traceAct, &QAction::triggered, window, [=] {
        if (!needDoc(QObject::tr("Trace image")))
            return;
        QImage img;
        double widthMm = 0;
        QPointF origin(0, 0);
        bool fromBackground = false;
        if (!bg->isNull()) {
            QMessageBox q(QMessageBox::Question, QObject::tr("Trace image"),
                          QObject::tr("Trace the current background image, or choose a file?"),
                          QMessageBox::NoButton, window);
            QPushButton *useBg = q.addButton(QObject::tr("Background image"), QMessageBox::AcceptRole);
            q.addButton(QObject::tr("Choose file…"), QMessageBox::ActionRole);
            q.addButton(QMessageBox::Cancel);
            q.exec();
            if (q.clickedButton() == static_cast<QAbstractButton *>(q.button(QMessageBox::Cancel)))
                return;
            fromBackground = (q.clickedButton() == useBg);
        }
        if (fromBackground) {
            img = bg->image;
            widthMm = bg->widthMm();
            origin = QPointF(bg->x, bg->y);
        } else {
            const QString path = QFileDialog::getOpenFileName(
                window, QObject::tr("Trace image"), {}, imageFilter());
            if (path.isEmpty())
                return;
            QImageReader reader(path);
            reader.setAutoTransform(true);
            img = reader.read();
            if (img.isNull()) {
                QMessageBox::warning(window, QObject::tr("Trace image"),
                                     QObject::tr("Could not read %1").arg(path));
                return;
            }
            widthMm = img.width() * BackgroundImage::kMmPerPixel;
            const double bw = doc->boardWidth();
            if (bw > 0 && widthMm > bw)
                widthMm = bw;
        }
        TraceDialog dlg(window);
        dlg.setImage(img, widthMm, origin);
        if (dlg.exec() != QDialog::Accepted)
            return;
        const QVector<Element> els = dlg.elements(doc->defaultLayer());
        if (els.isEmpty()) {
            emit canvas->statusHint(QObject::tr("Trace produced no outlines"));
            return;
        }
        TraceDialog::insertUndoable(canvas, doc, els);
        emit canvas->statusHint(QObject::tr("Inserted %1 traced path(s) — Ctrl+Z undoes")
                                    .arg(els.size()));
    });
}

} // namespace c2d
