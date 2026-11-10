#pragma once
#include <QObject>

namespace c2d {

// Either decimal key works in every number box. A number box follows the
// system's number format: with comma decimals (Norwegian, German, ...) Qt
// throws a typed "." away, so "3.175" became 317; with dot decimals a comma
// from the numeric keypad went the same way. With this filter on the
// application, a typed "." or "," that is not the box's own decimal sign is
// taken as it. Expressions ("max(a, 2)", "stock_t + 0.5") are left as typed.
class DecimalKey : public QObject
{
public:
    using QObject::QObject;
    static void install(QObject *app);

protected:
    bool eventFilter(QObject *o, QEvent *e) override;
};

} // namespace c2d
