#include "propertiespanel.h"
#include "canvas.h"
#include "c2ddocument.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QFormLayout>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
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
    // The form is scrolled rather than laid out directly on the panel:
    // selecting a text element reveals its font and arc rows, and a form that
    // owns the panel would grow the dock's minimum height by ~170 px. Stacked
    // with the other docks that pushed the window's minimum past the height of
    // a 1280x800 screen, so the window manager could no longer maximise it —
    // selecting text un-maximised the window and removed its maximise button.
    auto *inner = new QWidget(this);
    auto *form = new QFormLayout(inner);
    form->setContentsMargins(10, 10, 10, 10);
    form->setSpacing(6);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(inner);
    scroll->setMinimumHeight(70);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);

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

    m_rotation = spin(360);
    m_rotation->setSuffix(QStringLiteral(" °"));
    m_rotationRow = m_rotation;
    form->addRow(QStringLiteral("Rotation"), m_rotation);

    // ---- text ------------------------------------------------------------
    // Each row is added with a QLabel so both halves can be hidden together.
    auto textRow = [&](const QString &label, QWidget *w, QVector<QWidget *> &rows,
                       QVector<QWidget *> &labels) {
        auto *l = new QLabel(label, this);
        form->addRow(l, w);
        rows.append(w);
        labels.append(l);
    };
    auto textEdited = [this] {
        if (!m_loading)
            applyText();
    };

    m_text = new QLineEdit(this);
    connect(m_text, &QLineEdit::editingFinished, this, textEdited);
    textRow(QStringLiteral("Text"), m_text, m_textRows, m_textLabels);

    m_family = new QFontComboBox(this);
    connect(m_family, &QFontComboBox::currentFontChanged, this, [textEdited](const QFont &) { textEdited(); });
    textRow(QStringLiteral("Font"), m_family, m_textRows, m_textLabels);

    m_fontHeight = new QDoubleSpinBox(this);
    m_fontHeight->setRange(0.5, 1000);
    m_fontHeight->setDecimals(2);
    m_fontHeight->setSuffix(QStringLiteral(" mm"));
    m_fontHeight->setKeyboardTracking(false);
    connect(m_fontHeight, &QDoubleSpinBox::valueChanged, this, textEdited);
    textRow(QStringLiteral("Height"), m_fontHeight, m_textRows, m_textLabels);

    auto *styleRow = new QWidget(this);
    auto *styleLay = new QHBoxLayout(styleRow);
    styleLay->setContentsMargins(0, 0, 0, 0);
    m_bold = new QCheckBox(QStringLiteral("Bold"), this);
    m_italic = new QCheckBox(QStringLiteral("Italic"), this);
    connect(m_bold, &QCheckBox::toggled, this, textEdited);
    connect(m_italic, &QCheckBox::toggled, this, textEdited);
    styleLay->addWidget(m_bold);
    styleLay->addWidget(m_italic);
    styleLay->addStretch();
    textRow(QStringLiteral("Style"), styleRow, m_textRows, m_textLabels);

    m_arcOn = new QCheckBox(QStringLiteral("Text along arc"), this);
    connect(m_arcOn, &QCheckBox::toggled, this, textEdited);
    textRow(QStringLiteral("Arc"), m_arcOn, m_textRows, m_textLabels);

    m_arcRadius = new QDoubleSpinBox(this);
    m_arcRadius->setRange(0.1, 10000);
    m_arcRadius->setDecimals(2);
    m_arcRadius->setSuffix(QStringLiteral(" mm"));
    m_arcRadius->setKeyboardTracking(false);
    connect(m_arcRadius, &QDoubleSpinBox::valueChanged, this, textEdited);
    textRow(QStringLiteral("Arc radius"), m_arcRadius, m_arcRows, m_arcLabels);

    m_arcAngle = new QDoubleSpinBox(this);
    m_arcAngle->setRange(-360, 360);
    m_arcAngle->setDecimals(1);
    m_arcAngle->setSuffix(QStringLiteral(" °"));
    m_arcAngle->setKeyboardTracking(false);
    m_arcAngle->setToolTip(QStringLiteral("Turns the text around the arc centre (counter-clockwise positive)"));
    connect(m_arcAngle, &QDoubleSpinBox::valueChanged, this, textEdited);
    textRow(QStringLiteral("Arc offset"), m_arcAngle, m_arcRows, m_arcLabels);

    m_arcBottom = new QCheckBox(QStringLiteral("Text on bottom of circle"), this);
    connect(m_arcBottom, &QCheckBox::toggled, this, textEdited);
    textRow(QStringLiteral("Side"), m_arcBottom, m_arcRows, m_arcLabels);

    m_convert = new QPushButton(QStringLiteral("Convert to path"), this);
    m_convert->setToolTip(QStringLiteral("Turn the shape into editable nodes (N)"));
    connect(m_convert, &QPushButton::clicked, this, [this] {
        if (!m_id.isEmpty())
            m_canvas->convertToPaths({m_id});
    });
    form->addRow(m_convert);

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
    m_rotationRow->setVisible(false);
    const bool isText = have && e->geometryType == QLatin1String("text");
    for (QWidget *w : m_textRows) w->setVisible(isText);
    for (QWidget *w : m_textLabels) w->setVisible(isText);
    const bool arcOn = isText && e->raw.value("arc_enabled").toBool();
    for (QWidget *w : m_arcRows) w->setVisible(arcOn);
    for (QWidget *w : m_arcLabels) w->setVisible(arcOn);
    m_convert->setVisible(have && e->geometryType != QLatin1String("path"));

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
        m_rotationRow->setVisible(true);
        m_radius->setValue(e->raw.value("radius").toDouble());
        m_sides->setValue(int(e->raw.value("num_sides").toDouble()));
        m_rotation->setValue(e->raw.value("rotation").toDouble());
    } else if (isText) {
        const QFont f = Element::textFont(e->raw);
        m_text->setText(e->raw.value("text").toString());
        m_family->setCurrentFont(QFont(f.family()));
        m_familyShown = m_family->currentFont().family();   // substitute if not installed
        m_fontHeight->setValue(e->raw.value("font_height").toDouble(10.0));
        m_bold->setChecked(f.bold());
        m_italic->setChecked(f.italic());
        m_arcOn->setChecked(arcOn);
        m_arcRadius->setValue(e->raw.value("arc_radius").toDouble(25.4));
        m_arcAngle->setValue(e->raw.value("arc_angle_offset").toDouble());
        m_arcBottom->setChecked(e->raw.value("arc_text_on_bottom").toBool());
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
        p.insert(QStringLiteral("rotation"), m_rotation->value());
        m_canvas->editElement(m_id, p);
    } else {
        const QPointF bc = e->painterPath.boundingRect().center();
        m_canvas->moveElementBy(m_id, m_x->value() - bc.x(), m_y->value() - bc.y());
    }
}

// Only the keys that differ from the element go into the change set, so an
// untouched font combo (which may resolve to a substitute family) does not
// rewrite the element's face.
void PropertiesPanel::applyText()
{
    if (!m_doc || m_id.isEmpty())
        return;
    Element *e = m_doc->elementById(m_id);
    if (!e || e->geometryType != QLatin1String("text"))
        return;
    const QFont f = Element::textFont(e->raw);
    QJsonObject ch;
    if (m_text->text() != e->raw.value("text").toString() && !m_text->text().isEmpty())
        ch.insert("text", m_text->text());
    if (m_family->currentFont().family() != m_familyShown)
        ch.insert("family", m_family->currentFont().family());
    if (!qFuzzyCompare(m_fontHeight->value(), e->raw.value("font_height").toDouble(10.0)))
        ch.insert("font_height", m_fontHeight->value());
    if (m_bold->isChecked() != f.bold())
        ch.insert("bold", m_bold->isChecked());
    if (m_italic->isChecked() != f.italic())
        ch.insert("italic", m_italic->isChecked());
    if (m_arcOn->isChecked() != e->raw.value("arc_enabled").toBool())
        ch.insert("arc_enabled", m_arcOn->isChecked());
    if (!qFuzzyCompare(m_arcRadius->value(), e->raw.value("arc_radius").toDouble(25.4)))
        ch.insert("arc_radius", m_arcRadius->value());
    if (!qFuzzyCompare(m_arcAngle->value() + 1000, e->raw.value("arc_angle_offset").toDouble() + 1000))
        ch.insert("arc_angle_offset", m_arcAngle->value());
    if (m_arcBottom->isChecked() != e->raw.value("arc_text_on_bottom").toBool())
        ch.insert("arc_text_on_bottom", m_arcBottom->isChecked());
    if (!ch.isEmpty())
        m_canvas->editText(m_id, ch);
}

} // namespace c2d
