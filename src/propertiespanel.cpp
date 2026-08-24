#include "propertiespanel.h"
#include "canvas.h"
#include "c2ddocument.h"

#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QSpinBox>

namespace c2d {

QDoubleSpinBox *PropertiesPanel::spin(double max)
{
    auto *s = new QDoubleSpinBox(this);
    s->setRange(-max, max);
    s->setDecimals(3);
    s->setSuffix(QStringLiteral(" mm"));
    s->setKeyboardTracking(false);
    connect(s, &QDoubleSpinBox::valueChanged, this, [this] {
        if (!m_loading)
            apply();
    });
    return s;
}

PropertiesPanel::PropertiesPanel(Canvas *canvas, QWidget *parent)
    : QWidget(parent), m_canvas(canvas)
{
    auto *form = new QFormLayout(this);
    form->setContentsMargins(10, 10, 10, 10);
    form->setSpacing(6);

    m_typeLabel = new QLabel(QStringLiteral("—"), this);
    m_typeLabel->setStyleSheet(QStringLiteral("font-weight: bold;"));
    form->addRow(m_typeLabel);

    m_x = spin(10000);
    m_y = spin(10000);
    auto *posRow = new QWidget(this);
    auto *posLay = new QHBoxLayout(posRow);
    posLay->setContentsMargins(0, 0, 0, 0);
    posLay->addWidget(m_x);
    posLay->addWidget(m_y);
    form->addRow(QStringLiteral("Center"), posRow);

    m_radius = spin(10000);
    m_radiusRow = m_radius;
    form->addRow(QStringLiteral("Radius"), m_radius);

    m_width = spin(10000);
    m_height = spin(10000);
    m_sizeRow = new QWidget(this);
    auto *sizeLay = new QHBoxLayout(m_sizeRow);
    sizeLay->setContentsMargins(0, 0, 0, 0);
    sizeLay->addWidget(m_width);
    sizeLay->addWidget(m_height);
    form->addRow(QStringLiteral("W × H"), m_sizeRow);

    m_sides = new QSpinBox(this);
    m_sides->setRange(3, 64);
    m_sides->setKeyboardTracking(false);
    connect(m_sides, &QSpinBox::valueChanged, this, [this] {
        if (!m_loading)
            apply();
    });
    m_sidesRow = m_sides;
    form->addRow(QStringLiteral("Sides"), m_sides);

    setSelection({});
}

void PropertiesPanel::setDocument(Document *doc)
{
    m_doc = doc;
    setSelection({});
}

void PropertiesPanel::setSelection(const QStringList &ids)
{
    m_id = (ids.size() == 1) ? ids.first() : QString();
    refresh();
}

void PropertiesPanel::refresh()
{
    m_loading = true;
    Element *e = (m_doc && !m_id.isEmpty()) ? m_doc->elementById(m_id) : nullptr;
    const bool have = e != nullptr;
    setEnabled(have);
    m_radiusRow->setVisible(false);
    m_sizeRow->setVisible(false);
    m_sidesRow->setVisible(false);

    if (!have) {
        m_typeLabel->setText(QStringLiteral("no selection"));
        m_x->setValue(0);
        m_y->setValue(0);
        m_loading = false;
        return;
    }

    m_typeLabel->setText(e->geometryType);
    const QJsonArray c = e->raw.value("center").toArray();
    if (c.size() == 2) {
        m_x->setValue(c.at(0).toDouble());
        m_y->setValue(c.at(1).toDouble());
    } else {
        // path/text: expose the bounding-box center; edits become a move.
        const QPointF bc = e->painterPath.boundingRect().center();
        m_x->setValue(bc.x());
        m_y->setValue(bc.y());
    }

    if (e->geometryType == QLatin1String("circle")) {
        m_radiusRow->setVisible(true);
        m_radius->setValue(e->raw.value("radius").toDouble());
    } else if (e->geometryType == QLatin1String("rectangle")) {
        m_sizeRow->setVisible(true);
        m_width->setValue(e->raw.value("width").toDouble());
        m_height->setValue(e->raw.value("height").toDouble());
    } else if (e->geometryType == QLatin1String("regular_polygon")) {
        m_radiusRow->setVisible(true);
        m_sidesRow->setVisible(true);
        m_radius->setValue(e->raw.value("radius").toDouble());
        m_sides->setValue(int(e->raw.value("num_sides").toDouble()));
    }
    m_loading = false;
}

void PropertiesPanel::apply()
{
    if (!m_doc || m_id.isEmpty())
        return;
    Element *e = m_doc->elementById(m_id);
    if (!e)
        return;

    if (e->raw.contains(QLatin1String("center"))) {
        QHash<QString, double> p;
        p.insert(QStringLiteral("cx"), m_x->value());
        p.insert(QStringLiteral("cy"), m_y->value());
        p.insert(QStringLiteral("radius"), m_radius->value());
        p.insert(QStringLiteral("width"), m_width->value());
        p.insert(QStringLiteral("height"), m_height->value());
        p.insert(QStringLiteral("num_sides"), m_sides->value());
        m_canvas->editElement(m_id, p);
    } else {
        const QPointF bc = e->painterPath.boundingRect().center();
        m_canvas->moveElementBy(m_id, m_x->value() - bc.x(), m_y->value() - bc.y());
    }
}

} // namespace c2d
