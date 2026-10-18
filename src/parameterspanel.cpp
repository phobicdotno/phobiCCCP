#include "parameterspanel.h"
#include "canvas.h"
#include "c2ddocument.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace c2d {

static const QColor kTeal(0x7c, 0xd6, 0xc2);
static const QColor kRed(0xe8, 0x5c, 0x5c);
static const QColor kMuted(0x8b, 0x93, 0xa1);
static const int kBuiltins = 3;   // stock_w, stock_h, stock_t head the parameter table

static QLabel *sectionTitle(const QString &text, QWidget *parent)
{
    auto *l = new QLabel(text.toUpper(), parent);
    l->setStyleSheet(QStringLiteral(
        "color: #8b93a1; font-size: 11px; font-weight: 700; letter-spacing: 1px; padding-top: 6px;"));
    return l;
}

static QTableWidget *table(const QStringList &headers, QWidget *parent)
{
    auto *t = new QTableWidget(0, headers.size(), parent);
    t->setHorizontalHeaderLabels(headers);
    t->verticalHeader()->setVisible(false);
    t->verticalHeader()->setDefaultSectionSize(30);
    t->horizontalHeader()->setStretchLastSection(true);
    t->horizontalHeader()->setHighlightSections(false);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->setShowGrid(false);
    t->setAlternatingRowColors(true);
    t->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed
                       | QAbstractItemView::SelectedClicked);
    t->setStyleSheet(QStringLiteral(
        "QTableWidget { background: #1b1e24; alternate-background-color: #1f232a;"
        " border: 1px solid #2a2e36; border-radius: 6px; }"
        "QTableWidget::item { padding: 0 6px; }"
        "QTableWidget::item:selected { background: #2f3a4a; color: #f4f6fa; }"
        "QHeaderView::section { background: #23262e; color: #aab1bd; border: 0;"
        " border-bottom: 1px solid #2a2e36; padding: 5px 6px; font-weight: 600; }"));
    return t;
}

static QPushButton *smallButton(const QString &text, QWidget *parent)
{
    auto *b = new QPushButton(text, parent);
    b->setCursor(Qt::PointingHandCursor);
    b->setStyleSheet(QStringLiteral(
        "QPushButton { background: #2a2e36; color: #d8dce4; border: 1px solid #3a404b;"
        " border-radius: 6px; padding: 5px 12px; font-weight: 600; }"
        "QPushButton:hover { background: #333944; }"
        "QPushButton:disabled { color: #5a616d; }"));
    return b;
}

ParametersPanel::ParametersPanel(Canvas *canvas, QWidget *parent)
    : QWidget(parent), m_canvas(canvas)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(10, 10, 10, 10);
    v->setSpacing(6);

    // Status chip, and the counts beside it while there is room.
    auto *top = new QHBoxLayout;
    m_chip = new QLabel(this);
    m_counts = new QLabel(this);
    m_counts->setStyleSheet(QStringLiteral("color: #8b93a1;"));
    m_counts->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_counts->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    top->addWidget(m_chip);
    top->addWidget(m_counts, 1);
    v->addLayout(top);

    // User parameters.
    auto *ph = new QHBoxLayout;
    ph->addWidget(sectionTitle(tr("User parameters"), this));
    ph->addStretch(1);
    auto *add = smallButton(tr("+ Add"), this);
    m_remove = smallButton(tr("Remove"), this);
    ph->addWidget(add);
    ph->addWidget(m_remove);
    v->addLayout(ph);
    m_params = table({tr("Name"), tr("Expression"), tr("Value"), tr("Comment")}, this);
    m_params->setColumnWidth(0, 90);
    m_params->setColumnWidth(1, 120);
    m_params->setColumnWidth(2, 70);
    m_params->setMinimumHeight(150);
    v->addWidget(m_params, 3);
    connect(add, &QPushButton::clicked, this, &ParametersPanel::addParameter);
    connect(m_remove, &QPushButton::clicked, this, &ParametersPanel::removeParameter);
    connect(m_params, &QTableWidget::itemChanged, this, &ParametersPanel::paramEdited);
    connect(m_params, &QTableWidget::itemSelectionChanged, this, [this] {
        m_remove->setEnabled(m_params->currentRow() >= kBuiltins);
    });

    // Dimensions.
    v->addWidget(sectionTitle(tr("Dimensions"), this));
    m_dims = table({tr("Name"), tr("Measures"), tr("Expression"), tr("Value")}, this);
    m_dims->setColumnWidth(0, 50);
    m_dims->setColumnWidth(1, 120);
    m_dims->setColumnWidth(2, 120);
    m_dims->setMinimumHeight(110);
    v->addWidget(m_dims, 3);
    connect(m_dims, &QTableWidget::itemChanged, this, &ParametersPanel::dimensionEdited);
    connect(m_dims, &QTableWidget::itemSelectionChanged, this, [this] {
        if (m_updating)
            return;
        const int r = m_dims->currentRow();
        if (r >= 0 && m_dims->item(r, 0))
            m_canvas->selectConstraint(m_dims->item(r, 0)->data(Qt::UserRole).toString());
    });

    // Geometric constraints.
    auto *ch = new QHBoxLayout;
    ch->addWidget(sectionTitle(tr("Constraints"), this));
    ch->addStretch(1);
    m_delCon = smallButton(tr("Delete"), this);
    ch->addWidget(m_delCon);
    v->addLayout(ch);
    m_cons = new QListWidget(this);
    m_cons->setIconSize(QSize(18, 18));
    m_cons->setMinimumHeight(110);
    m_cons->setStyleSheet(QStringLiteral(
        "QListWidget { background: #1b1e24; border: 1px solid #2a2e36; border-radius: 6px; }"
        "QListWidget::item { padding: 4px 4px; }"
        "QListWidget::item:selected { background: #2f3a4a; color: #f4f6fa; }"));
    v->addWidget(m_cons, 3);
    connect(m_cons, &QListWidget::itemSelectionChanged, this, [this] {
        m_delCon->setEnabled(m_cons->currentItem() != nullptr);
        if (m_updating || !m_cons->currentItem())
            return;
        m_canvas->selectConstraint(m_cons->currentItem()->data(Qt::UserRole).toString());
    });
    connect(m_delCon, &QPushButton::clicked, this, [this] {
        if (QListWidgetItem *it = m_cons->currentItem())
            m_canvas->deleteConstraint(it->data(Qt::UserRole).toString());
    });

    auto *help = new QLabel(tr(
        "Names work in every dimension and in toolpath number fields: <i>plate_w - 2*margin</i>, "
        "<i>stock_t + 0.5</i>, <i>1/4 in</i>. Units mm cm m in ft deg; functions sin cos tan sqrt "
        "abs min max round. Double-click a dimension on the canvas to change it."), this);
    help->setWordWrap(true);
    help->setStyleSheet(QStringLiteral("color: #8b93a1; font-size: 11px; padding-top: 4px;"));
    v->addWidget(help);

    connect(m_canvas, &Canvas::documentChanged, this, &ParametersPanel::refresh);
    connect(m_canvas, &Canvas::constraintSelected, this, &ParametersPanel::showConstraint);
    refresh();
}

void ParametersPanel::setDocument(Document *doc)
{
    m_doc = doc;
    refresh();
}

QString ParametersPanel::describe(int i) const
{
    const Constraint &c = m_doc->parametric().constraints.at(i);
    QStringList parts;
    for (const GeoRef &r : c.refs) {
        if (r.isOrigin()) {
            parts << tr("origin");
            continue;
        }
        QString what = r.kind == GeoRef::Line ? tr("line") : r.kind == GeoRef::Circle ? tr("circle")
                                                                                        : tr("point");
        if (r.kind == GeoRef::Point && r.index < 0)
            what = tr("center");
        const Element *e = nullptr;
        for (const Element &el : m_doc->elements())
            if (el.id == r.id) { e = &el; break; }
        if (!e) {
            parts << tr("(deleted)");
            continue;
        }
        QString shape = e->geometryType == QLatin1String("regular_polygon") ? tr("polygon")
                        : Element::isPoint(*e) ? tr("sketch point") : e->geometryType;
        parts << (r.kind == GeoRef::Circle || Element::isPoint(*e) ? shape
                                                                  : tr("%1 of %2").arg(what, shape));
    }
    return QStringLiteral("%1  ·  %2").arg(Constraint::typeName(c.type), parts.join(QStringLiteral(", ")));
}

void ParametersPanel::refresh()
{
    m_updating = true;
    const QString selUid = m_canvas->selectedConstraint();
    const int keepParamRow = m_params->currentRow();
    m_params->setRowCount(0);
    m_dims->setRowCount(0);
    m_cons->clear();
    setEnabled(m_doc != nullptr);
    if (!m_doc) {
        m_chip->clear();
        m_counts->clear();
        m_updating = false;
        return;
    }
    const Parametric &par = m_doc->parametric();
    QStringList errors;
    const QHash<QString, double> vals = param::values(*m_doc, par, &errors);
    auto errorFor = [&](const QString &name) {
        for (const QString &e : errors)
            if (e.startsWith(name + QLatin1Char(':')))
                return e.mid(name.size() + 1).trimmed();
        return QString();
    };
    auto ro = [](const QString &text) {
        auto *it = new QTableWidgetItem(text);
        it->setFlags(it->flags() & ~Qt::ItemIsEditable);
        return it;
    };
    auto valueItem = [&](const QString &name, const QString &unit) {
        const QString why = errorFor(name);
        auto *it = ro(why.isEmpty() ? QStringLiteral("%1 %2").arg(param::formatNumber(vals.value(name)), unit).trimmed()
                                    : tr("error"));
        it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        if (!why.isEmpty()) {
            it->setForeground(kRed);
            it->setToolTip(why);
        }
        return it;
    };

    // Parameters: the stock first, read-only.
    const QStringList builtinNotes = {tr("Stock width"), tr("Stock height"), tr("Stock thickness")};
    const QStringList builtins = param::builtinNames();
    for (int i = 0; i < kBuiltins; ++i) {
        m_params->insertRow(i);
        auto *n = ro(builtins.at(i));
        QFont f = n->font();
        f.setItalic(true);
        n->setFont(f);
        n->setForeground(kMuted);
        m_params->setItem(i, 0, n);
        auto *e = ro(param::formatNumber(vals.value(builtins.at(i))));
        e->setForeground(kMuted);
        m_params->setItem(i, 1, e);
        m_params->setItem(i, 2, valueItem(builtins.at(i), QStringLiteral("mm")));
        auto *c = ro(builtinNotes.at(i));
        c->setForeground(kMuted);
        m_params->setItem(i, 3, c);
    }
    for (int i = 0; i < par.params.size(); ++i) {
        const UserParameter &up = par.params.at(i);
        const int row = kBuiltins + i;
        m_params->insertRow(row);
        m_params->setItem(row, 0, new QTableWidgetItem(up.name));
        m_params->setItem(row, 1, new QTableWidgetItem(up.expr));
        m_params->setItem(row, 2, valueItem(up.name, QString()));
        m_params->setItem(row, 3, new QTableWidgetItem(up.comment));
    }
    if (keepParamRow >= 0 && keepParamRow < m_params->rowCount())
        m_params->selectRow(keepParamRow);
    m_remove->setEnabled(m_params->currentRow() >= kBuiltins);

    // Dimensions and constraints.
    int nDims = 0, nCons = 0;
    const QStringList failing = m_canvas->failingConstraints();
    for (int i = 0; i < par.constraints.size(); ++i) {
        const Constraint &c = par.constraints.at(i);
        if (!param::resolves(*m_doc, c))
            continue;
        const bool bad = failing.contains(c.uid);
        if (c.isDimension()) {
            const int row = m_dims->rowCount();
            m_dims->insertRow(row);
            auto *n = new QTableWidgetItem(c.name);
            n->setData(Qt::UserRole, c.uid);
            m_dims->setItem(row, 0, n);
            auto *t = ro(describe(i).section(QStringLiteral("  ·  "), 0, 0));
            t->setIcon(QIcon(constraintPixmap(c.type, kMuted, 16, devicePixelRatioF())));
            t->setToolTip(describe(i));
            m_dims->setItem(row, 1, t);
            m_dims->setItem(row, 2, new QTableWidgetItem(c.expr));
            auto *val = valueItem(c.name, c.type == Constraint::Angle ? QStringLiteral("°")
                                                                     : QStringLiteral("mm"));
            if (bad) {
                val->setForeground(kRed);
                val->setToolTip(tr("Can't hold together with the other constraints"));
            }
            m_dims->setItem(row, 3, val);
            if (c.uid == selUid)
                m_dims->selectRow(row);
            ++nDims;
        } else {
            auto *it = new QListWidgetItem(
                QIcon(constraintPixmap(c.type, bad ? kRed : kTeal, 18, devicePixelRatioF())),
                describe(i), m_cons);
            it->setData(Qt::UserRole, c.uid);
            if (bad) {
                it->setForeground(kRed);
                it->setToolTip(tr("Can't hold together with the other constraints"));
            }
            if (c.uid == selUid)
                m_cons->setCurrentItem(it);
            ++nCons;
        }
    }
    m_delCon->setEnabled(m_cons->currentItem() != nullptr);

    // The chip.
    QString chipText, fg, bg;
    if (nDims + nCons == 0) {
        chipText = tr("No constraints yet");
        fg = QStringLiteral("#aab1bd");
        bg = QStringLiteral("rgba(170,177,189,0.10)");
    } else if (!failing.isEmpty()) {
        chipText = failing.size() == 1 ? tr("1 constraint can't hold")
                                       : tr("%1 constraints can't hold").arg(failing.size());
        fg = QStringLiteral("#e85c5c");
        bg = QStringLiteral("rgba(232,92,92,0.14)");
    } else if (m_canvas->freedomLeft() == 0) {
        chipText = tr("Fully constrained");
        fg = QStringLiteral("#7cd6c2");
        bg = QStringLiteral("rgba(124,214,194,0.14)");
    } else {
        chipText = m_canvas->freedomLeft() == 1 ? tr("1 degree of freedom left")
                                                : tr("%1 degrees of freedom left").arg(m_canvas->freedomLeft());
        fg = QStringLiteral("#f2a536");
        bg = QStringLiteral("rgba(242,165,54,0.14)");
    }
    m_chip->setText(QStringLiteral("●  %1").arg(chipText));
    m_chip->setStyleSheet(QStringLiteral("color: %1; background: %2; border-radius: 11px;"
                                         " padding: 4px 11px; font-weight: 700;").arg(fg, bg));
    auto count = [](int n, const QString &one, const QString &many) {
        return QStringLiteral("%1 %2").arg(n).arg(n == 1 ? one : many);
    };
    m_counts->setText(QStringLiteral("%1 · %2 · %3").arg(
        count(par.params.size(), tr("parameter"), tr("parameters")),
        count(nDims, tr("dimension"), tr("dimensions")),
        count(nCons, tr("constraint"), tr("constraints"))));
    m_updating = false;
}

void ParametersPanel::showConstraint(const QString &uid)
{
    m_updating = true;
    m_dims->clearSelection();
    m_cons->clearSelection();
    for (int r = 0; r < m_dims->rowCount(); ++r)
        if (m_dims->item(r, 0) && m_dims->item(r, 0)->data(Qt::UserRole).toString() == uid)
            m_dims->selectRow(r);
    for (int r = 0; r < m_cons->count(); ++r)
        if (m_cons->item(r)->data(Qt::UserRole).toString() == uid)
            m_cons->setCurrentRow(r);
    m_delCon->setEnabled(m_cons->currentItem() != nullptr);
    m_updating = false;
}

void ParametersPanel::addParameter()
{
    if (!m_doc)
        return;
    Parametric after = m_doc->parametric();
    QString name;
    for (int i = 1;; ++i) {
        name = QStringLiteral("param%1").arg(i);
        if (!after.nameInUse(name))
            break;
    }
    after.params.append({name, QStringLiteral("10"), QString()});
    m_canvas->applyParametric(after, tr("add parameter %1").arg(name), true);
    const int row = kBuiltins + after.params.size() - 1;
    if (row < m_params->rowCount()) {
        m_params->setCurrentCell(row, 0);
        m_params->editItem(m_params->item(row, 0));
    }
}

void ParametersPanel::removeParameter()
{
    if (!m_doc)
        return;
    const int i = m_params->currentRow() - kBuiltins;
    Parametric after = m_doc->parametric();
    if (i < 0 || i >= after.params.size())
        return;
    const QString name = after.params.at(i).name;
    after.params.removeAt(i);
    m_canvas->applyParametric(after, tr("remove parameter %1").arg(name), true);
    // What used it now shows "error" until it is given something else.
    QStringList users;
    for (const UserParameter &p : after.params)
        if (param::namesIn(p.expr).contains(name)) users << p.name;
    for (const Constraint &c : after.constraints)
        if (c.isDimension() && param::namesIn(c.expr).contains(name)) users << c.name;
    if (!users.isEmpty())
        emit message(tr("%1 removed — still used by %2").arg(name, users.join(QStringLiteral(", "))));
}

bool ParametersPanel::rename(const QString &from, const QString &to)
{
    if (!param::isValidName(to)) {
        emit message(tr("“%1” can't be a name: start with a letter, then letters, digits or _ "
                        "(and not a function, unit or stock_*)").arg(to));
        return false;
    }
    if (m_doc->parametric().nameInUse(to)) {
        emit message(tr("%1 is already in use").arg(to));
        return false;
    }
    Parametric after = m_doc->parametric();
    after.renameEverywhere(from, to);
    m_canvas->applyParametric(after, tr("rename %1 to %2").arg(from, to), true, {},
                              [from, to](Document &d) {
        for (Toolpath &t : d.toolpathsRef()) {
            QJsonObject binds = t.json.value(QStringLiteral("phobic_expr")).toObject();
            bool any = false;
            for (auto it = binds.begin(); it != binds.end(); ++it) {
                QJsonObject b = it.value().toObject();
                const QString e = param::renameIn(b.value("expr").toString(), from, to);
                if (e != b.value("expr").toString()) {
                    b.insert("expr", e);
                    it.value() = b;
                    any = true;
                }
            }
            if (any)
                t.json.insert(QStringLiteral("phobic_expr"), binds);
        }
    });
    return true;
}

void ParametersPanel::paramEdited(QTableWidgetItem *it)
{
    if (m_updating || !m_doc)
        return;
    const int i = it->row() - kBuiltins;
    if (i < 0 || i >= m_doc->parametric().params.size())
        return;
    const UserParameter up = m_doc->parametric().params.at(i);
    const QString text = it->text().trimmed();
    bool done = false;
    if (it->column() == 0) {
        done = text == up.name || rename(up.name, text);
    } else if (it->column() == 1) {
        if (text == up.expr)
            return;
        Parametric after = m_doc->parametric();
        after.params[i].expr = text;
        if (const QString e = param::newError(*m_doc, m_doc->parametric(), after); !e.isEmpty()) {
            emit message(tr("%1 = %2: %3").arg(up.name, text, e));
            refresh();
            return;
        }
        // Like a dimension typed on the canvas: a value the constraints
        // can't reach is refused rather than half-applied.
        done = m_canvas->applyParametric(after, tr("%1 = %2").arg(up.name, text));
        emit message(done ? tr("%1 = %2").arg(up.name, text)
                          : tr("%1 = %2 can't be reached with the constraints").arg(up.name, text));
    } else if (it->column() == 3) {
        Parametric after = m_doc->parametric();
        after.params[i].comment = it->text();
        done = m_canvas->applyParametric(after, tr("comment on %1").arg(up.name), true);
    }
    if (!done)
        refresh();
}

void ParametersPanel::dimensionEdited(QTableWidgetItem *it)
{
    if (m_updating || !m_doc)
        return;
    const QString uid = m_dims->item(it->row(), 0)->data(Qt::UserRole).toString();
    const int i = m_doc->parametric().indexOfUid(uid);
    if (i < 0)
        return;
    const Constraint c = m_doc->parametric().constraints.at(i);
    const QString text = it->text().trimmed();
    bool done = false;
    if (it->column() == 0) {
        done = text == c.name || rename(c.name, text);
    } else if (it->column() == 2) {
        double v;
        QString why;
        Parametric after = m_doc->parametric();
        after.constraints[i].expr = text;
        if (param::namesIn(text).contains(c.name)) {
            why = tr("can't be defined by itself");
        } else if (!param::evaluateIn(*m_doc, text, &v, &why)) {
            // why says it
        } else if (const QString e = param::newError(*m_doc, m_doc->parametric(), after); !e.isEmpty()) {
            why = e;
        } else {
            done = text == c.expr || m_canvas->applyParametric(after, tr("%1 = %2").arg(c.name, text));
            if (!done)
                why = tr("can't be reached with the other constraints");
        }
        if (!why.isEmpty())
            emit message(tr("%1 = %2: %3").arg(c.name, text, why));
    }
    if (!done)
        refresh();
}

} // namespace c2d
