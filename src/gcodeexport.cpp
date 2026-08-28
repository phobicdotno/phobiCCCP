#include "gcodeexport.h"
#include "c2ddocument.h"
#include "post_grbl.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QLineF>
#include <QPainterPathStroker>
#include <QPolygonF>
#include <QtMath>
#include <algorithm>

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

// Circular-ring detection: every vertex equidistant from the vertex centroid,
// letting the ring go out as two G2/G3 half arcs instead of line segments.
static bool asCircle(const QPolygonF &poly, QPointF *center, double *radius, bool *ccw)
{
    if (poly.size() < 8 || poly.first() != poly.last())
        return false;
    const int n = poly.size() - 1;
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
    double area2 = 0;
    for (int i = 0; i < n; ++i) {
        const QPointF &a = poly.at(i), &b = poly.at(i + 1);
        area2 += a.x() * b.y() - b.x() * a.y();
    }
    *center = c;
    *radius = r;
    *ccw = area2 > 0;
    return true;
}

static double perimeter(const QPolygonF &poly)
{
    double len = 0;
    for (int i = 1; i < poly.size(); ++i)
        len += QLineF(poly.at(i - 1), poly.at(i)).length();
    return len;
}

// Deterministic pseudo-random for the texture toolpath (no wall-clock seeds).
struct Lcg {
    quint32 s;
    explicit Lcg(quint32 seed) : s(seed ? seed : 1) {}
    double next() { s = s * 1664525u + 1013904223u; return (s >> 8) / double(1 << 24); }
    double in(double lo, double hi) { return lo + (hi - lo) * next(); }
};

namespace {

// One contiguous cutting job: rings cut in order at each depth pass. The pass
// direction alternates (in→out, then out→in) so the tool plunges in place at
// each new depth instead of tracking back across the pocket.
struct Job {
    QList<QPolygonF> rings;
    bool closed = true;      // false: open engrave line (retract to repeat)
    QPointF start() const { return rings.first().first(); }
};

struct CutParams {
    double zTop = 0, zBot = 0, stepdown = 1.0;
    double linkDist = 1e9;   // stay-down link limit between rings
    double rampAngle = 0;    // >0: ramp entries on closed loops (degrees)
    double tabTop = -1e9;    // cutout tabs: lift to this Z inside a tab
    double tabWidth = 0;     // tab span along the loop (0 = no tabs)
};

class Emitter
{
public:
    Emitter(QVector<Op> &ops, double safeZ, double feed, double plunge)
        : m_ops(ops), m_safeZ(safeZ), m_feed(feed), m_plunge(plunge) {}

    QPointF pos() const { return QPointF(m_x, m_y); }

    void rapidTo(const QPointF &p)
    {
        m_ops.append(Op::rapid(p.x(), p.y(), m_safeZ));
        m_x = p.x();
        m_y = p.y();
    }

    void plungeTo(const QPointF &p, double z)
    {
        m_ops.append(Op::feedTo(p.x(), p.y(), z, m_plunge));
        m_x = p.x();
        m_y = p.y();
    }

    // Descend along the ring at rampAngle instead of a straight plunge,
    // walking the loop (multiple laps if needed) until z is reached.
    void rampDescend(const QPolygonF &ring, double zFrom, double zTo, double angleDeg)
    {
        const double slope = qTan(qDegreesToRadians(qBound(1.0, angleDeg, 45.0)));
        double z = zFrom;
        int lap = 0;
        while (z > zTo + 1e-9 && lap < 20) {
            for (int i = 1; i < ring.size() && z > zTo + 1e-9; ++i) {
                const double L = QLineF(ring.at(i - 1), ring.at(i)).length();
                z = qMax(zTo, z - L * slope);
                m_ops.append(Op::feedTo(ring.at(i).x(), ring.at(i).y(), z, m_plunge));
                m_x = ring.at(i).x();
                m_y = ring.at(i).y();
            }
            ++lap;
        }
        // Return to the loop start at depth so the full pass begins cleanly.
        if (QLineF(pos(), ring.first()).length() > 1e-6)
            followSpan(ring, 0, ring.size() - 1, zTo);
    }

    // Follow ring vertices from index a to b (inclusive) at depth z.
    void followSpan(const QPolygonF &ring, int a, int b, double z)
    {
        for (int i = a; i <= b; ++i) {
            m_ops.append(Op::feedTo(ring.at(i).x(), ring.at(i).y(), z, m_feed));
            m_x = ring.at(i).x();
            m_y = ring.at(i).y();
        }
    }

    // Follow one closed/open ring at depth z; arcs for circles; tab lifts when
    // z is below tabTop and tabWidth > 0.
    void followRing(const QPolygonF &poly, double z, const CutParams &cp)
    {
        const bool tabs = cp.tabWidth > 0.01 && z < cp.tabTop - 1e-9
                          && poly.isClosed() && poly.size() > 3;
        if (!tabs) {
            QPointF c;
            double r = 0;
            bool ccw = true;
            if (asCircle(poly, &c, &r, &ccw)) {
                const QPointF s = poly.first();
                const QPointF o(2 * c.x() - s.x(), 2 * c.y() - s.y());
                m_ops.append(Op::arcTo(o.x(), o.y(), z, c.x() - s.x(), c.y() - s.y(), !ccw, m_feed));
                m_ops.append(Op::arcTo(s.x(), s.y(), z, c.x() - o.x(), c.y() - o.y(), !ccw, m_feed));
                m_x = s.x();
                m_y = s.y();
                return;
            }
            followSpan(poly, 1, poly.size() - 1, z);
            return;
        }

        // Tabbed pass: evenly spaced tabs, each cp.tabWidth long, crossed at
        // cp.tabTop instead of z.
        const double len = perimeter(poly);
        const int nTabs = qBound(3, int(len / 150.0) + 3, 8);
        auto inTab = [&](double s) {
            for (int t = 0; t < nTabs; ++t) {
                const double c = len * (t + 0.5) / nTabs;
                if (qAbs(s - c) <= cp.tabWidth / 2
                    || qAbs(s - c + len) <= cp.tabWidth / 2
                    || qAbs(s - c - len) <= cp.tabWidth / 2)
                    return true;
            }
            return false;
        };
        double s = 0;
        const double step = 1.0;   // 1 mm resolution along the loop
        bool up = false;
        for (int i = 1; i < poly.size(); ++i) {
            const QLineF seg(poly.at(i - 1), poly.at(i));
            const double L = seg.length();
            double done = 0;
            while (done < L - 1e-9) {
                const double d = qMin(step, L - done);
                done += d;
                s += d;
                const QPointF p = seg.pointAt(done / L);
                const bool tab = inTab(s);
                if (tab != up) {
                    up = tab;
                    m_ops.append(Op::feedTo(m_x, m_y, up ? cp.tabTop : z,
                                            up ? m_feed : m_plunge));
                }
                m_ops.append(Op::feedTo(p.x(), p.y(), up ? cp.tabTop : z, m_feed));
                m_x = p.x();
                m_y = p.y();
            }
        }
        if (up)
            m_ops.append(Op::feedTo(m_x, m_y, z, m_plunge));
    }

    void retract()
    {
        m_ops.append(Op::rapid(m_x, m_y, m_safeZ));
    }

    void runJob(const Job &job, const CutParams &cp)
    {
        rapidTo(job.start());
        double z = cp.zTop;
        bool first = true;
        bool more = true;
        bool forward = true;
        while (more) {
            z = qMax(cp.zBot, z - cp.stepdown);
            more = z > cp.zBot + 1e-9;

            // Ring order alternates per pass so each new pass starts where the
            // last one ended — plunge in place, no track-back across the pocket.
            QList<int> order;
            for (int k = 0; k < job.rings.size(); ++k)
                order.append(forward ? k : job.rings.size() - 1 - k);

            const QPolygonF &firstRing = job.rings.at(order.first());
            if (first) {
                if (cp.rampAngle > 0 && job.closed) {
                    plungeTo(firstRing.first(), cp.zTop);
                    rampDescend(firstRing, cp.zTop, z, cp.rampAngle);
                } else {
                    plungeTo(firstRing.first(), z);
                }
                first = false;
            } else if (job.closed) {
                if (cp.rampAngle > 0)
                    rampDescend(firstRing, z + cp.stepdown, z, cp.rampAngle);
                else
                    plungeTo(pos(), z);   // in place — we ended here last pass
            } else {
                retract();
                rapidTo(job.start());
                plungeTo(job.start(), z);
            }

            for (int oi = 0; oi < order.size(); ++oi) {
                const QPolygonF &ring = job.rings.at(order.at(oi));
                const QPointF rs = ring.first();
                if (QLineF(pos(), rs).length() > 1e-6) {
                    if (QLineF(pos(), rs).length() <= cp.linkDist) {
                        m_ops.append(Op::feedTo(rs.x(), rs.y(), z, m_feed));
                        m_x = rs.x();
                        m_y = rs.y();
                    } else {
                        retract();
                        rapidTo(rs);
                        plungeTo(rs, z);
                    }
                }
                followRing(ring, z, cp);
            }
            forward = !forward;
        }
        retract();
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
        const bool keyhole = (t.type == QLatin1String("keyhole_toolpath"));
        const bool texture = (t.type == QLatin1String("texture_toolpath"));
        const bool vcarve = (t.type == QLatin1String("advanced_vcarve_toolpath"));
        if (!contour && !pocket && !cutout && !drilling && !keyhole && !texture
            && !vcarve) {
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

        CutParams cp;
        cp.zTop = depthToZ(j.value("start_depth"));
        if (cutout) {
            cp.zBot = depthToZ(j.value("cut_depth"))
                      + depthToZ(j.value("break_through"));
            cp.stepdown = qMax(0.05, j.value("depth_per_pass").toDouble(1.0));
            if (!j.value("ignore_tabs").toBool(true)
                && j.value("tab_height").toDouble() > 0.01) {
                cp.tabTop = cp.zBot + j.value("tab_height").toDouble();
                cp.tabWidth = qBound(1.0, j.value("tab_width").toDouble(6.0), 30.0);
            }
        } else {
            cp.zBot = depthToZ(j.value("end_depth"));
            cp.stepdown = qMax(0.05, j.value("stepdown").toDouble(1.0));
        }
        if (j.value("enable_ramping").toBool(false))
            cp.rampAngle = qBound(1.0, j.value("ramp_angle").toDouble(20.0), 45.0);

        const QVector<const Element *> elems = referenced(doc, j);
        if (elems.isEmpty()) {
            res.skipped << QStringLiteral("%1 (no vectors)").arg(name);
            continue;
        }

        QVector<Op> body;
        Emitter em(body, safeZ, feed, plunge);

        QList<Job> jobs;
        if (drilling || keyhole) {
            const double peck = j.value("peck_distance").toDouble(0);
            const double slotLen = j.value("length").toDouble(12.7);
            const double slotAng = qDegreesToRadians(j.value("angle").toDouble(90));
            QList<Job> pts;
            for (const Element *e : elems) {
                const QJsonArray c = e->raw.value("center").toArray();
                const QPointF ctr = c.size() == 2
                    ? QPointF(c.at(0).toDouble(), c.at(1).toDouble())
                    : e->painterPath.boundingRect().center();
                Job job;
                job.rings.append(QPolygonF() << ctr);
                pts.append(job);
            }
            for (const Job &h : orderJobs(pts, lastPos)) {
                const QPointF ctr = h.start();
                em.rapidTo(ctr);
                if (drilling && peck > 0.01) {
                    double z = cp.zTop;
                    while (z > cp.zBot + 1e-9) {
                        z = qMax(cp.zBot, z - peck);
                        em.plungeTo(ctr, z);
                        em.retract();
                    }
                } else {
                    em.plungeTo(ctr, cp.zBot);
                    if (keyhole) {
                        // Slide the keyhole bit out and back at depth.
                        const QPointF end = ctr + slotLen
                            * QPointF(qCos(slotAng), qSin(slotAng));
                        body.append(Op::feedTo(end.x(), end.y(), cp.zBot, feed));
                        body.append(Op::feedTo(ctr.x(), ctr.y(), cp.zBot, feed));
                    }
                    em.retract();
                }
                lastPos = ctr;
            }
        } else if (texture) {
            // Decorative strokes: hatch lines at `angle`, clipped to the
            // region, broken into random-length strokes at random depths.
            const QPainterPath region = regionOf(elems);
            const QRectF bb = region.boundingRect();
            const double ang = qDegreesToRadians(j.value("angle").toDouble(0));
            const QPointF dir(qCos(ang), qSin(ang));
            const QPointF nrm(-dir.y(), dir.x());
            const double stepover = qMax(0.2, j.value("stepover").toDouble(3));
            const double sVar = qBound(0.0, j.value("stepover_variation").toDouble(0), 1.0);
            const double minL = qMax(1.0, j.value("min_length").toDouble(15));
            const double maxL = qMax(minL, j.value("max_length").toDouble(30));
            const double zMin = depthToZ(j.value("min_depth"));
            const double zMax = depthToZ(j.value("max_depth"));
            Lcg rng(qHash(t.uuid));

            const double diag = QLineF(bb.topLeft(), bb.bottomRight()).length();
            const QPointF mid = bb.center();
            int guard = 0;
            for (double off = -diag / 2; off < diag / 2 && guard < 20000; off +=
                 stepover * (1.0 + sVar * (rng.next() - 0.5))) {
                const QPointF base = mid + nrm * off;
                // walk the hatch line, extracting in-region runs
                double runStart = -1;
                for (double s = -diag / 2; s <= diag / 2 + 0.5; s += 0.5) {
                    const QPointF p = base + dir * s;
                    const bool in = s <= diag / 2 && region.contains(p);
                    if (in && runStart < 0)
                        runStart = s;
                    if (!in && runStart >= 0) {
                        double a = runStart;
                        const double runEnd = s - 0.5;
                        runStart = -1;
                        while (a < runEnd - 1.0 && guard < 20000) {
                            ++guard;
                            const double L =
                                qMin(runEnd - a, rng.in(minL, maxL));
                            const QPointF sp = base + dir * a;
                            const QPointF ep = base + dir * (a + L);
                            const double z = rng.in(qMin(zMin, zMax), qMax(zMin, zMax));
                            em.rapidTo(sp);
                            em.plungeTo(sp, z);
                            body.append(Op::feedTo(ep.x(), ep.y(), z, feed));
                            em.retract();
                            a += qMax(1.0, L * 0.7 + rng.in(0, L * 0.3));
                        }
                    }
                }
            }
        } else if (vcarve) {
            // V-carve as depth-graded inset rings: a V-bit centered δ inside
            // the boundary touches it at depth δ / tan(halfAngle).
            const double bitAngle = tool.value("angle").toDouble(60);
            const double halfTan = qTan(qDegreesToRadians(qBound(10.0, bitAngle, 179.0) / 2));
            const double stepover = qMax(0.05, j.value("stepover").toDouble(0.2));
            const QPainterPath region = regionOf(elems);
            if (j.value("pocket_enabled").toBool(false))
                res.skipped << QStringLiteral("%1 (flat pocket pass not emitted)").arg(name);
            // Carve one component (letter/shape) completely before the next.
            QList<QPainterPath> comps = components(region);
            std::sort(comps.begin(), comps.end(),
                      [](const QPainterPath &a, const QPainterPath &b) {
                          const QPointF ca = a.boundingRect().center();
                          const QPointF cb = b.boundingRect().center();
                          return ca.x() + ca.y() * 0.01 < cb.x() + cb.y() * 0.01;
                      });
            const CutParams flat;   // single full-depth lap per ring
            for (const QPainterPath &comp : comps) {
                double delta = stepover;
                int guard = 0;
                while (guard++ < 2000) {
                    const QList<QPolygonF> rings = insetRings(comp, delta);
                    if (rings.isEmpty())
                        break;
                    const double z = qMax(cp.zBot, -delta / halfTan);
                    for (const QPolygonF &ring : rings) {
                        em.rapidTo(ring.first());
                        em.plungeTo(ring.first(), z);
                        em.followRing(ring, z, flat);
                        em.retract();
                    }
                    if (z <= cp.zBot + 1e-9)
                        break;   // walls at full depth; deeper rings would flat-bottom
                    delta += stepover;
                }
                lastPos = em.pos();
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
        } else { // pocket
            const double stepover = qMax(0.1, j.value("stepover").toDouble(toolR));
            cp.linkDist = qMax(2.5 * stepover, 4 * toolR);
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

        if (!jobs.isEmpty()) {
            for (const Job &job : orderJobs(jobs, lastPos)) {
                em.runJob(job, cp);
                lastPos = em.pos();
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
