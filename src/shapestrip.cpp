#include "shapestrip.h"
#include "c2ddocument.h"
#include "canvas.h"
#include <QAction>
#include <QDropEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>

namespace c2d {

namespace {
constexpr int kSide = 72;       // tile picture, px
constexpr int kMaxTiles = 400;  // a traced image can hold thousands of shapes

QPixmap tileFor(const Element &e, int index, double dpr, bool selected)
{
    QPixmap pm(QSize(kSide, kSide) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(selected ? QColor(0x3a, 0x2e, 0x16) : QColor(0x1b, 0x1f, 0x27));
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF bb = e.painterPath.boundingRect();
    const double span = qMax(qMax(bb.width(), bb.height()), 1e-3);
    const double k = (kSide - 18) / span;
    QTransform tr;   // mm, Y up -> pixels, Y down, centred
    tr.translate(kSide / 2.0, kSide / 2.0);
    tr.scale(k, -k);
    tr.translate(-bb.center().x(), -bb.center().y());
    const QPainterPath &path = e.painterPath;
    const bool closed = path.elementCount() > 2
        && QLineF(path.pointAtPercent(0), path.pointAtPercent(1)).length() < 1e-3;
    p.setPen(QPen(QColor(0xe6, 0xe9, 0xef), 1.4));
    p.setBrush(closed ? QColor(0xf2, 0xa5, 0x36, 0x40) : QColor(Qt::transparent));
    p.drawPath(tr.map(path));
    p.setPen(QColor(0x8a, 0x92, 0xa0));
    p.drawText(QRect(4, kSide - 15, kSide - 8, 12), Qt::AlignLeft | Qt::AlignVCenter,
               QString::number(index + 1));
    return pm;
}
}

ShapeStrip::ShapeStrip(Canvas *canvas, QWidget *parent)
    : QListWidget(parent), m_canvas(canvas)
{
    setViewMode(QListView::IconMode);
    setFlow(QListView::LeftToRight);
    setWrapping(true);
    setResizeMode(QListView::Adjust);
    setMovement(QListView::Snap);
    setIconSize(QSize(kSide, kSide));
    setGridSize(QSize(kSide + 12, kSide + 12));
    setSpacing(0);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setDragDropMode(QAbstractItemView::InternalMove);
    setDefaultDropAction(Qt::MoveAction);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setStyleSheet(QStringLiteral("QListWidget{background:#14171d;border:none;}"
                                 "QListWidget::item{border:1px solid #2a2f3a;border-radius:6px;}"
                                 "QListWidget::item:selected{border:2px solid #f2a536;}"));

    m_hold.setSingleShot(true);
    m_hold.setInterval(600);
    connect(&m_hold, &QTimer::timeout, this, [this] {
        if (const QListWidgetItem *it = itemAt(m_pressAt)) {
            if (!it->isSelected())
                setCurrentItem(const_cast<QListWidgetItem *>(it), QItemSelectionModel::ClearAndSelect);
            showMenu(m_pressAt);
        }
    });
    viewport()->installEventFilter(this);
    connect(this, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        m_hold.stop();
        if (itemAt(pos))
            showMenu(pos);
    });
    connect(this, &QListWidget::itemSelectionChanged, this, &ShapeStrip::toCanvas);
    connect(m_canvas, &Canvas::selectionChangedIds, this, &ShapeStrip::fromCanvas);
    connect(m_canvas, &Canvas::documentChanged, this, &ShapeStrip::reload);
    reload();
}

QStringList ShapeStrip::idsInOrder() const
{
    QStringList ids;
    for (int i = 0; i < count(); ++i)
        ids << item(i)->data(Qt::UserRole).toString();
    return ids;
}

QStringList ShapeStrip::selectedIds() const
{
    QStringList ids;
    for (const QListWidgetItem *it : selectedItems())
        ids << it->data(Qt::UserRole).toString();
    return ids;
}

void ShapeStrip::reload()
{
    if (m_busy)
        return;
    m_busy = true;
    const int scroll = verticalScrollBar()->value();
    const QStringList sel = m_canvas->selectedElementIds();
    clear();
    if (const Document *doc = m_canvas->document()) {
        const double dpr = devicePixelRatioF();
        const auto &els = doc->elements();
        for (int i = 0; i < els.size() && i < kMaxTiles; ++i) {
            const Element &e = els.at(i);
            auto *it = new QListWidgetItem(QIcon(tileFor(e, i, dpr, false)), QString(), this);
            it->setData(Qt::UserRole, e.id);
            const QRectF bb = e.painterPath.boundingRect();
            QString kind = e.geometryType.isEmpty() ? QStringLiteral("shape") : e.geometryType;
            kind[0] = kind.at(0).toUpper();
            it->setToolTip(QStringLiteral("%1 %2: %3 x %4 mm\nDrag to reorder. Right click or hold for Duplicate / Delete.")
                               .arg(i + 1).arg(kind).arg(bb.width(), 0, 'f', 1).arg(bb.height(), 0, 'f', 1));
            it->setSizeHint(QSize(kSide + 12, kSide + 12));
            if (sel.contains(e.id))
                it->setSelected(true);
        }
    }
    verticalScrollBar()->setValue(scroll);
    m_busy = false;
}

void ShapeStrip::fromCanvas(const QStringList &ids)
{
    if (m_busy)
        return;
    m_busy = true;
    clearSelection();
    for (int i = 0; i < count(); ++i)
        if (ids.contains(item(i)->data(Qt::UserRole).toString()))
            item(i)->setSelected(true);
    m_busy = false;
}

void ShapeStrip::toCanvas()
{
    if (m_busy)
        return;
    m_busy = true;
    m_canvas->selectElements(selectedIds());
    m_busy = false;
}

void ShapeStrip::dropEvent(QDropEvent *e)
{
    // The list moves the tile itself; the document follows, as one undo step.
    const QStringList before = [this] {
        QStringList ids;
        if (const Document *d = m_canvas->document())
            for (const Element &el : d->elements())
                ids << el.id;
        return ids;
    }();
    QListWidget::dropEvent(e);
    const QStringList after = idsInOrder();
    m_busy = true;
    if (after.size() == before.size() && after != before) {
        // One tile moved; the first place the orders differ is either where it
        // landed or where it left. Whichever reproduces the new order is it.
        int i = 0;
        while (before.at(i) == after.at(i))
            ++i;
        for (const QString &cand : {after.at(i), before.at(i)}) {
            QStringList t = before;
            t.move(t.indexOf(cand), after.indexOf(cand));
            if (t == after) {
                m_canvas->moveElementTo(cand, after.indexOf(cand));
                break;
            }
        }
    }
    m_busy = false;
    reload();
}

bool ShapeStrip::eventFilter(QObject *o, QEvent *e)
{
    if (o == viewport()) {
        if (e->type() == QEvent::MouseButtonPress) {
            auto *me = static_cast<QMouseEvent *>(e);
            if (me->button() == Qt::LeftButton) {
                m_pressAt = me->pos();
                m_hold.start();
            }
        } else if (e->type() == QEvent::MouseMove) {
            auto *me = static_cast<QMouseEvent *>(e);
            if ((me->pos() - m_pressAt).manhattanLength() > 6)
                m_hold.stop();
        } else if (e->type() == QEvent::MouseButtonRelease) {
            m_hold.stop();
        }
    }
    return QListWidget::eventFilter(o, e);
}

void ShapeStrip::showMenu(const QPoint &viewportPos)
{
    const QStringList ids = selectedIds();
    if (ids.isEmpty())
        return;
    QMenu menu(this);
    const QString many = ids.size() > 1 ? QStringLiteral(" %1 shapes").arg(ids.size()) : QString();
    QAction *dup = menu.addAction(QStringLiteral("Duplicate") + many);
    QAction *del = menu.addAction(QStringLiteral("Delete") + many);
    QAction *pick = menu.exec(viewport()->mapToGlobal(viewportPos));
    if (pick == dup)
        m_canvas->duplicateElements(ids);
    else if (pick == del)
        m_canvas->deleteElements(ids);
}

} // namespace c2d
