#include "jobview.h"
#include "c2ddocument.h"
#include "gcodeexport.h"
#include "post_grbl.h"
#include "toollibrary.h"
#include "tooldiagram.h"
#include "toolstrip.h"

#include <QColor>
#include <QDateTime>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QProgressBar>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtMath>

#include <cmath>

namespace c2d {

// ---- reading the program ---------------------------------------------------

namespace {

// The words of one program line: letter -> value, in order of appearance.
struct Words {
    QVector<QPair<QChar, double>> w;
    bool has(QChar c) const
    {
        for (const auto &p : w)
            if (p.first == c)
                return true;
        return false;
    }
    double get(QChar c, double def) const
    {
        for (const auto &p : w)
            if (p.first == c)
                return p.second;
        return def;
    }
};

Words parseWords(const QString &line)
{
    Words out;
    int i = 0;
    const int n = line.size();
    while (i < n) {
        const QChar c = line.at(i).toUpper();
        if (c.isLetter()) {
            int j = i + 1;
            while (j < n && (line.at(j).isDigit() || line.at(j) == QLatin1Char('.')
                             || line.at(j) == QLatin1Char('-') || line.at(j) == QLatin1Char('+')))
                ++j;
            bool ok = false;
            const double v = line.mid(i + 1, j - i - 1).toDouble(&ok);
            if (ok)
                out.w.append({c, v});
            i = j;
        } else {
            ++i;
        }
    }
    return out;
}

QString typeLabel(const QString &type)
{
    if (type == QLatin1String("pocket_toolpath")) return QStringLiteral("Pocket");
    if (type == QLatin1String("contour")) return QStringLiteral("Contour");
    if (type == QLatin1String("cutout")) return QStringLiteral("Cutout");
    if (type == QLatin1String("drilling_toolpath")) return QStringLiteral("Drill");
    if (type == QLatin1String("advanced_vcarve_toolpath")) return QStringLiteral("V-carve");
    if (type == QLatin1String("texture_toolpath")) return QStringLiteral("Texture");
    if (type == QLatin1String("keyhole_toolpath")) return QStringLiteral("Keyhole");
    if (type.isEmpty()) return QString();
    QString t = type;
    t.remove(QStringLiteral("_toolpath"));
    t.replace(QLatin1Char('_'), QLatin1Char(' '));
    if (!t.isEmpty())
        t[0] = t.at(0).toUpper();
    return t;
}

QString mm(double v)
{
    QString s = QString::number(v, 'f', 1);
    if (s.endsWith(QLatin1String(".0")))
        s.chop(2);
    return s;
}

// "Pocket · Ø3 mm · 4 mm deep", the toolpath type and its bit for the
// toolpath called `name`, from the document; left empty when it is not
// there (a renamed or removed toolpath).
void describeToolpath(JobSection &s, const Document *doc, const QString &name)
{
    if (!doc)
        return;
    for (const Toolpath &t : doc->toolpaths()) {
        if (t.json.value(QStringLiteral("name")).toString() != name)
            continue;
        QStringList parts;
        s.type = t.json.value(QStringLiteral("type")).toString(t.type);
        const QString type = typeLabel(s.type);
        if (!type.isEmpty())
            parts << type;
        const QJsonObject tool = t.json.value(QStringLiteral("tool")).toObject();
        if (!tool.isEmpty())
            s.bit = toolpathBit(tool);
        const double dia = numOr(tool.value(QStringLiteral("diameter")), 0);
        if (dia > 0)
            parts << QStringLiteral("Ø%1 mm").arg(mm(dia));
        const double depth = std::abs(numOr(t.json.value(QStringLiteral("end_depth")), 0));
        if (depth > 0)
            parts << QStringLiteral("%1 mm deep").arg(mm(depth));
        s.detail = parts.join(QStringLiteral(" · "));
        return;
    }
}

// A toolpath row's share done, 0..1, drawn as a 2 px bar along the row's foot.
constexpr int ProgressRole = Qt::UserRole + 1;

class StepDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &ix) const override
    {
        QStyledItemDelegate::paint(p, opt, ix);
        const QVariant v = ix.siblingAtColumn(0).data(ProgressRole);
        if (!v.isValid())
            return;
        // The bar runs the width of the whole row: each column paints its
        // stretch of it, against the row's own left edge.
        const QRect row = opt.rect.adjusted(0, 0, 0, -3);
        const QRect track(row.left(), row.bottom() - 1, row.width(), 2);
        const auto *view = qobject_cast<const QAbstractItemView *>(opt.widget);
        const int left = view ? view->visualRect(ix.siblingAtColumn(0)).left() : row.left();
        const int right = view ? view->visualRect(ix.siblingAtColumn(ix.model()->columnCount() - 1)).right() : row.right();
        const double f = qBound(0.0, v.toDouble(), 1.0);
        const int fillTo = left + int(std::lround((right - left) * f));
        p->fillRect(track, QColor(0x2f, 0x34, 0x3d));
        const QRect done = track.intersected(QRect(QPoint(left, track.top()), QPoint(fillTo - 1, track.bottom())));
        if (!done.isEmpty())
            p->fillRect(done, f >= 1.0 ? QColor(0x6a, 0x71, 0x7d) : QColor(0xf2, 0xa5, 0x36));
    }
};

// The cut preview: the toolpath's own cutting moves from above, fitted to a
// box like the shape thumbnails on the toolpath cards. Drilling, which never
// moves sideways, shows its holes.
void paintCutPreview(QPainter &p, const QRectF &box, const JobSection &s, bool grey)
{
    const QColor ink = grey ? QColor(0x6a, 0x71, 0x7d) : QColor(0xf2, 0xa5, 0x36);
    p.setPen(QPen(grey ? QColor(0x2f, 0x34, 0x3d) : QColor(0x3a, 0x40, 0x4c), 1));
    p.setBrush(QColor(0x23, 0x26, 0x2e));
    p.drawRoundedRect(box.adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
    const bool sideways = s.cut.elementCount() > 1 && (s.cut.boundingRect().width() > 1e-6
                                                       || s.cut.boundingRect().height() > 1e-6);
    if (!sideways && s.plunges.isEmpty())
        return;
    QRectF r = sideways ? s.cut.boundingRect() : QRectF(s.plunges.first(), QSizeF(0, 0));
    if (!sideways)
        for (const QPointF &c : s.plunges) {
            r.setLeft(qMin(r.left(), c.x()));
            r.setRight(qMax(r.right(), c.x()));
            r.setTop(qMin(r.top(), c.y()));
            r.setBottom(qMax(r.bottom(), c.y()));
        }
    const QRectF in = box.adjusted(5, 5, -5, -5);
    const double span = qMax(qMax(r.width(), r.height()), 1e-6);
    const double k = (sideways || s.plunges.size() > 1) ? qMin(in.width(), in.height()) / span : 1;
    QTransform t;
    t.translate(in.center().x(), in.center().y());
    t.scale(k, -k);
    t.translate(-r.center().x(), -r.center().y());
    p.setBrush(Qt::NoBrush);
    if (sideways) {
        p.setPen(QPen(ink, 1.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(t.map(s.cut));
    } else {
        p.setPen(Qt::NoPen);
        p.setBrush(ink);
        for (const QPointF &c : s.plunges)
            p.drawEllipse(t.map(c), 2.2, 2.2);
    }
}

// The row's picture: the cut preview, then the bit's outline from the tool
// library. A tool change shows only the bit it changes to; done steps are
// greyed like their text.
QIcon sectionIcon(const JobSection &s)
{
    constexpr int kH = 52, kCut = 52, kGap = 8, kBit = 26, kDpr = 3;
    QIcon icon;
    for (const QIcon::Mode mode : {QIcon::Normal, QIcon::Disabled}) {
        QPixmap pm(QSize(kCut + kGap + kBit, kH) * kDpr);
        pm.setDevicePixelRatio(kDpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        if (s.kind == JobSection::Toolpath)
            paintCutPreview(p, QRectF(0, 0, kCut, kH), s, mode == QIcon::Disabled);
        if (!s.bit.kind.isEmpty())
            toolKindIcon(s.bit).paint(&p, QRect(kCut + kGap, 6, kBit, kH - 12), Qt::AlignCenter, mode);
        p.end();
        icon.addPixmap(pm, mode);
    }
    return icon;
}

// Each toolpath's cutting moves, in the order of the program's comments.
struct CutShape {
    QPainterPath path;
    QVector<QPointF> plunges;
};

QVector<CutShape> cutShapes(const QVector<Op> &ops)
{
    QVector<CutShape> out;
    double x = 0, y = 0, z = 0;
    bool inPath = false;                        // the path's pen is down at (x, y)
    for (const Op &o : ops) {
        if (o.kind == Op::Comment) {
            out.append(CutShape());
            inPath = false;
            continue;
        }
        if (o.kind != Op::Rapid && o.kind != Op::Feed && o.kind != Op::Arc)
            continue;
        if (o.kind == Op::Rapid || out.isEmpty()) {
            inPath = false;
        } else {
            CutShape &c = out.last();
            const bool flat = std::abs(o.x - x) < 1e-9 && std::abs(o.y - y) < 1e-9;
            if (flat) {
                if (o.z < z - 1e-9 && (c.plunges.isEmpty() || c.plunges.last() != QPointF(x, y)))
                    c.plunges.append(QPointF(x, y));
            } else {
                if (!inPath)
                    c.path.moveTo(x, y);
                inPath = true;
                if (o.kind == Op::Arc) {
                    const double cx = x + o.ci, cy = y + o.cj, rad = std::hypot(o.ci, o.cj);
                    const double a0 = std::atan2(y - cy, x - cx), a1 = std::atan2(o.y - cy, o.x - cx);
                    double sweep = o.cw ? a0 - a1 : a1 - a0;
                    while (sweep <= 1e-9)
                        sweep += 2 * M_PI;
                    const int n = qMax(2, int(sweep / (M_PI / 18)));
                    for (int i = 1; i <= n; ++i) {
                        const double a = a0 + (o.cw ? -1 : 1) * sweep * i / n;
                        c.path.lineTo(cx + rad * std::cos(a), cy + rad * std::sin(a));
                    }
                } else {
                    c.path.lineTo(o.x, o.y);
                }
            }
        }
        x = o.x;
        y = o.y;
        z = o.z;
    }
    return out;
}

} // namespace

JobPlan planJob(const QStringList &lines, const QVector<Op> &ops, const Document *doc, double rapidRate)
{
    JobPlan plan;
    // Full toolpath names, in the order their comments come: the post's
    // comment is the first 9 characters and "...".
    QStringList names;
    for (const Op &o : ops)
        if (o.kind == Op::Comment)
            names << o.text;
    const QVector<Op> changes = toolChanges(ops);
    const QVector<CutShape> cuts = cutShapes(ops);
    int nameAt = 0, changeAt = 0;

    if (rapidRate <= 0)
        rapidRate = 5000;
    double x = 0, y = 0, z = 0, f = 0;
    int motion = 0;                           // modal G0/1/2/3
    bool haveXY = false;
    int q = 0;                                // queue index of the next kept line
    plan.cumSec.append(0);

    JobSection *sec = nullptr;
    JobPass *pass = nullptr;
    int sinceClose = 0;                       // lines since the last section ended go to the next
    double floorZ = 0;
    bool flatSeen = false;                    // a cut along XY at the floor this pass
    int passCount = 0;

    auto closePass = [&] {
        if (pass) {
            pass->qEnd = q;
            pass->z = floorZ;
            pass->estSec = plan.cumSec.at(q) - plan.cumSec.at(pass->qStart);
            pass = nullptr;
        }
    };
    auto closeSection = [&] {
        closePass();
        if (sec) {
            sec->qEnd = q;
            sinceClose = q;
            sec->estSec = plan.cumSec.at(q) - plan.cumSec.at(sec->qStart);
            // Say what each pass is now the count is known.
            const int n = sec->passes.size();
            for (int i = 0; i < n; ++i) {
                JobPass &p = sec->passes[i];
                if (sec->kind == JobSection::ToolChange)
                    continue;
                p.what = n > 1 ? QStringLiteral("Pass %1 of %2  ·  to Z %3 mm").arg(i + 1).arg(n).arg(mm(p.z))
                               : QStringLiteral("to Z %1 mm").arg(mm(p.z));
            }
            sec = nullptr;
        }
    };
    auto openSection = [&](JobSection::Kind kind, const QString &name, const QString &detail) {
        closeSection();
        JobSection s;
        s.kind = kind;
        s.name = name;
        s.detail = detail;
        s.qStart = sinceClose;
        plan.sections.append(s);
        sec = &plan.sections.last();
        passCount = 0;
    };
    auto openPass = [&] {
        closePass();
        if (!sec)
            openSection(JobSection::Toolpath, QStringLiteral("Program"), QString());
        JobPass p;
        p.qStart = q;
        sec->passes.append(p);
        pass = &sec->passes.last();
        floorZ = z;
        flatSeen = false;
        ++passCount;
    };

    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty())
            continue;
        if (line.startsWith(QLatin1Char('('))) {
            // A toolpath starts (not queued: no index).
            QString shown = line.mid(1);
            if (shown.endsWith(QLatin1Char(')')))
                shown.chop(1);
            QString name = shown.trimmed();
            const int cutAt = nameAt;
            if (nameAt < names.size()) {
                const QString full = names.at(nameAt);
                QString t = full;
                for (QChar &ch : t)
                    if (ch == u'(' || ch == u')' || ch == u'\n' || ch == u'\r')
                        ch = u' ';
                if (t.length() > 12)
                    t = t.left(9) + QStringLiteral("...");
                if (t == shown || t.trimmed() == name)
                    name = full;
                ++nameAt;
            }
            openSection(JobSection::Toolpath, name, QString());
            describeToolpath(*sec, doc, name);
            if (cutAt < cuts.size()) {
                sec->cut = cuts.at(cutAt).path;
                sec->plunges = cuts.at(cutAt).plunges;
            }
            continue;
        }
        // From here on the line is queued.
        double sec_ = 0;
        const QString up = line.toUpper();
        if (up.startsWith(QLatin1String("M0 ;T")) || up.startsWith(QLatin1String("M0;T"))
            || up.startsWith(QLatin1String("M00 ;T"))) {
            const int tool = up.mid(up.indexOf(QLatin1Char('T')) + 1).toInt();
            QString detail = QStringLiteral("T%1").arg(tool);
            LibraryTool lt;
            if (doc && changeAt < changes.size()) {
                lt = toolForChange(*doc, changes.at(changeAt));
                if (!lt.name.isEmpty() || lt.diameter > 0)
                    detail += QStringLiteral("  %1%2").arg(lt.diameter > 0 ? QStringLiteral("Ø%1 mm ").arg(mm(lt.diameter)) : QString(), lt.name);
            }
            ++changeAt;
            openSection(JobSection::ToolChange, QStringLiteral("Tool change"), detail);
            sec->bit = lt;
            JobPass p;
            p.qStart = q;
            p.what = QStringLiteral("park, change the bit, measure, continue");
            sec->passes.append(p);
            pass = &sec->passes.last();
            // Ends with this line: the section after it starts with the
            // next toolpath comment (or the program's own retract).
            plan.cumSec.append(plan.cumSec.last());
            ++q;
            closeSection();
            continue;
        }
        if (up.contains(QLatin1String("G53"))) {
            // A machine-coordinate move of the sender's (the rise to the
            // top): about a second, and nothing modal to read from it.
            sec_ = 1.0;
        } else if (up.startsWith(QLatin1Char('M'))) {
            sec_ = 0;                                // M03/M05: no wait of its own
        } else if (up.startsWith(QLatin1String("G4"))) {
            sec_ = parseWords(up).get(QLatin1Char('P'), 0);
        } else {
            const Words w = parseWords(up);
            if (w.has(QLatin1Char('G'))) {
                for (const auto &p : w.w)
                    if (p.first == QLatin1Char('G') && p.second >= 0 && p.second <= 3)
                        motion = int(p.second);
            }
            const bool hasX = w.has(QLatin1Char('X')), hasY = w.has(QLatin1Char('Y')), hasZ = w.has(QLatin1Char('Z'));
            const double nx = w.get(QLatin1Char('X'), x), ny = w.get(QLatin1Char('Y'), y), nz = w.get(QLatin1Char('Z'), z);
            if (w.has(QLatin1Char('F')))
                f = w.get(QLatin1Char('F'), f);
            if (hasX || hasY || hasZ) {
                double len = 0;
                if ((motion == 2 || motion == 3) && (w.has(QLatin1Char('I')) || w.has(QLatin1Char('J')))) {
                    const double ci = x + w.get(QLatin1Char('I'), 0), cj = y + w.get(QLatin1Char('J'), 0);
                    const double r = std::hypot(x - ci, y - cj);
                    double a0 = std::atan2(y - cj, x - ci), a1 = std::atan2(ny - cj, nx - ci);
                    double sweep = motion == 3 ? a1 - a0 : a0 - a1;
                    while (sweep <= 1e-9)
                        sweep += 2 * M_PI;
                    while (sweep > 2 * M_PI + 1e-9)
                        sweep -= 2 * M_PI;
                    len = std::hypot(r * sweep, nz - z);
                } else {
                    len = std::sqrt((nx - x) * (nx - x) + (ny - y) * (ny - y) + (nz - z) * (nz - z));
                }
                if (motion == 0)
                    sec_ = len / rapidRate * 60.0;
                else if (f > 0)
                    sec_ = len / f * 60.0;
                // Passes: a cutting move deeper than this pass's floor, once
                // the pass has cut along the floor, starts the next one. A
                // ramp or a helix going down is the start of a pass; pecks
                // that only go deeper (drilling) stay one pass.
                if (motion != 0) {
                    const bool xyMove = haveXY && (std::abs(nx - x) > 1e-9 || std::abs(ny - y) > 1e-9);
                    if (!pass) {
                        openPass();
                        floorZ = nz;
                    } else if (nz < floorZ - 1e-6 && flatSeen) {
                        openPass();
                        floorZ = nz;
                    } else if (nz < floorZ - 1e-6) {
                        floorZ = nz;
                    }
                    if (xyMove && std::abs(nz - z) < 1e-9 && std::abs(nz - floorZ) < 1e-6)
                        flatSeen = true;
                }
                x = nx;
                y = ny;
                z = nz;
                haveXY = true;
            }
        }
        plan.cumSec.append(plan.cumSec.last() + sec_);
        ++q;
    }
    closeSection();
    plan.total = q;
    return plan;
}

// ---- the widget ------------------------------------------------------------

JobView::JobView(QWidget *parent) : QWidget(parent)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(8);
    m_title = new QLabel(this);
    QFont tf = m_title->font();
    tf.setBold(true);
    tf.setPointSizeF(tf.pointSizeF() + 3);
    m_title->setFont(tf);
    m_title->setWordWrap(true);
    m_summary = new QLabel(this);
    m_summary->setStyleSheet(QStringLiteral("color: #a3abb8; font-size: 14px;"));
    m_current = new QLabel(this);
    m_current->setStyleSheet(QStringLiteral("color: #f2a536; font-size: 15px; font-weight: 600;"));
    m_current->setWordWrap(true);
    m_passBar = new QProgressBar(this);
    m_passBar->setTextVisible(false);
    m_passBar->setFixedHeight(8);
    m_tree = new QTreeWidget(this);
    m_tree->setItemDelegate(new StepDelegate(m_tree));
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({QStringLiteral("Step"), QStringLiteral("Time")});
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->setRootIsDecorated(true);
    m_tree->setItemsExpandable(false);
    m_tree->setFocusPolicy(Qt::NoFocus);
    m_tree->setSelectionMode(QAbstractItemView::NoSelection);
    m_tree->setUniformRowHeights(false);
    m_tree->setIconSize(QSize(86, 52));
    m_tree->setStyleSheet(QStringLiteral(
        "QTreeWidget { background: #16181d; border: none; font-size: 14px; }"
        "QTreeWidget::item { padding: 4px 2px; }"
        "QHeaderView::section { background: #1e2127; color: #8b93a1; border: none; padding: 4px; }"));
    v->addWidget(m_title);
    v->addWidget(m_summary);
    v->addWidget(m_current);
    v->addWidget(m_passBar);
    v->addWidget(m_tree, 1);
    m_tick = new QTimer(this);
    m_tick->setInterval(1000);
    connect(m_tick, &QTimer::timeout, this, [this] { refresh(); });
    clear();
}

void JobView::clear()
{
    m_plan = JobPlan();
    m_jobName.clear();
    m_acked = 0;
    m_inserted = 0;
    m_running = -1;
    m_runningPass = -1;
    m_parked = false;
    m_parkedAt = -1;
    m_parkedTotal = 0;
    m_sectionStart.clear();
    m_sectionDone.clear();
    m_passStart.clear();
    m_passDone.clear();
    m_sectionItems.clear();
    m_sectionIcons.clear();
    m_passItems.clear();
    m_tree->clear();
    m_title->setText(QStringLiteral("No job running"));
    m_summary->clear();
    m_current->clear();
    m_passBar->setRange(0, 1);
    m_passBar->setValue(0);
    m_tick->stop();
}

void JobView::setPlan(const JobPlan &plan, const QString &jobName)
{
    clear();
    m_plan = plan;
    m_jobName = jobName;
    m_clock.start();
    const int n = plan.sections.size();
    m_sectionStart = QVector<qint64>(n, -1);
    m_sectionDone = QVector<qint64>(n, -1);
    m_passStart.resize(n);
    m_passDone.resize(n);
    m_sectionItems.resize(n);
    m_sectionIcons.resize(n);
    m_passItems.resize(n);
    for (int i = 0; i < n; ++i) {
        const JobSection &s = plan.sections.at(i);
        auto *it = new QTreeWidgetItem(m_tree);
        it->setText(0, s.detail.isEmpty() ? s.name : s.name + QStringLiteral("   —  ") + s.detail);
        it->setText(1, clock(s.estSec));
        it->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        m_sectionIcons[i] = sectionIcon(s);
        it->setIcon(0, m_sectionIcons.at(i));
        m_sectionItems[i] = it;
        const int np = s.passes.size();
        m_passStart[i] = QVector<qint64>(np, -1);
        m_passDone[i] = QVector<qint64>(np, -1);
        m_passItems[i].resize(np);
        for (int k = 0; k < np; ++k) {
            const JobPass &p = s.passes.at(k);
            // One pass: the section line says it all (its text is the
            // section's "what"); several: a child each.
            if (np == 1 && s.kind == JobSection::Toolpath) {
                it->setText(0, it->text(0) + QStringLiteral("   ·  ") + p.what);
                m_passItems[i][k] = nullptr;
                continue;
            }
            if (s.kind == JobSection::ToolChange) {
                m_passItems[i][k] = nullptr;
                continue;
            }
            auto *c = new QTreeWidgetItem(it);
            c->setText(0, p.what);
            c->setText(1, clock(p.estSec));
            c->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
            m_passItems[i][k] = c;
        }
        it->setExpanded(true);
    }
    m_title->setText(m_jobName.isEmpty() ? QStringLiteral("Job") : m_jobName);
    m_tick->start();
    refresh();
}

void JobView::setProgress(int acked, int total)
{
    Q_UNUSED(total);
    m_acked = acked;
    refresh();
}

void JobView::setOverride(int percent)
{
    if (percent <= 0 || percent == m_override)
        return;
    m_override = percent;
    refresh();
}

void JobView::setParked(bool parked)
{
    if (parked == m_parked)
        return;
    m_parked = parked;
    if (!m_clock.isValid())
        return;
    if (parked) {
        m_parkedAt = m_clock.elapsed();
    } else if (m_parkedAt >= 0) {
        m_parkedTotal += m_clock.elapsed() - m_parkedAt;
        m_parkedAt = -1;
    }
    refresh();
}

double JobView::spanSec(int a, int b) const
{
    a = qBound(0, a, m_plan.total);
    b = qBound(0, b, m_plan.total);
    if (b <= a)
        return 0;
    return (m_plan.cumSec.at(b) - m_plan.cumSec.at(a)) * 100.0 / qMax(10, m_override);
}

double JobView::leftFrom(int q) const
{
    return spanSec(q, m_plan.total);
}

QString JobView::clock(double sec)
{
    const int s = int(std::lround(qMax(0.0, sec)));
    if (s >= 3600)
        return QStringLiteral("%1:%2:%3").arg(s / 3600).arg((s / 60) % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

QString JobView::eta(double sec)
{
    return QDateTime::currentDateTime().addSecs(qint64(std::lround(qMax(0.0, sec)))).toString(QStringLiteral("HH:mm"));
}

void JobView::refresh()
{
    if (m_plan.isEmpty())
        return;
    // Plan index of the first line not yet acked. Lines the sender inserted
    // (rise, return) are acked before the plan's own continue, so they are
    // taken off; never below what was already reached.
    const int q = qBound(0, m_acked - m_inserted, m_plan.total);
    const qint64 now = m_clock.elapsed();
    const QColor cDone(0x6a, 0x71, 0x7d), cRun(0xf2, 0xa5, 0x36), cAhead(0xd6, 0xdb, 0xe3);
    int runSec = -1, runPass = -1;
    for (int i = 0; i < m_plan.sections.size(); ++i) {
        const JobSection &s = m_plan.sections.at(i);
        QTreeWidgetItem *it = m_sectionItems.at(i);
        const bool started = q >= s.qStart;
        const bool finished = q >= s.qEnd;
        if (started && m_sectionStart[i] < 0)
            m_sectionStart[i] = now;
        if (finished && m_sectionDone[i] < 0) {
            m_sectionDone[i] = now;
            it->setIcon(0, QIcon(m_sectionIcons.at(i).pixmap(QSize(86, 52), QIcon::Disabled)));
        }
        QFont f = it->font(0);
        f.setBold(started && !finished);
        it->setFont(0, f);
        it->setFont(1, f);
        const QColor col = finished ? cDone : started ? cRun : cAhead;
        it->setForeground(0, col);
        it->setForeground(1, col);
        if (finished) {
            const qint64 took = m_sectionDone[i] - qMax<qint64>(0, m_sectionStart[i]);
            it->setText(1, QStringLiteral("✓ %1").arg(clock(took / 1000.0)));
        } else if (started) {
            it->setText(1, QStringLiteral("%1 left").arg(clock(spanSec(q, s.qEnd))));
            runSec = i;
        } else {
            it->setText(1, clock(s.estSec * 100.0 / qMax(10, m_override)));
        }
        if (s.kind == JobSection::Toolpath) {
            const double whole = spanSec(s.qStart, s.qEnd);
            const double f = finished ? 1.0 : !started ? 0.0
                           : whole > 0 ? spanSec(s.qStart, q) / whole
                                       : double(q - s.qStart) / qMax(1, s.qEnd - s.qStart);
            it->setData(0, ProgressRole, f);
        }
        for (int k = 0; k < s.passes.size(); ++k) {
            const JobPass &p = s.passes.at(k);
            const bool pStarted = q >= p.qStart;
            const bool pFinished = q >= p.qEnd;
            if (pStarted && m_passStart[i][k] < 0)
                m_passStart[i][k] = now;
            if (pFinished && m_passDone[i][k] < 0)
                m_passDone[i][k] = now;
            if (pStarted && !pFinished && runSec == i)
                runPass = k;
            QTreeWidgetItem *c = m_passItems.at(i).at(k);
            if (!c)
                continue;
            QFont pf = c->font(0);
            pf.setBold(pStarted && !pFinished);
            c->setFont(0, pf);
            c->setFont(1, pf);
            const QColor pc = pFinished ? cDone : pStarted ? cRun : cAhead;
            c->setForeground(0, pc);
            c->setForeground(1, pc);
            if (pFinished)
                c->setText(1, QStringLiteral("✓ %1").arg(clock((m_passDone[i][k] - qMax<qint64>(0, m_passStart[i][k])) / 1000.0)));
            else if (pStarted)
                c->setText(1, QStringLiteral("%1 left").arg(clock(spanSec(q, p.qEnd))));
            else
                c->setText(1, clock(p.estSec * 100.0 / qMax(10, m_override)));
        }
    }
    if (runSec != m_running || runPass != m_runningPass) {
        m_running = runSec;
        m_runningPass = runPass;
        if (runSec >= 0) {
            QTreeWidgetItem *target = (runPass >= 0 && m_passItems.at(runSec).at(runPass))
                                          ? m_passItems.at(runSec).at(runPass)
                                          : m_sectionItems.at(runSec);
            m_tree->scrollToItem(target, QAbstractItemView::PositionAtCenter);
        }
    }
    // Header: what runs now, and the whole job's time left and finish time.
    const double left = leftFrom(q);
    const QString ov = m_override != 100 ? QStringLiteral("  ·  feed %1 %").arg(m_override) : QString();
    if (q >= m_plan.total) {
        m_summary->setText(QStringLiteral("All %1 steps done  ·  took %2")
                               .arg(m_plan.sections.size()).arg(clock((now - m_parkedTotal) / 1000.0)));
        m_current->setText(QStringLiteral("Finished"));
        m_passBar->setRange(0, 1);
        m_passBar->setValue(1);
        m_tick->stop();
    } else {
        m_summary->setText(QStringLiteral("%1 left  ·  done about %2  ·  step %3 of %4%5")
                               .arg(clock(left), eta(left)).arg(qMax(0, runSec) + 1).arg(m_plan.sections.size()).arg(ov));
        if (runSec >= 0) {
            const JobSection &s = m_plan.sections.at(runSec);
            QString what = s.name;
            if (runPass >= 0 && runPass < s.passes.size() && !s.passes.at(runPass).what.isEmpty())
                what += QStringLiteral("  ·  ") + s.passes.at(runPass).what;
            if (m_parked)
                what += QStringLiteral("  ·  waiting at the tool change");
            m_current->setText(what);
            const int a = runPass >= 0 ? s.passes.at(runPass).qStart : s.qStart;
            const int b = runPass >= 0 ? s.passes.at(runPass).qEnd : s.qEnd;
            m_passBar->setRange(0, qMax(1, b - a));
            m_passBar->setValue(qBound(0, q - a, qMax(1, b - a)));
        } else {
            m_current->setText(QStringLiteral("Starting…"));
            m_passBar->setRange(0, 1);
            m_passBar->setValue(0);
        }
    }
}

} // namespace c2d
