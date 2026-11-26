#include "apptheme.h"
#include "touchprofile.h"

#include <QApplication>
#include <QPalette>
#include <QProxyStyle>
#include <QStyleFactory>

namespace c2d {

namespace {

// Tool buttons with variants open their menu after a 500 ms press-and-hold
// (Fusion's default is 600 ms); a plain click uses the current variant.
// Scrollbars and splitter handles are thicker than Fusion's so a finger can
// hold them on the shop PC's touchscreen. All of it is in logical pixels, so
// it grows with the GUI scale like everything else.
class PhobicStyle : public QProxyStyle
{
public:
    using QProxyStyle::QProxyStyle;
    int pixelMetric(PixelMetric m, const QStyleOption *opt, const QWidget *w) const override
    {
        switch (m) {
        case PM_ScrollBarExtent:    return 22;   // Fusion: 14
        case PM_ScrollBarSliderMin: return 40;
        case PM_SplitterWidth:      return 12;
        default:                    return QProxyStyle::pixelMetric(m, opt, w);
        }
    }
    int styleHint(StyleHint hint, const QStyleOption *opt, const QWidget *w,
                  QStyleHintReturn *ret) const override
    {
        if (hint == SH_ToolButton_PopupDelay)
            return 500;
        // Plain OK / Cancel: no tick and cross icons on dialog buttons.
        if (hint == SH_DialogButtonBox_ButtonsHaveIcons)
            return 0;
        return QProxyStyle::styleHint(hint, opt, w, ret);
    }
};

} // namespace

// Fusion + a hand-tuned dark palette; small QSS pass for spacing/accents.
void applyAppTheme(QApplication &app)
{
    app.setStyle(touch::wrap(new PhobicStyle(QStyleFactory::create(QStringLiteral("Fusion")))));

    QPalette p;
    const QColor bg(0x1e, 0x21, 0x27), base(0x16, 0x18, 0x1d),
        text(0xd8, 0xdc, 0xe4), disabled(0x6a, 0x71, 0x7d),
        accent(0xf2, 0xa5, 0x36), button(0x2a, 0x2e, 0x36);
    p.setColor(QPalette::Window, bg);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, bg);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, button);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, QColor(0x1a, 0x1a, 0x1a));
    p.setColor(QPalette::ToolTipBase, base);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, disabled);
    p.setColor(QPalette::Disabled, QPalette::Text, disabled);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    p.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    app.setPalette(p);

    app.setStyleSheet(QStringLiteral(R"(
        QMenuBar { font-size: 14px; background: transparent; }
        QMenuBar::item { padding: 7px 10px; border-radius: 6px; }
        QMenuBar::item:selected { background: #333945; }
        QMenu { font-size: 14px; }
        QMenu::item { padding: 6px 28px 6px 12px; }
        QMenu::item:selected { background: palette(highlight); color: palette(highlighted-text); }
        QMenu::item:disabled { color: palette(mid); background: transparent; }
        QToolBar { spacing: 4px; padding: 4px; border: 0; }
        QToolButton { padding: 4px 8px; border-radius: 4px; }
        QToolButton:checked { background: #3a4150; }
        QToolButton:hover { background: #333945; }
        QStatusBar { background: #16181d; }
        QPlainTextEdit { font-family: monospace; font-size: 12px; border: 0; }
        QDockWidget::title { background: #23262e; padding: 7px; font-size: 13px; }
    )")
        // Finger-sized spin boxes draw their own field and buttons (see
        // touchprofile.cpp); a padding rule would make the style sheet
        // lay them out instead.
        + (touch::enabled() ? QString() : QStringLiteral("QSpinBox { padding: 2px 4px; }"))
        + touch::styleSheet());
}

} // namespace c2d
