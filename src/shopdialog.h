#pragma once
#include <QPixmap>
#include <QString>

class QWidget;

namespace c2d {

// The Machine page's own popups, in its look: dark card, a headline, an
// optional amber note, big touch-sized buttons. Used for the questions that
// come up while a job runs (tool change, stream the project) in place of
// the desktop's stock message box.
struct ShopAsk {
    QString title;            // window title, and the small caps caption
    QString headline;         // one line, large
    QString body;             // plain text; blank lines start new paragraphs
    QString note;             // optional, shown in an amber (or red) box
    QPixmap picture;          // optional, beside the headline and body (a tool diagram)
    bool noteDanger = false;  // red note box instead of amber
    QString okText = QStringLiteral("OK");
    QString cancelText = QStringLiteral("Cancel");   // empty: no cancel button
    QString altText;          // optional third answer, between Cancel and OK
    bool okDanger = false;    // OK in red (destructive or risky) instead of amber
    bool defaultOk = true;    // Enter presses OK; otherwise Cancel
};

enum class ShopAnswer { Cancel, Ok, Alt };

// Which button closed it; Esc is Cancel.
ShopAnswer shopChoose(QWidget *parent, const ShopAsk &a);
// True when OK was pressed.
bool shopAsk(QWidget *parent, const ShopAsk &a);

// An information popup with a single OK.
void shopTell(QWidget *parent, const QString &title, const QString &headline,
              const QString &body = QString());

} // namespace c2d
