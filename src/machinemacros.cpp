#include "machinemacros.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QVBoxLayout>

namespace c2d {

namespace machinemacros {

QVector<MachineMacro> load(QSettings &s)
{
    QVector<MachineMacro> out;
    const int n = s.beginReadArray(QStringLiteral("machine/macros"));
    for (int i = 0; i < n && out.size() < kMax; ++i) {
        s.setArrayIndex(i);
        MachineMacro m;
        m.name = s.value(QStringLiteral("name")).toString().trimmed();
        const QString text = s.value(QStringLiteral("gcode")).toString().trimmed();
        if (!text.isEmpty())
            m.lines = text.split(QChar('\n'));
        if (!m.name.isEmpty() || !m.lines.isEmpty())
            out << m;
    }
    s.endArray();
    return out;
}

void save(QSettings &s, const QVector<MachineMacro> &macros)
{
    s.remove(QStringLiteral("machine/macros"));
    s.beginWriteArray(QStringLiteral("machine/macros"));
    for (int i = 0; i < macros.size() && i < kMax; ++i) {
        s.setArrayIndex(i);
        s.setValue(QStringLiteral("name"), macros.at(i).name);
        s.setValue(QStringLiteral("gcode"), macros.at(i).lines.join(QChar('\n')));
    }
    s.endArray();
}

QStringList cleanLines(const QString &text)
{
    QStringList out;
    const QStringList raw = text.split(QChar('\n'));
    // A note in brackets ends at its ')': "(retract) G53 G0 Z-5 (up)" still
    // has a move in it. Only a line that is nothing but notes is left out.
    static const QRegularExpression paren(QStringLiteral("\\([^)]*\\)"));
    for (QString l : raw) {
        l = l.trimmed();
        if (l.isEmpty() || l.startsWith(QChar(';')))
            continue;
        QString bare = l;
        bare.remove(paren);
        bare = bare.trimmed();
        if (bare.isEmpty() || bare.startsWith(QChar(';')))
            continue;
        out << l;
    }
    return out;
}

Typed classify(const QString &typed)
{
    const QString t = typed.trimmed();
    if (t.isEmpty())
        return Typed::Empty;
    if (t == QLatin1String("?") || t == QLatin1String("!") || t == QLatin1String("~"))
        return Typed::Realtime;
    const QString u = t.toUpper().remove(QChar(' '));
    if (u == QLatin1String("$H"))
        return Typed::Home;
    if (u == QLatin1String("$X"))
        return Typed::Unlock;
    return Typed::Line;
}

QStringList pushHistory(QStringList history, const QString &line, int max)
{
    const QString l = line.trimmed();
    if (l.isEmpty())
        return history;
    if (history.isEmpty() || history.last() != l)
        history << l;
    while (history.size() > max)
        history.removeFirst();
    return history;
}

} // namespace machinemacros

MacroEditDialog::MacroEditDialog(const QVector<MachineMacro> &macros, QWidget *parent)
    : QDialog(parent), m_macros(macros)
{
    setWindowTitle(QStringLiteral("Macros"));
    setObjectName(QStringLiteral("macroDialog"));
    auto big = [this](const QString &text) {
        auto *b = new QPushButton(text, this);
        b->setObjectName(QStringLiteral("big"));
        b->setMinimumHeight(48);
        b->setAutoDefault(false);
        return b;
    };

    auto *lay = new QVBoxLayout(this);
    auto *head = new QLabel(QStringLiteral(
        "Your own buttons on the Machine page, for this machine. Each one sends its "
        "G-code lines in order, one at a time, like Park. Up to %1. They do not run "
        "while a job runs.").arg(machinemacros::kMax), this);
    head->setWordWrap(true);
    lay->addWidget(head);

    auto *cols = new QHBoxLayout;
    auto *left = new QVBoxLayout;
    m_list = new QListWidget(this);
    m_list->setMinimumWidth(220);
    QFont f = m_list->font();
    f.setPointSizeF(f.pointSizeF() * 1.25);
    m_list->setFont(f);
    left->addWidget(m_list, 1);
    auto *addBtn = big(QStringLiteral("+ Add"));
    auto *delBtn = big(QStringLiteral("Delete"));
    auto *upBtn = big(QStringLiteral("▲ Up"));
    auto *downBtn = big(QStringLiteral("▼ Down"));
    auto *row1 = new QHBoxLayout;
    row1->addWidget(addBtn);
    row1->addWidget(delBtn);
    auto *row2 = new QHBoxLayout;
    row2->addWidget(upBtn);
    row2->addWidget(downBtn);
    left->addLayout(row1);
    left->addLayout(row2);
    cols->addLayout(left, 2);

    auto *right = new QVBoxLayout;
    right->addWidget(new QLabel(QStringLiteral("Name on the button"), this));
    m_name = new QLineEdit(this);
    m_name->setMinimumHeight(40);
    m_name->setMaxLength(24);
    right->addWidget(m_name);
    right->addWidget(new QLabel(QStringLiteral("G-code, one line each (lines starting with ; are notes)"), this));
    m_lines = new QPlainTextEdit(this);
    QFont mono(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::Monospace);
    m_lines->setFont(mono);
    m_lines->setMinimumSize(360, 220);
    m_lines->setPlaceholderText(QStringLiteral("G90\nG53 G0 Z-5\nG53 G0 X-400 Y-5"));
    right->addWidget(m_lines, 1);
    cols->addLayout(right, 3);
    lay->addLayout(cols, 1);

    auto *foot = new QHBoxLayout;
    foot->addStretch(1);
    auto *cancel = big(QStringLiteral("Cancel"));
    auto *ok = big(QStringLiteral("Save"));
    foot->addWidget(cancel);
    foot->addWidget(ok);
    lay->addLayout(foot);

    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(ok, &QPushButton::clicked, this, [this] {
        storeCurrent();
        QVector<MachineMacro> keep;
        for (MachineMacro m : std::as_const(m_macros)) {
            if (machinemacros::cleanLines(m.lines.join(QChar('\n'))).isEmpty())
                continue;   // a macro with nothing to send is no button
            if (m.name.isEmpty())
                m.name = QStringLiteral("Macro %1").arg(keep.size() + 1);
            keep << m;
        }
        m_macros = keep;
        accept();
    });
    connect(addBtn, &QPushButton::clicked, this, [this] {
        storeCurrent();
        if (m_macros.size() >= machinemacros::kMax)
            return;
        m_macros << MachineMacro{QStringLiteral("Macro %1").arg(m_macros.size() + 1), {}};
        m_shown = -1;
        refreshList();
        m_list->setCurrentRow(m_macros.size() - 1);
        m_name->setFocus();
        m_name->selectAll();
    });
    connect(delBtn, &QPushButton::clicked, this, [this] {
        const int i = m_list->currentRow();
        if (i < 0 || i >= m_macros.size())
            return;
        m_macros.removeAt(i);
        m_shown = -1;
        refreshList();
        m_list->setCurrentRow(qMin(i, int(m_macros.size()) - 1));
        showCurrent();
    });
    connect(upBtn, &QPushButton::clicked, this, [this] { move(-1); });
    connect(downBtn, &QPushButton::clicked, this, [this] { move(1); });
    connect(m_list, &QListWidget::currentRowChanged, this, [this] {
        if (m_filling)
            return;
        storeCurrent();
        showCurrent();
    });
    connect(m_name, &QLineEdit::textEdited, this, [this](const QString &t) {
        if (m_shown >= 0 && m_shown < m_macros.size() && m_list->item(m_shown))
            m_list->item(m_shown)->setText(t);
    });

    refreshList();
    m_list->setCurrentRow(m_macros.isEmpty() ? -1 : 0);
    showCurrent();
    resize(780, 460);
}

void MacroEditDialog::refreshList()
{
    m_filling = true;
    m_list->clear();
    for (const MachineMacro &m : std::as_const(m_macros))
        m_list->addItem(m.name);
    m_filling = false;
}

void MacroEditDialog::storeCurrent()
{
    if (m_shown < 0 || m_shown >= m_macros.size())
        return;
    m_macros[m_shown].name = m_name->text().trimmed();
    const QString text = m_lines->toPlainText().trimmed();
    m_macros[m_shown].lines = text.isEmpty() ? QStringList() : text.split(QChar('\n'));
}

void MacroEditDialog::showCurrent()
{
    m_shown = m_list->currentRow();
    const bool have = m_shown >= 0 && m_shown < m_macros.size();
    m_name->setEnabled(have);
    m_lines->setEnabled(have);
    m_name->setText(have ? m_macros.at(m_shown).name : QString());
    m_lines->setPlainText(have ? m_macros.at(m_shown).lines.join(QChar('\n')) : QString());
}

void MacroEditDialog::move(int by)
{
    storeCurrent();
    const int i = m_list->currentRow();
    const int j = i + by;
    if (i < 0 || j < 0 || j >= m_macros.size())
        return;
    m_macros.swapItemsAt(i, j);
    m_shown = -1;
    refreshList();
    m_list->setCurrentRow(j);
}

} // namespace c2d
