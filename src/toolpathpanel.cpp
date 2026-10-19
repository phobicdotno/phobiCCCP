#include "toolpathpanel.h"
#include "toolpathform.h"
#include <functional>
#include <cmath>
#include <QPainter>
#include <QDropEvent>
#include <QMouseEvent>
#include "tooldiagram.h"
#include "settingsdialog.h"
#include <QApplication>
#include "canvas.h"
#include "c2ddocument.h"
#include "toollibrary.h"
#include "toollibrarydialog.h"
#include "toolpathcommands.h"
#include "toolpathfactory.h"

#include <QHBoxLayout>
#include <QFontInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QFont>
#include <QMenu>
#include <QSet>
#include <QStyledItemDelegate>
#include <QSettings>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>
#include <QUuid>
#include <QVBoxLayout>

namespace c2d {


namespace {

enum Role {
    UuidRole = Qt::UserRole,
    SubtitleRole = Qt::UserRole + 1,
    ToolIconRole = Qt::UserRole + 2,
    ToolTextRole = Qt::UserRole + 3,
    UsedRole = Qt::UserRole + 4,
};

// One toolpath as a card: on/off box, type icon, name over "type · vectors ·
// depth", and the tool (outline + Ø) at the end. Click the box to switch the
// toolpath on or off; drag the card to change the cutting order.
class CardDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    static QRect boxRect(const QRect &card)
    {
        const int s = qMin(26, card.height() - 16);
        return QRect(card.left() + 10, card.center().y() - s / 2, s, s);
    }

    QSize sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &) const override
    {
        const QFontMetrics fm(opt.font);
        return QSize(200, fm.height() * 2 + 22);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &ix) const override
    {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRect card = opt.rect.adjusted(2, 3, -2, -3);
        const bool sel = opt.state & QStyle::State_Selected;
        const bool cur = opt.state & QStyle::State_HasFocus;
        const bool on = ix.data(Qt::CheckStateRole).toInt() == Qt::Checked;
        const bool used = ix.data(UsedRole).toBool();
        QColor bg = sel ? QColor(0x3a, 0x31, 0x22) : QColor(0x23, 0x26, 0x2e);
        if (!sel && (opt.state & QStyle::State_MouseOver))
            bg = QColor(0x2a, 0x2e, 0x36);
        p->setPen(QPen(sel || cur ? QColor(0xf2, 0xa5, 0x36) : used ? QColor(0x4a, 0x90, 0xd9)
                                                               : QColor(0x2f, 0x34, 0x3d),
                       sel || used ? 2 : 1));
        p->setBrush(bg);
        p->drawRoundedRect(QRectF(card).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);

        // on/off box
        const QRect box = boxRect(card);
        p->setPen(QPen(on ? QColor(0xf2, 0xa5, 0x36) : QColor(0x6a, 0x71, 0x7d), 1.6));
        p->setBrush(on ? QColor(0xf2, 0xa5, 0x36) : Qt::transparent);
        p->drawRoundedRect(QRectF(box), 5, 5);
        if (on) {
            p->setPen(QPen(QColor(0x16, 0x18, 0x1d), 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const QRectF b(box);
            QPolygonF tick;
            tick << QPointF(b.left() + b.width() * 0.25, b.center().y())
                 << QPointF(b.left() + b.width() * 0.43, b.bottom() - b.height() * 0.27)
                 << QPointF(b.right() - b.width() * 0.22, b.top() + b.height() * 0.28);
            p->drawPolyline(tick);
        }

        // type icon
        const int isz = qMin(30, card.height() - 12);
        const QRect typeR(box.right() + 10, card.center().y() - isz / 2, isz, isz);
        const QIcon typeIcon = ix.data(Qt::DecorationRole).value<QIcon>();
        if (!typeIcon.isNull())
            typeIcon.paint(p, typeR, Qt::AlignCenter, on ? QIcon::Normal : QIcon::Disabled);

        // tool at the end
        const QFontMetrics fm(opt.font);
        const QString toolText = ix.data(ToolTextRole).toString();
        const int tw = fm.horizontalAdvance(toolText);
        const int th = card.height() - 14;
        const QRect toolTextR(card.right() - 10 - tw, card.top(), tw, card.height());
        const QRect toolIconR(toolTextR.left() - 6 - th * 2 / 3, card.top() + 7, th * 2 / 3, th);
        const QIcon toolIcon = ix.data(ToolIconRole).value<QIcon>();
        if (!toolIcon.isNull())
            toolIcon.paint(p, toolIconR, Qt::AlignCenter);
        p->setPen(QColor(0xaa, 0xb1, 0xbd));
        p->drawText(toolTextR, Qt::AlignVCenter | Qt::AlignRight, toolText);

        // name over subtitle
        const int x = typeR.right() + 10;
        const QRect textR(x, card.top() + 6, toolIconR.left() - 8 - x, card.height() - 12);
        QFont nf = opt.font;
        nf.setBold(true);
        p->setFont(nf);
        p->setPen(on ? QColor(0xe6, 0xe9, 0xef) : QColor(0x6a, 0x71, 0x7d));
        const QRect nameR(textR.left(), textR.top(), textR.width(), textR.height() / 2);
        p->drawText(nameR, Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(nf).elidedText(ix.data(Qt::DisplayRole).toString(), Qt::ElideRight, nameR.width()));
        QFont sf = opt.font;
        sf.setPointSizeF(sf.pointSizeF() > 0 ? sf.pointSizeF() * 0.85 : 9);
        if (sf.pixelSize() > 0)
            sf.setPixelSize(qMax(9, int(opt.font.pixelSize() * 0.85)));
        p->setFont(sf);
        p->setPen(QColor(0x8b, 0x93, 0xa1));
        const QRect subR(textR.left(), textR.top() + textR.height() / 2, textR.width(), textR.height() / 2);
        p->drawText(subR, Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(sf).elidedText(ix.data(SubtitleRole).toString(), Qt::ElideRight, subR.width()));
        p->restore();
    }

    bool editorEvent(QEvent *e, QAbstractItemModel *model, const QStyleOptionViewItem &opt,
                     const QModelIndex &ix) override
    {
        if (e->type() == QEvent::MouseButtonRelease) {
            auto *me = static_cast<QMouseEvent *>(e);
            if (me->button() == Qt::LeftButton
                && boxRect(opt.rect.adjusted(2, 3, -2, -3)).adjusted(-6, -6, 6, 6).contains(me->position().toPoint())) {
                const bool on = ix.data(Qt::CheckStateRole).toInt() == Qt::Checked;
                model->setData(ix, on ? Qt::Unchecked : Qt::Checked, Qt::CheckStateRole);
                return true;
            }
        }
        return QStyledItemDelegate::editorEvent(e, model, opt, ix);
    }

    QWidget *createEditor(QWidget *, const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        return nullptr;   // the name is edited in the settings below
    }
};

// The list with drag-and-drop reordering: a dropped card reports where it
// landed, and the panel turns that into one undoable move.
class CardList : public QTreeWidget
{
public:
    std::function<void(const QString &uuid, int to)> moved;
    using QTreeWidget::QTreeWidget;

protected:
    void dropEvent(QDropEvent *e) override
    {
        QTreeWidgetItem *dragged = currentItem();
        const QString uuid = dragged ? dragged->data(0, UuidRole).toString() : QString();
        QTreeWidget::dropEvent(e);
        if (uuid.isEmpty() || !moved)
            return;
        for (int i = 0; i < topLevelItemCount(); ++i)
            if (topLevelItem(i)->data(0, UuidRole).toString() == uuid) {
                moved(uuid, i);
                return;
            }
    }
};

} // namespace

static QToolButton *smallButton(QWidget *parent, const QString &text, const QString &tip)
{
    auto *b = new QToolButton(parent);
    b->setText(text);
    b->setToolTip(tip);
    b->setAutoRaise(true);
    return b;
}

ToolpathPanel::ToolpathPanel(Canvas *canvas, QWidget *parent)
    : QWidget(parent), m_canvas(canvas)
{
    applyTextSize();
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->setSpacing(6);

    // Lifecycle bar: New ▾ | Duplicate | Delete | ▲ | ▼
    auto *bar = new QHBoxLayout;
    bar->setSpacing(2);
    auto *newBtn = new QToolButton(this);
    newBtn->setText(QStringLiteral("New ▾"));
    newBtn->setPopupMode(QToolButton::InstantPopup);
    newBtn->setToolTip(QStringLiteral(
        "Create a toolpath of this type for the vectors selected on the canvas\n"
        "(Carbide Create's defaults, the last-used tool, added last in the group)"));
    auto *newMenu = new QMenu(newBtn);
    for (const ToolpathKind &k : toolpathKinds()) {
        QAction *a = newMenu->addAction(k.label);
        if (k.type == QLatin1String("engrave_toolpath")) {
            newMenu->insertSeparator(a);
            a->setToolTip(QStringLiteral(
                "Engraving (outline / hatch fill) — phobiCCCP-only type; "
                "Carbide Create does not read it"));
        } else if (k.type == QLatin1String("face_toolpath")) {
            a->setToolTip(QStringLiteral(
                "Face the stock (or the box around the selected vectors) flat — "
                "phobiCCCP-only type; Carbide Create does not read it"));
        } else if (k.type == QLatin1String("bore_toolpath")) {
            a->setToolTip(QStringLiteral(
                "Helical bore down the wall of each selected circle — "
                "phobiCCCP-only type; Carbide Create does not read it"));
        } else if (k.type == QLatin1String("chamfer_toolpath")) {
            a->setToolTip(QStringLiteral(
                "V-bit chamfer along the selected edges — "
                "phobiCCCP-only type; Carbide Create does not read it"));
        } else if (k.type == QLatin1String("adaptive_toolpath")) {
            a->setToolTip(QStringLiteral(
                "Adaptive clearing: pocket the selected shapes with a light, "
                "steady bite at a deep stepdown — "
                "phobiCCCP-only type; Carbide Create does not read it"));
        } else if (k.type == QLatin1String("thread_toolpath")) {
            a->setToolTip(QStringLiteral(
                "Thread milling: a helix of one pitch per turn around each selected "
                "circle (the thread's major diameter), inside or outside — "
                "phobiCCCP-only type; Carbide Create does not read it"));
        } else if (k.type == QLatin1String("slot_toolpath")) {
            a->setToolTip(QStringLiteral(
                "2D Slot: ramp down the centreline of each selected slot (or open "
                "vector), cleaning the walls when the slot is wider than the tool — "
                "phobiCCCP-only type; Carbide Create does not read it"));
        } else if (k.type == QLatin1String("circular_toolpath")) {
            a->setToolTip(QStringLiteral(
                "Circular: clear a round pocket, or a boss, on each selected circle "
                "in full laps — phobiCCCP-only type; Carbide Create does not read it"));
        }
        const QString type = k.type;
        connect(a, &QAction::triggered, this, [this, type] { newToolpath(type); });
    }
    newBtn->setMenu(newMenu);
    bar->addWidget(newBtn);
    m_dupBtn = smallButton(this, QStringLiteral("Duplicate"),
                           QStringLiteral("Copy the selected toolpath (placed right after it)"));
    connect(m_dupBtn, &QToolButton::clicked, this, &ToolpathPanel::duplicateCurrent);
    bar->addWidget(m_dupBtn);
    m_delBtn = smallButton(this, QStringLiteral("Delete"),
                           QStringLiteral("Delete the selected toolpath (undoable)"));
    connect(m_delBtn, &QToolButton::clicked, this, &ToolpathPanel::deleteCurrent);
    bar->addWidget(m_delBtn);
    bar->addStretch(1);
    m_upBtn = smallButton(this, QStringLiteral("▲"),
                          QStringLiteral("Move up — toolpaths are machined top to bottom"));
    connect(m_upBtn, &QToolButton::clicked, this, [this] { moveCurrent(-1); });
    bar->addWidget(m_upBtn);
    m_downBtn = smallButton(this, QStringLiteral("▼"), QStringLiteral("Move down"));
    connect(m_downBtn, &QToolButton::clicked, this, [this] { moveCurrent(+1); });
    bar->addWidget(m_downBtn);
    lay->addLayout(bar);

    auto *cards = new CardList(this);
    m_list = cards;
    m_list->setColumnCount(1);
    m_list->setHeaderHidden(true);
    m_list->setRootIsDecorated(false);
    m_list->setIndentation(0);
    m_list->setUniformRowHeights(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setItemDelegate(new CardDelegate(m_list));
    m_list->setMouseTracking(true);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setStyleSheet(QStringLiteral("QTreeWidget { background: transparent; }"
                                         "QTreeWidget::item { background: transparent; }"));
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setDragEnabled(true);
    m_list->setAcceptDrops(true);
    m_list->setDropIndicatorShown(true);
    m_list->setDragDropMode(QAbstractItemView::InternalMove);
    m_list->setDefaultDropAction(Qt::MoveAction);
    m_list->setMinimumHeight(60);
    m_list->setToolTip(QStringLiteral(
        "Cutting order, top to bottom: drag a card to move it. "
        "The box switches a toolpath on or off."));
    cards->moved = [this](const QString &uuid, int to) {
        // Queued: the move ends in refresh(), which rebuilds this list while
        // the view is still finishing the drop.
        QMetaObject::invokeMethod(this, [this, uuid, to] {
            if (!m_doc)
                return;
            const int from = m_doc->toolpathIndex(uuid);
            if (from < 0 || from == to) {
                refresh();
                return;
            }
            m_uuid = uuid;
            m_canvas->undoStack()->push(new TpMoveCmd(m_canvas, m_doc, uuid, from, to));
        }, Qt::QueuedConnection);
    };
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    lay->addWidget(m_list);
    connect(m_list, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *, QTreeWidgetItem *) { onCurrentChanged(); });
    connect(m_list, &QTreeWidget::itemChanged, this, &ToolpathPanel::onItemChanged);
    // The halo follows the list's *selection*, not its current row, so that
    // clicking the same row again after the canvas took the selection brings
    // the halo back (currentItemChanged would not fire).
    connect(m_list, &QTreeWidget::itemSelectionChanged, this,
            &ToolpathPanel::updateHighlight);
    // Reverse lookup: picking a shape on the canvas bolds the toolpaths that
    // machine it, and hands the selection over — the toolpath row deselects
    // and its halo clears, so the two panes never both claim to be "the
    // selection". The row stays *current*, so the parameter table and
    // "Assign selected vectors" still act on the toolpath you were editing.
    connect(m_canvas, &Canvas::selectionChangedIds, this,
            [this](const QStringList &ids) {
                if (!ids.isEmpty())
                    m_list->clearSelection();
                markUsage(ids);
            });
    connect(m_list, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &p) {
        if (!m_list->itemAt(p) || m_uuid.isEmpty())
            return;
        QMenu menu(this);
        menu.addAction(QStringLiteral("Rename…"), this, &ToolpathPanel::renameCurrent);
        menu.addAction(QStringLiteral("Duplicate"), this, &ToolpathPanel::duplicateCurrent);
        menu.addAction(QStringLiteral("Delete"), this, &ToolpathPanel::deleteCurrent);
        menu.addSeparator();
        menu.addAction(QStringLiteral("Move up"), this, [this] { moveCurrent(-1); });
        menu.addAction(QStringLiteral("Move down"), this, [this] { moveCurrent(+1); });
        menu.exec(m_list->viewport()->mapToGlobal(p));
    });

    m_hint = new QLabel(this);
    m_hint->setWordWrap(true);
    m_hint->setStyleSheet(QStringLiteral("color: #a0522d;"));
    m_hint->hide();
    lay->addWidget(m_hint);

    m_form = new ToolpathForm(this);
    lay->addWidget(m_form, 1);
    connect(m_form, &ToolpathForm::edited, this, [this](const QJsonObject &j) {
        // Queued: editToolpath ends in refresh(), which updates this very form
        // while it is still inside the editor's signal.
        const QString uuid = m_uuid;
        if (uuid.isEmpty())
            return;
        QMetaObject::invokeMethod(this, [this, uuid, j] { m_canvas->editToolpath(uuid, j); },
                                  Qt::QueuedConnection);
    });
    connect(m_form, &ToolpathForm::changeToolRequested, this, [this] { pickTool(); });

    // Replace the toolpath's vector list with the canvas selection.
    auto *assign = new QPushButton(QStringLiteral("Assign selected vectors"), this);
    assign->setToolTip(QStringLiteral(
        "Set this toolpath's vectors to the elements selected on the canvas"));
    lay->addWidget(assign);
    connect(assign, &QPushButton::clicked, this, [this] {
        if (!m_doc || m_uuid.isEmpty())
            return;
        Toolpath *t = m_doc->toolpathByUuid(m_uuid);
        const QStringList ids = m_canvas->selectedElementIds();
        if (!t || ids.isEmpty())
            return;
        QJsonArray arr;
        for (const QString &id : ids) {
            QJsonObject o;
            o.insert(QStringLiteral("uuid"), id);
            arr.append(o);
        }
        QJsonObject j = t->json;
        j.insert(QStringLiteral("elements"), arr);
        m_canvas->editToolpath(m_uuid, j);
    });

    // V-carve inlay: generate the mirrored male from the selected female.
    auto *inlay = new QPushButton(QStringLiteral("Create inlay male"), this);
    inlay->setToolTip(QStringLiteral(
        "From the selected advanced v-carve (the female): place a mirrored copy\n"
        "of its design beside it inside a border, and add a v-carve of the\n"
        "inverse region with the depths shifted by the glue gap. Carve it in a\n"
        "second piece, flip it over into the female, glue, then plane flush."));
    lay->addWidget(inlay);
    connect(inlay, &QPushButton::clicked, this, [this] { createInlayMale(); });

    updateButtons();
}

// The panel's text, larger than the rest of the app (read and edited at arm's
// length on the shop screen). A plain setFont() is overridden once the app
// stylesheet polishes the children, so the size goes in a stylesheet.
void ToolpathPanel::setTypeIcons(std::function<QIcon(const QString &)> f)
{
    m_typeIcon = std::move(f);
    refresh();
}

void ToolpathPanel::applyTextSize()
{
    QSettings st;
    const int pct = appsettings::panelTextPercent(st);
    const int base = QFontInfo(QApplication::font()).pixelSize();
    setStyleSheet(QStringLiteral("QWidget { font-size: %1px; }").arg(qRound(base * pct / 100.0)));
}

// ---- lifecycle -----------------------------------------------------------

void ToolpathPanel::newToolpath(const QString &type)
{
    if (!m_doc)
        return;
    const QStringList ids = m_canvas->selectedElementIds();
    const Toolpath t = makeToolpath(*m_doc, type, ids);
    m_uuid = t.uuid;   // refresh() (from documentChanged) selects it
    m_canvas->undoStack()->push(
        new TpInsertCmd(m_canvas, m_doc, t, m_doc->toolpaths().size(), QStringLiteral("new")));
    if (ids.isEmpty())
        m_hint->setText(QStringLiteral(
            "\"%1\" has no vectors yet: select shapes on the canvas and press "
            "\"Assign selected vectors\".").arg(t.json.value("name").toString()));
    else
        m_hint->clear();
    m_hint->setVisible(!m_hint->text().isEmpty());
}

void ToolpathPanel::duplicateCurrent()
{
    if (!m_doc || m_uuid.isEmpty())
        return;
    const Toolpath *t = m_doc->toolpathByUuid(m_uuid);
    if (!t)
        return;
    const Toolpath copy = duplicateToolpath(*t);
    const int index = m_doc->toolpathIndex(m_uuid) + 1;
    m_uuid = copy.uuid;
    m_canvas->undoStack()->push(
        new TpInsertCmd(m_canvas, m_doc, copy, index, QStringLiteral("duplicate")));
}

void ToolpathPanel::deleteCurrent()
{
    if (!m_doc || m_uuid.isEmpty())
        return;
    const Toolpath *t = m_doc->toolpathByUuid(m_uuid);
    if (!t)
        return;
    // Keep the selection on a neighbour so the editor does not go blank.
    const int index = m_doc->toolpathIndex(m_uuid);
    const int n = m_doc->toolpaths().size();
    const int next = index + 1 < n ? index + 1 : index - 1;
    const QString nextUuid = next >= 0 ? m_doc->toolpaths().at(next).uuid : QString();
    const Toolpath gone = *t;
    m_uuid = nextUuid;
    m_canvas->undoStack()->push(new TpRemoveCmd(m_canvas, m_doc, gone));
}

void ToolpathPanel::renameCurrent()
{
    if (!m_doc || m_uuid.isEmpty())
        return;
    const QString uuid = m_uuid;
    const Toolpath *t = m_doc->toolpathByUuid(uuid);
    if (!t)
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("Rename toolpath"), QStringLiteral("Name:"),
        QLineEdit::Normal, t->json.value("name").toString(), &ok);
    // The dialog runs the event loop: a pad may have replaced the design.
    t = m_doc->toolpathByUuid(uuid);
    if (!ok || name.trimmed().isEmpty() || !t)
        return;
    QJsonObject j = t->json;
    j.insert(QStringLiteral("name"), name.trimmed());
    m_canvas->editToolpath(uuid, j);
}

void ToolpathPanel::moveCurrent(int delta)
{
    if (!m_doc || m_uuid.isEmpty())
        return;
    const int from = m_doc->toolpathIndex(m_uuid);
    const int to = from + delta;
    if (from < 0 || to < 0 || to >= m_doc->toolpaths().size())
        return;
    m_canvas->undoStack()->push(new TpMoveCmd(m_canvas, m_doc, m_uuid, from, to));
}

void ToolpathPanel::updateButtons()
{
    const bool has = m_doc && !m_uuid.isEmpty();
    const int index = has ? m_doc->toolpathIndex(m_uuid) : -1;
    m_dupBtn->setEnabled(has);
    m_delBtn->setEnabled(has);
    m_upBtn->setEnabled(has && index > 0);
    m_downBtn->setEnabled(has && index >= 0 && index + 1 < m_doc->toolpaths().size());
}

// ---- tool library / inlay ------------------------------------------------

void ToolpathPanel::pickTool()
{
    if (!m_doc || m_uuid.isEmpty())
        return;
    const QString uuid = m_uuid;
    const Toolpath *t = m_doc->toolpathByUuid(uuid);
    if (!t)
        return;
    ToolLibrary lib;
    QString err;
    if (!lib.loadDefault(&err)) {
        QMessageBox::warning(this, QStringLiteral("Tool library"), err);
        return;
    }
    QString mat = lib.materialIdForCC(m_doc->params().value("material"));
    if (mat.isEmpty())
        mat = QStringLiteral("softwood");
    const int cur = int(t->json.value("tool").toObject().value("number").toDouble());
    ToolLibraryDialog dlg(lib, mat, cur, this);
    if (dlg.exec() != QDialog::Accepted || !dlg.selectedTool())
        return;
    // The dialog runs the event loop: a pad may have replaced the design.
    t = m_doc->toolpathByUuid(uuid);
    if (!t)
        return;
    const ToolFeeds f = dlg.selectedFeeds();
    if (!f.valid()) {
        QMessageBox::information(this, QStringLiteral("Tool library"),
            QStringLiteral("%1 has no feeds and speeds for %2.")
                .arg(dlg.selectedTool()->name, lib.materialName(dlg.selectedMaterialId())));
        return;
    }
    QJsonObject j = t->json;
    ToolLibrary::applyToToolpath(j, *dlg.selectedTool(), f);
    m_canvas->editToolpath(uuid, j);
}

void ToolpathPanel::createInlayMale()
{
    if (!m_doc || m_uuid.isEmpty())
        return;
    Toolpath *t = m_doc->toolpathByUuid(m_uuid);
    if (!t || t->type != QLatin1String("advanced_vcarve_toolpath")) {
        QMessageBox::information(this, QStringLiteral("Inlay male"),
            QStringLiteral("Select an advanced v-carve toolpath first — the male "
                           "is generated from the female's design."));
        return;
    }
    // Copies: the questions below run the event loop, and a pad may replace
    // the design meanwhile.
    const Toolpath female = *t;
    QVector<QPainterPath> els;
    for (const QJsonValue &v : t->json.value("elements").toArray())
        if (const Element *e = m_doc->elementById(v.toObject().value("uuid").toString()))
            els.append(e->painterPath);
    if (els.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Inlay male"),
                                 QStringLiteral("The toolpath references no vectors."));
        return;
    }
    bool ok = false;
    const double gap = QInputDialog::getDouble(
        this, QStringLiteral("Inlay male"),
        QStringLiteral("Glue gap (mm): both male depths are shifted down by\n"
                       "this much, so the assembled male sits proud by the gap\n"
                       "(glue space below, planed flush after glue-up)"),
        0.5, 0.0, 5.0, 2, &ok);
    if (!ok)
        return;
    const double margin = QInputDialog::getDouble(
        this, QStringLiteral("Inlay male"),
        QStringLiteral("Border margin around the mirrored design (mm)"),
        10.0, 2.0, 100.0, 1, &ok);
    if (!ok || !m_doc->toolpathByUuid(female.uuid))
        return;

    QRectF bb;
    for (const QPainterPath &e : els)
        bb = bb.united(e.boundingRect());
    const double cx = bb.center().x();
    const double dx = bb.width() + 2 * margin + 20.0;   // male sits to the right
    const QJsonObject layer = m_doc->defaultLayer();

    QVector<Element> newEls;
    QJsonArray refs;
    const Element border = Element::makeRectangle(
        QPointF(cx + dx, bb.center().y()),
        bb.width() + 2 * margin, bb.height() + 2 * margin, layer);
    newEls.append(border);
    refs.append(QJsonObject{{QStringLiteral("uuid"), border.id}});
    // Mirror every subpath about the design's vertical centerline, then shift
    // beside the original. Border + mirrored design under even-odd fill = the
    // inverse region: the male carves the field, leaving the design standing.
    for (const QPainterPath &e : els) {
        const QList<QPolygonF> polys = e.toSubpathPolygons();
        for (const QPolygonF &p : polys) {
            const bool closed = p.size() > 3 && p.first() == p.last();
            const int n = closed ? int(p.size()) - 1 : int(p.size());
            if (n < 2)
                continue;
            QVector<QPointF> pts;
            pts.reserve(n);
            for (int i = n - 1; i >= 0; --i)   // reversed: mirroring flips winding
                pts.append(QPointF(2 * cx - p.at(i).x() + dx, p.at(i).y()));
            const Element pe = Element::makePath(pts, closed, layer);
            newEls.append(pe);
            refs.append(QJsonObject{{QStringLiteral("uuid"), pe.id}});
        }
    }

    Toolpath male = female;
    QJsonObject j = male.json;
    j.insert(QStringLiteral("uuid"), QUuid::createUuid().toString());
    j.insert(QStringLiteral("name"),
             j.value("name").toString() + QStringLiteral(" (inlay male)"));
    j.insert(QStringLiteral("elements"), refs);
    auto num = [](const QJsonValue &v) {
        return v.isString() ? v.toString().toDouble() : v.toDouble();
    };
    // Preserve the file's depth sign convention and value type (string/number).
    const double sign = num(j.value("end_depth")) < 0 ? -1.0 : 1.0;
    auto setDepth = [&](const char *key, double absVal) {
        const QJsonValue orig = j.value(QLatin1String(key));
        if (orig.isString())
            j.insert(QLatin1String(key), QString::number(sign * absVal, 'f', 3));
        else
            j.insert(QLatin1String(key), sign * absVal);
    };
    setDepth("start_depth", qAbs(num(j.value("start_depth"))) + gap);
    setDepth("end_depth", qAbs(num(j.value("end_depth"))) + gap);
    male.json = j;
    male.uuid = j.value("uuid").toString();
    m_canvas->insertGenerated(newEls, male);
    QMessageBox::information(this, QStringLiteral("Inlay male"),
        QStringLiteral("Generated \"%1\": mirrored design in a %2 mm border, "
                       "depths shifted %3 mm.\n\nCarve it in the second piece, "
                       "flip that piece face-down into the female, glue, then "
                       "plane the ~%3 mm proud surface flush.")
            .arg(j.value("name").toString())
            .arg(margin, 0, 'f', 1)
            .arg(gap, 0, 'f', 2));
}

// ---- list / editor -------------------------------------------------------

void ToolpathPanel::setDocument(Document *doc)
{
    m_doc = doc;
    m_form->setDocument(doc);
    m_uuid.clear();
    m_hint->hide();
    refresh();
}

void ToolpathPanel::refresh()
{
    m_loading = true;
    const QString keep = m_uuid;
    m_list->clear();
    QTreeWidgetItem *keepItem = nullptr;
    if (m_doc) {
        for (const Toolpath &t : m_doc->toolpaths()) {
            auto *it = new QTreeWidgetItem(m_list);
            it->setText(0, t.json.value("name").toString());
            it->setCheckState(0, t.json.value("enabled").toBool(true) ? Qt::Checked
                                                                       : Qt::Unchecked);
            it->setData(0, UuidRole, t.uuid);
            if (m_typeIcon)
                it->setData(0, Qt::DecorationRole, m_typeIcon(t.type));
            const QJsonArray refs = t.json.value("elements").toArray();
            QString sub = QStringLiteral("%1 \u00b7 %2 vector%3").arg(toolpathLabel(t.type))
                              .arg(refs.size()).arg(refs.size() == 1 ? "" : "s");
            for (const char *k : {"end_depth", "cut_depth", "max_depth"}) {
                const QJsonValue d = t.json.value(QLatin1String(k));
                if (!d.isUndefined()) {
                    const double v = std::fabs(d.isString() ? d.toString().toDouble() : d.toDouble());
                    sub += QStringLiteral(" \u00b7 %1 mm deep").arg(v, 0, 'g', 4);
                    break;
                }
            }
            it->setData(0, SubtitleRole, sub);
            const QJsonObject tool = t.json.value("tool").toObject();
            if (!tool.isEmpty()) {
                LibraryTool lt;
                lt.diameter = tool.value("diameter").toDouble();
                lt.angle = tool.value("angle").toDouble();
                const int ty = int(tool.value("type").toDouble());
                lt.kind = ty == 1 ? QStringLiteral("ball")
                        : (ty == 2 || lt.angle > 0) ? QStringLiteral("vbit") : QStringLiteral("endmill");
                it->setData(0, ToolIconRole, toolKindIcon(lt));
                it->setData(0, ToolTextRole, QStringLiteral("\u00d8%1").arg(lt.diameter, 0, 'g', 4));
            }
            QStringList vids;
            for (const QJsonValue &v : refs)
                vids << v.toObject().value("uuid").toString();
            it->setToolTip(0, QStringLiteral("T%1  %2\n%3")
                                  .arg(int(tool.value("number").toDouble()))
                                  .arg(tool.value("name").toString(), describeVectors(vids)));
            it->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable
                         | Qt::ItemIsDragEnabled);
            if (t.uuid == keep)
                keepItem = it;
        }
    }
    m_loading = false;
    {
        const int n = qBound(1, m_list->topLevelItemCount(), 6);
        const int rowH = m_list->sizeHintForRow(0) > 0 ? m_list->sizeHintForRow(0)
                                                       : QFontMetrics(m_list->font()).height() * 2 + 22;
        m_list->setFixedHeight(n * rowH + 6);
    }
    if (keepItem)
        m_list->setCurrentItem(keepItem);
    else if (m_list->topLevelItemCount() > 0)
        m_list->setCurrentItem(m_list->topLevelItem(0));
    else
        showToolpath(QString());
    // setCurrentItem on an unchanged current item does not signal; re-sync.
    onCurrentChanged();
    markUsage(m_selectedElements);
}

void ToolpathPanel::onCurrentChanged()
{
    if (m_loading)
        return;
    const QTreeWidgetItem *it = m_list->currentItem();
    const QString uuid = it ? it->data(0, UuidRole).toString() : QString();
    showToolpath(uuid);
    updateHighlight();
}

// Outline the shapes of the selected toolpath, or nothing when the canvas has
// taken the selection over.
void ToolpathPanel::updateHighlight()
{
    const QList<QTreeWidgetItem *> sel = m_list->selectedItems();
    m_canvas->setVectorHighlight(
        sel.isEmpty() ? QStringList()
                      : vectorIdsOf(sel.first()->data(0, UuidRole).toString()));
}

// ---- which toolpath cuts which shape --------------------------------------

QStringList ToolpathPanel::vectorIdsOf(const QString &uuid) const
{
    QStringList ids;
    if (!m_doc || uuid.isEmpty())
        return ids;
    const Toolpath *t = m_doc->toolpathByUuid(uuid);
    if (!t)
        return ids;
    for (const QJsonValue &v : t->json.value("elements").toArray()) {
        const QString id = v.toObject().value("uuid").toString();
        if (!id.isEmpty())
            ids << id;
    }
    return ids;
}

// A human summary of a vector list: "2 x circle, 1 x rectangle". Ids that no
// longer resolve are counted as missing, which is worth seeing — it means the
// toolpath refers to a shape that has since been deleted.
QString ToolpathPanel::describeVectors(const QStringList &ids) const
{
    if (ids.isEmpty())
        return QStringLiteral("No vectors assigned — select shapes on the canvas "
                              "and press \"Assign selected vectors\".");
    QStringList order;
    QHash<QString, int> hist;
    int missing = 0;
    for (const QString &id : ids) {
        const Element *e = m_doc ? m_doc->elementById(id) : nullptr;
        if (!e) {
            ++missing;
            continue;
        }
        QString k = e->geometryType;
        if (k.isEmpty())
            k = QStringLiteral("shape");
        if (!hist.contains(k))
            order << k;
        hist[k]++;
    }
    QStringList parts;
    for (const QString &k : std::as_const(order))
        parts << (hist.value(k) == 1 ? k : QStringLiteral("%1 x %2").arg(hist.value(k)).arg(k));
    if (missing > 0)
        parts << QStringLiteral("%1 missing").arg(missing);
    return QStringLiteral("Cuts: ") + parts.join(QStringLiteral(", "))
           + QStringLiteral("\nSelect this row to outline them on the canvas.");
}

// Reverse lookup: bold every row whose vectors include something the user has
// selected on the canvas, so clicking a shape shows what will machine it.
void ToolpathPanel::markUsage(const QStringList &selectedElementIds)
{
    m_selectedElements = selectedElementIds;
    // Every setFont/setToolTip below emits itemChanged, and refresh() has
    // already cleared m_loading by the time it calls us - so simply selecting
    // a shape ran the user-edit slot once per row and left m_uuid pointing at
    // the LAST toolpath while row 0 was still highlighted. Delete then removed
    // the wrong toolpath, and parameter edits were written to it.
    const bool wasLoading = m_loading;
    m_loading = true;
    const QSet<QString> sel(selectedElementIds.begin(), selectedElementIds.end());
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        QTreeWidgetItem *it = m_list->topLevelItem(i);
        const QStringList ids = vectorIdsOf(it->data(0, UuidRole).toString());
        bool used = false;
        for (const QString &id : ids)
            if (sel.contains(id)) {
                used = true;
                break;
            }
        if (it->data(0, UsedRole).toBool() != used)
            it->setData(0, UsedRole, used);   // blue edge: machines the selected shape
    }
    m_loading = wasLoading;
}

void ToolpathPanel::onItemChanged(QTreeWidgetItem *item, int column)
{
    if (m_loading || column != 0 || !m_doc)
        return;
    const QString uuid = item->data(0, UuidRole).toString();
    Toolpath *t = m_doc->toolpathByUuid(uuid);
    if (!t)
        return;
    QJsonObject j = t->json;
    const bool enabled = item->checkState(0) == Qt::Checked;
    const QString name = item->text(0).trimmed();
    if (enabled != j.value("enabled").toBool(true))
        j.insert(QStringLiteral("enabled"), enabled);
    if (!name.isEmpty() && name != j.value("name").toString())
        j.insert(QStringLiteral("name"), name);
    // Only a change to the row the user is actually on may move the selection.
    if (item == m_list->currentItem())
        m_uuid = uuid;
    // editToolpath ends in documentChanged -> refresh(), which clears the tree
    // and destroys `item` (and, for a rename, its open editor) while the view
    // is still unwinding the change notification on it. Let the current event
    // finish first.
    QMetaObject::invokeMethod(this, [this, uuid, j] { m_canvas->editToolpath(uuid, j); },
                              Qt::QueuedConnection);
}

void ToolpathPanel::showToolpath(const QString &uuid)
{
    m_loading = true;
    m_uuid.clear();
    const Toolpath *t = m_doc ? m_doc->toolpathByUuid(uuid) : nullptr;
    if (t) {
        m_uuid = t->uuid;
        m_form->setToolpath(t->uuid, t->type, t->json);
        if (t->json.value("elements").toArray().isEmpty()) {
            m_hint->setText(QStringLiteral(
                "\"%1\" has no vectors: select shapes on the canvas and press "
                "\"Assign selected vectors\".").arg(t->json.value("name").toString()));
            m_hint->show();
        } else {
            m_hint->hide();
        }
    } else {
        m_form->clear();
    }
    m_loading = false;
    updateButtons();
}

} // namespace c2d
