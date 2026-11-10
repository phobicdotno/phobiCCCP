#include "decimalkey.h"

#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QLineEdit>

namespace c2d {

void DecimalKey::install(QObject *app)
{
    app->installEventFilter(new DecimalKey(app));
}

bool DecimalKey::eventFilter(QObject *o, QEvent *e)
{
    if (e->type() != QEvent::KeyPress)
        return false;
    auto *k = static_cast<QKeyEvent *>(e);
    const QString t = k->text();
    if (t != QLatin1String(".") && t != QLatin1String(","))
        return false;
    if (k->modifiers() & ~(Qt::KeypadModifier | Qt::ShiftModifier))
        return false;
    // The key goes to the box itself, or to the line edit inside it.
    auto *box = qobject_cast<QDoubleSpinBox *>(o);
    if (!box && qobject_cast<QLineEdit *>(o))
        box = qobject_cast<QDoubleSpinBox *>(o->parent());
    if (!box || box->isReadOnly())
        return false;
    const QLocale loc = box->locale();
    const QString dp = loc.decimalPoint();
    if (t == dp || (box->isGroupSeparatorShown() && t == loc.groupSeparator()))
        return false;
    // Only in a plain number: anything else is an expression, where both
    // signs have a meaning of their own.
    const QString text = box->cleanText();
    for (const QChar c : text) {
        if (!(c.isDigit() || c.isSpace() || c == QLatin1Char('-') || c == QLatin1Char('+')
              || dp.contains(c) || loc.groupSeparator().contains(c)))
            return false;
    }
    QKeyEvent as(QEvent::KeyPress, dp == QLatin1String(",") ? Qt::Key_Comma : Qt::Key_Period,
                 k->modifiers(), dp, k->isAutoRepeat(), k->count());
    QCoreApplication::sendEvent(o, &as);
    return true;
}

} // namespace c2d
