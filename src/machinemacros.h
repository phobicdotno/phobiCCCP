#pragma once
#include <QDialog>
#include <QString>
#include <QStringList>
#include <QVector>

class QSettings;
class QListWidget;
class QLineEdit;
class QPlainTextEdit;

namespace c2d {

// The operator's own buttons on the Machine page: a name and plain G-code
// lines, run one line at a time like Park. Kept in machine/macros, so each
// machine profile (machineprofiles.h) has its own.
struct MachineMacro {
    QString name;
    QStringList lines;   // as typed, notes included: cleanLines() before sending
};

namespace machinemacros {

constexpr int kMax = 8;

QVector<MachineMacro> load(QSettings &s);
void save(QSettings &s, const QVector<MachineMacro> &macros);
// The lines of a typed macro: trimmed, blank lines and whole-line comments
// ("; ..." or "( ... )") left out.
QStringList cleanLines(const QString &text);

// What a line typed on the command line is.
enum class Typed {
    Empty,
    Realtime,   // "?", "!" or "~" alone: sent as the realtime byte
    Home,       // $H: through Initialize, so the panel knows it homed
    Unlock,     // $X: through Unlock
    Line,       // anything else: G-code or a $ command, sent as a line
};
Typed classify(const QString &typed);

// Command-line history: newest last, no repeats in a row, at most `max`.
QStringList pushHistory(QStringList history, const QString &line, int max = 30);

} // namespace machinemacros

// Add, change, delete and reorder the macros. Big buttons for the touch screen.
class MacroEditDialog : public QDialog
{
    Q_OBJECT
public:
    explicit MacroEditDialog(const QVector<MachineMacro> &macros, QWidget *parent = nullptr);
    QVector<MachineMacro> macros() const { return m_macros; }

private:
    void refreshList();
    void showCurrent();
    void storeCurrent();
    void move(int by);

    QVector<MachineMacro> m_macros;
    QListWidget *m_list = nullptr;
    QLineEdit *m_name = nullptr;
    QPlainTextEdit *m_lines = nullptr;
    int m_shown = -1;
    bool m_filling = false;
};

} // namespace c2d
