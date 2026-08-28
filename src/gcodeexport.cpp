#include "gcodeexport.h"
#include "c2ddocument.h"
#include "post_grbl.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QLineF>
#include <QPainterPathStroker>
#include <QPolygonF>

namespace c2d {

// Depth values are strings whose sign convention flipped between CC builds
// (843 stores "-19.126", 853 stores "19.126" for the same cut). Machine Z is
// negative below stock top either way.
static double depthToZ(const QJsonValue &v)
{
    return -qAbs(v.isString() ? v.toString().toDouble() : v.toDouble());
}

static QVector<const Element *> referenced(Document &doc, const QJsonObject &tp)
{
    QVector<const Element *> out;
    for (const QJsonValue &ev : tp.value("elements").toArray()) {
        const QString id = ev.toObject().value("uuid").toString();
        if (const Element *e = doc.elementById(id))
            out.append(e);
    }
    return out;
}

static void closeLoop(QPolygonF &poly)
{
    if (poly.size() > 2 && poly.first() != poly.last())
        poly.append(poly.first());
}

// Geometric offsetting with Qt path booleans: stroking the region boundary
// with width 2·delta gives a band from -delta..+delta around it; subtracting
// the band insets the region, uniting it outsets. Round joins approximate the
// true tool-radius offset.
static QPainterPath offsetBand(const QPainterPath &region, double delta)
{
    QPainterPathStroker st;
    st.setWidth(2 * delta);
    st.setJoinStyle(Qt::RoundJoin);
    st.setCapStyle(Qt::RoundCap);
    return st.createStroke(region);
}

static QList<QPolygonF> insetRings(const QPainterPath &region, double delta)
{
    QList<QPolygonF> out;
    const QPainterPath inner =
        region.subtracted(offsetBand(region, delta)).simplified();
    for (QPolygonF p : inner.toSubpathPolygons())
        if (p.size() > 2) {
            closeLoop(p);
            out.append(p);
        }
    return out;
}

static QList<QPolygonF> outsetRings(const QPainterPath &region, double delta)
{
    QList<QPolygonF> out;
    const QPainterPath outer =
        region.united(offsetBand(region, delta)).simplified();
    for (QPolygonF p : outer.toSubpathPolygons())
        if (p.size() > 2) {
            closeLoop(p);
            out.append(p);
        }
    return out;
}

// Union of the referenced closed vectors as a filled region.
static QPainterPath regionOf(const QVector<const Element *> &elems)
{
    QPainterPath r;
    for (const Element *e : elems) {
        QPainterPath p = e->painterPath;
        r = r.isEmpty() ? p : r.united(p);
    }
    return r.simplified();
}

// Split a (simplified) region into connected components: each outer boundary
// paired with its immediate holes, so pockets are machined one at a time.
static QList<QPainterPath> components(const QPainterPath &region)
{
    const QList<QPolygonF> polys = region.toSubpathPolygons();
    const int n = polys.size();
    QVector<int> depth(n, 0);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            if (i != j && !polys.at(i).isEmpty()
                && polys.at(j).containsPoint(polys.at(i).first(), Qt::OddEvenFill))
                depth[i]++;

    QHash<int, QPainterPath> comp;
    for (int i = 0; i < n; ++i)
        if (depth.at(i) % 2 == 0) {
            QPainterPath p;
            p.addPolygon(polys.at(i));
            p.closeSubpath();
            comp.insert(i, p);
        }
    for (int i = 0; i < n; ++i) {
        if (depth.at(i) % 2 == 0)
            continue;
        for (int j = 0; j < n; ++j) {
            if (j != i && depth.at(j) == depth.at(i) - 1 && comp.contains(j)
                && polys.at(j).containsPoint(polys.at(i).first(), Qt::OddEvenFill)) {
                comp[j].addPolygon(polys.at(i));
                comp[j].closeSubpath();
                break;
            }
        }
    }
    return comp.values();
}

// Circular-ring detection: every vertex equidistant from the vertex centroid.
// True for our circle vectors and their insets/outsets, which lets those rings
// go out as two G2/G3 half arcs instead of dozens of line segments.
static bool asCircle(const QPolygonF &poly, QPointF *center, double *radius, bool *ccw)
{
    if (poly.size() < 8 || poly.first() != poly.last())
        return false;
    const int n = poly.size() - 1;   // last repeats first
    QPointF c(0, 0);
    for (int i = 0; i < n; ++i)
        c += poly.at(i);
    c /= double(n);
    double rSum = 0;
    for (int i = 0; i < n; ++i)
        rSum += QLineF(c, poly.at(i)).length();
    const double r = rSum / n;
    if (r < 0.05)
        return false;
    for (int i = 0; i < n; ++i)
        if (qAbs(QLineF(c, poly.at(i)).length() - r) > qMax(0.01, r * 0.004))
            return false;
    double area2 = 0;   // signed area (Y-up: CCW positive)
    for (int i = 0; i < n; ++i) {
        const QPointF &a = poly.at(i), &b = poly.at(i + 1);
        area2 += a.x() * b.y() - b.x() * a.y();
    }
    *center = c;
    *radius = r;
    *ccw = area2 > 0;
    return true;
}

namespace {

// One contiguous cutting job: rings cut in order at each depth pass, with
// stay-down linking between nearby rings and between passes on closed loops.
struct Job {
    QList<QPolygonF> rings;
    bool closed = true;      // false: open engrave line (must retract to repeat)
    QPointF start() const { return rings.first().first(); }
};

class Emitter
{
public:
    Emitter(QVector<Op> &ops, double safeZ, double feed, double plunge)
        : m_ops(ops), m_safeZ(safeZ), m_feed(feed), m_plunge(plunge) {}

    // Follow one ring at depth z, using arcs when the ring is a circle.
    void followRing(const QPolygonF &poly, double z)
    {
        QPointF c;
        double r = 0;
        bool ccw = true;
        if (asCircle(poly, &c, &r, &ccw)) {
            const QPointF s = poly.first();
            const QPointF o = QPointF(2 * c.x() - s.x(), 2 * c.y() - s.y());
            m_ops.append(Op::arcTo(o.x(), o.y(), z, c.x() - s.x(), c.y() - s.y(), !ccw, m_feed));
            m_ops.append(Op::arcTo(s.x(), s.y(), z, c.x() - o.x(), c.y() - o.y(), !ccw, m_feed));
            return;
        }
        for (int i = 1; i < poly.size(); ++i)
            m_ops.append(Op::feedTo(poly.at(i).x(), poly.at(i).y(), z, m_feed));
    }

    void runJob(const Job &job, double zTop, double zBot, double stepdown,
                double linkDist)
    {
        const QPointF s0 = job.start();
        m_ops.append(Op::rapid(s0.x(), s0.y(), m_safeZ));
        double z = zTop;
        bool first = true;
        bool more = true;
        while (more) {
            const double zPrev = z;
            z = qMax(zBot, z - stepdown);
            more = z > zBot + 1e-9;

            if (first) {
                m_ops.append(Op::feedTo(s0.x(), s0.y(), z, m_plunge));
                first = false;
            } else if (job.closed) {
                // Closed loops end where they start: stay down, feed back to
                // the first ring's start at the previous depth, plunge deeper.
                m_ops.append(Op::feedTo(s0.x(), s0.y(), zPrev, m_feed));
                m_ops.append(Op::feedTo(s0.x(), s0.y(), z, m_plunge));
            } else {
                m_ops.append(Op::rapid(m_x, m_y, m_safeZ));
                m_ops.append(Op::rapid(s0.x(), s0.y(), m_safeZ));
                m_ops.append(Op::feedTo(s0.x(), s0.y(), z, m_plunge));
            }
            m_x = s0.x();
            m_y = s0.y();

            for (int k = 0; k < job.rings.size(); ++k) {
                const QPolygonF &ring = job.rings.at(k);
                if (k > 0) {
                    const QPointF rs = ring.first();
                    if (QLineF(QPointF(m_x, m_y), rs).length() <= linkDist) {
                        m_ops.append(Op::feedTo(rs.x(), rs.y(), z, m_feed));
                    } else {
                        m_ops.append(Op::rapid(m_x, m_y, m_safeZ));
                        m_ops.append(Op::rapid(rs.x(), rs.y(), m_safeZ));
                        m_ops.append(Op::feedTo(rs.x(), rs.y(), z, m_plunge));
                    }
                    m_x = rs.x();
                    m_y = rs.y();
                }
                followRing(ring, z);
                m_x = ring.last().x();
                m_y = ring.last().y();
            }
        }
        m_ops.append(Op::rapid(m_x, m_y, m_safeZ));
    }

private:
    QVector<Op> &m_ops;
    double m_safeZ, m_feed, m_plunge;
    double m_x = 0, m_y = 0;
};

// Greedy nearest-neighbor ordering of jobs to keep rapids short.
QList<Job> orderJobs(QList<Job> jobs, QPointF from)
{
    QList<Job> out;
    while (!jobs.isEmpty()) {
        int best = 0;
        double bestD = QLineF(from, jobs.first().start()).length();
        for (int i = 1; i < jobs.size(); ++i) {
            const double d = QLineF(from, jobs.at(i).start()).length();
            if (d < bestD) {
                bestD = d;
                best = i;
            }
        }
        from = jobs.at(best).start();
        out.append(jobs.takeAt(best));
    }
    return out;
}

} // namespace

GcodeResult exportGcode(Document &doc)
{
    GcodeResult res;
    QVector<Op> ops;
    const double safeZ = doc.params().value("retract", "2.54").toDouble();
    QPointF lastPos(0, 0);

    int lastTool = -1;
    for (const Toolpath &t : doc.toolpaths()) {
        const QJsonObject j = t.json;
        if (!j.value("enabled").toBool(true))
            continue;
        const QString name = j.value("name").toString();

        const bool contour = (t.type == QLatin1String("contour"));
        const bool pocket = (t.type == QLatin1String("pocket_toolpath"));
        const bool cutout = (t.type == QLatin1String("cutout"));
        const bool drilling = (t.type == QLatin1String("drilling_toolpath"));
        if (!contour && !pocket && !cutout && !drilling) {
            res.skipped << QStringLiteral("%1 (%2 not supported yet)")
                               .arg(name, t.type);
            continue;
        }

        const QJsonObject speeds = j.value("speeds").toObject();
        const double feed = speeds.value("feedrate").toDouble(500);
        const double plunge = speeds.value("plungerate").toDouble(100);
        const int rpm = int(speeds.value("rpm").toDouble(10000));
        const QJsonObject tool = j.value("tool").toObject();
        const int toolNo = int(tool.value("number").toDouble(0));
        const double toolR = tool.value("diameter").toDouble(6) / 2.0;

        const double zTop = depthToZ(j.value("start_depth"));
        double zBot;
        double stepdown;
        if (cutout) {
            zBot = depthToZ(j.value("cut_depth"))
                   + depthToZ(j.value("break_through"));   // both negative-down
            stepdown = qMax(0.05, j.value("depth_per_pass").toDouble(1.0));
        } else {
            zBot = depthToZ(j.value("end_depth"));
            stepdown = qMax(0.05, j.value("stepdown").toDouble(1.0));
        }

        const QVector<const Element *> elems = referenced(doc, j);
        if (elems.isEmpty()) {
            res.skipped << QStringLiteral("%1 (no vectors)").arg(name);
            continue;
        }

        QVector<Op> body;
        Emitter em(body, safeZ, feed, plunge);
        double linkDist = 1e9;   // contour/cutout jobs have one ring each

        QList<Job> jobs;
        if (drilling) {
            const double peck = j.value("peck_distance").toDouble(0);
            // Nearest-neighbor over the hole centers.
            QList<Job> holes;
            for (const Element *e : elems) {
                const QJsonArray c = e->raw.value("center").toArray();
                const QPointF ctr = c.size() == 2
                    ? QPointF(c.at(0).toDouble(), c.at(1).toDouble())
                    : e->painterPath.boundingRect().center();
                Job job;
                job.rings.append(QPolygonF() << ctr);
                holes.append(job);
            }
            for (const Job &h : orderJobs(holes, lastPos)) {
                const QPointF ctr = h.start();
                body.append(Op::rapid(ctr.x(), ctr.y(), safeZ));
                if (peck > 0.01) {
                    double z = zTop;
                    while (z > zBot + 1e-9) {
                        z = qMax(zBot, z - peck);
                        body.append(Op::feedTo(ctr.x(), ctr.y(), z, plunge));
                        body.append(Op::rapid(ctr.x(), ctr.y(), safeZ));
                    }
                } else {
                    body.append(Op::feedTo(ctr.x(), ctr.y(), zBot, plunge));
                    body.append(Op::rapid(ctr.x(), ctr.y(), safeZ));
                }
                lastPos = ctr;
            }
        } else if (contour && j.value("ofset_dir").toInt(0) == 0) {
            for (const Element *e : elems)
                for (const QPolygonF &p : e->painterPath.toSubpathPolygons())
                    if (p.size() >= 2) {
                        Job job;
                        job.rings.append(p);
                        job.closed = p.isClosed();
                        jobs.append(job);
                    }
        } else if (contour || cutout) {
            const QPainterPath region = regionOf(elems);
            const bool inside = contour ? j.value("ofset_dir").toInt(0) < 0
                                        : j.value("flip_inside_outside").toBool(false);
            const QList<QPolygonF> rings = inside ? insetRings(region, toolR)
                                                  : outsetRings(region, toolR);
            for (const QPolygonF &p : rings) {
                Job job;
                job.rings.append(p);
                jobs.append(job);
            }
        } else { // pocket: per connected component, inset shells innermost-first
            const double stepover = qMax(0.1, j.value("stepover").toDouble(toolR));
            linkDist = qMax(2.5 * stepover, 4 * toolR);
            for (const QPainterPath &comp : components(regionOf(elems))) {
                Job job;
                QList<QList<QPolygonF>> shells;
                double delta = toolR;
                while (shells.size() < 500) {
                    const QList<QPolygonF> s = insetRings(comp, delta);
                    if (s.isEmpty())
                        break;
                    shells.append(s);
                    delta += stepover;
                }
                for (int k = shells.size() - 1; k >= 0; --k)
                    job.rings.append(shells.at(k));
                if (!job.rings.isEmpty())
                    jobs.append(job);
            }
        }

        if (!drilling) {
            if (jobs.isEmpty()) {
                res.skipped << QStringLiteral("%1 (no cuttable loops)").arg(name);
                continue;
            }
            for (const Job &job : orderJobs(jobs, lastPos)) {
                em.runJob(job, zTop, zBot, stepdown, linkDist);
                lastPos = job.rings.last().last();
            }
        }

        if (body.isEmpty()) {
            res.skipped << QStringLiteral("%1 (empty)").arg(name);
            continue;
        }
        if (toolNo != lastTool) {
            ops.append(Op::tool(toolNo));
            lastTool = toolNo;
        }
        ops.append(Op::comment(name));
        ops.append(Op::spindle(rpm));
        ops.append(body);
        ops.append(Op::spindle(0));
        res.done << name;
    }

    res.gcode = GrblPost(true).generate(ops);
    return res;
}

} // namespace c2d
