#include "settingsdialog.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QSettings>
#include <QVBoxLayout>

namespace c2d {

namespace appsettings {

QList<int> uiScaleChoices()
{
    return {100, 125, 150, 175, 200};
}

int uiScalePercent(QSettings &s)
{
    const int v = s.value(QStringLiteral("ui/scale"), 100).toInt();
    return uiScaleChoices().contains(v) ? v : 100;
}

// Carbide 3D's published cutting areas, X × Y. The Shapeoko 3 figures are
// its nominal 16" / 33" sizes; the 4 and Pro grew to 17.5" on the short side.
QList<Machine> machines()
{
    auto m = [](const char *key, const char *name, double w, double h) {
        return Machine{QString::fromLatin1(key),
                       QStringLiteral("%1 (%2 × %3 mm)").arg(QString::fromUtf8(name))
                           .arg(qRound(w)).arg(qRound(h)),
                       w, h};
    };
    return {
        m("s3-standard", "Shapeoko 3 Standard", 406.4, 406.4),
        m("s3-xl", "Shapeoko 3 XL", 406.4, 838.2),
        m("s3-xxl", "Shapeoko 3 XXL", 838.2, 838.2),
        m("s4-standard", "Shapeoko 4 Standard", 444.5, 444.5),
        m("s4-xl", "Shapeoko 4 XL", 444.5, 838.2),
        m("s4-xxl", "Shapeoko 4 XXL", 838.2, 838.2),
        m("pro-standard", "Shapeoko Pro Standard", 444.5, 444.5),
        m("pro-xl", "Shapeoko Pro XL", 444.5, 838.2),
        m("pro-xxl", "Shapeoko Pro XXL", 838.2, 838.2),
        m("s5pro-2x2", "Shapeoko 5 Pro 2×2", 623, 623),
        m("s5pro-4x2", "Shapeoko 5 Pro 4×2", 1237, 623),
        m("s5pro-4x4", "Shapeoko 5 Pro 4×4", 1237, 1237),
    };
}

Machine machine(QSettings &s)
{
    QString key = s.value(QStringLiteral("machine/size"), QStringLiteral("s5pro-4x4")).toString();
    if (key == QLatin1String("custom")) {
        const double w = s.value(QStringLiteral("machine/customWidth"), 1000).toDouble();
        const double h = s.value(QStringLiteral("machine/customHeight"), 1000).toDouble();
        return {key, QStringLiteral("Custom (%1 × %2 mm)").arg(w).arg(h), w, h};
    }
    if (!key.contains(QLatin1Char('-')))
        key.prepend(QStringLiteral("s5pro-"));   // the first keys were "2x2" ... "4x4"
    const QList<Machine> all = machines();
    for (const Machine &m : all)
        if (m.key == key)
            return m;
    return all.last();
}

} // namespace appsettings

int appsettings::panelTextPercent(QSettings &s)
{
    const int v = s.value(QStringLiteral("ui/panelText"), 125).toInt();
    return panelTextChoices().contains(v) ? v : 125;
}

QList<int> appsettings::panelTextChoices() { return {100, 110, 125, 150, 175, 200}; }

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent), m_scale(new QComboBox(this)), m_panelText(new QComboBox(this)),
      m_machine(new QComboBox(this)),
      m_customW(new QDoubleSpinBox(this)), m_customH(new QDoubleSpinBox(this)), m_tabHeight(new QDoubleSpinBox(this))
{
    setWindowTitle(QStringLiteral("Settings"));
    QSettings s;

    m_scaleWas = appsettings::uiScalePercent(s);
    for (int pct : appsettings::uiScaleChoices()) {
        m_scale->addItem(QStringLiteral("%1 %").arg(pct), pct);
        if (pct == m_scaleWas)
            m_scale->setCurrentIndex(m_scale->count() - 1);
    }

    const int panelText = appsettings::panelTextPercent(s);
    for (int pct : appsettings::panelTextChoices()) {
        m_panelText->addItem(QStringLiteral("%1 %").arg(pct), pct);
        if (pct == panelText)
            m_panelText->setCurrentIndex(m_panelText->count() - 1);
    }

    const auto current = appsettings::machine(s);
    for (const auto &m : appsettings::machines()) {
        m_machine->addItem(m.label, m.key);
        if (m.key == current.key)
            m_machine->setCurrentIndex(m_machine->count() - 1);
    }
    m_machine->addItem(QStringLiteral("Custom size"), QStringLiteral("custom"));
    if (current.key == QLatin1String("custom"))
        m_machine->setCurrentIndex(m_machine->count() - 1);

    for (QDoubleSpinBox *box : {m_customW, m_customH}) {
        box->setRange(10, 5000);
        box->setDecimals(1);
        box->setSuffix(QStringLiteral(" mm"));
    }
    m_customW->setValue(s.value(QStringLiteral("machine/customWidth"), 1000).toDouble());
    m_customH->setValue(s.value(QStringLiteral("machine/customHeight"), 1000).toDouble());
    m_tabHeight->setRange(0.1, 50);
    m_tabHeight->setDecimals(2);
    m_tabHeight->setSuffix(QStringLiteral(" mm"));
    m_tabHeight->setValue(s.value(QStringLiteral("defaults/tabHeight"), 3.0).toDouble());
    auto syncCustom = [this] {
        const bool custom = m_machine->currentData().toString() == QLatin1String("custom");
        m_customW->setEnabled(custom);
        m_customH->setEnabled(custom);
    };
    connect(m_machine, &QComboBox::currentIndexChanged, this, syncCustom);
    syncCustom();

    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("GUI scale:"), m_scale);
    form->addRow(QStringLiteral("Toolpaths text size:"), m_panelText);
    form->addRow(QStringLiteral("Machine:"), m_machine);
    form->addRow(QStringLiteral("Custom width (X):"), m_customW);
    form->addRow(QStringLiteral("Custom height (Y):"), m_customH);
    form->addRow(QStringLiteral("Tab height:"), m_tabHeight);

    auto *note = new QLabel(QStringLiteral(
        "The GUI scale applies after a restart; the Toolpaths text size at once. The machine sets the size of the "
        "blank design phobiCCCP starts with, and the default for File → New. The tab height is what a new "
        "contour starts with (each toolpath can change its own)."), this);
    note->setWordWrap(true);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *lay = new QVBoxLayout(this);
    lay->addLayout(form);
    lay->addWidget(note);
    lay->addWidget(buttons);
}

bool SettingsDialog::save()
{
    QSettings s;
    const int scale = m_scale->currentData().toInt();
    s.setValue(QStringLiteral("ui/scale"), scale);
    s.setValue(QStringLiteral("ui/panelText"), m_panelText->currentData().toInt());
    s.setValue(QStringLiteral("machine/size"), m_machine->currentData().toString());
    s.setValue(QStringLiteral("machine/customWidth"), m_customW->value());
    s.setValue(QStringLiteral("machine/customHeight"), m_customH->value());
    s.setValue(QStringLiteral("defaults/tabHeight"), m_tabHeight->value());
    return scale != m_scaleWas;
}

} // namespace c2d
