#include "post_grbl.h"

#include <limits>
#include <QRegularExpression>
#include <QStringList>
#include <cmath>

namespace c2d {

// Modal number formatter: emits `<name><value>` only when the value changes from
// the previous block, mirroring CC's per-axis "last output" memory.
namespace {
struct Modal {
    QString name;
    int digits;
    QString last;
    QString fmt(double v) {
        const QString s = QString::number(v, 'f', digits);
        if (s == last)
            return QString();
        last = s;
        return name + s;
    }
    void reset() { last = QStringLiteral("--"); }
};

QString block(std::initializer_list<QString> words) {
    QStringList parts;
    for (const QString &w : words)
        if (!w.isEmpty())
            parts << w;
    return parts.join(QString());
}
} // namespace

namespace {
MotionLimits g_limits;

// The fastest an axis-limited quantity allows along unit direction u.
double alongDir(const double lim[3], const double u[3])
{
    double v = std::numeric_limits<double>::infinity();
    for (int i = 0; i < 3; ++i)
        if (std::abs(u[i]) > 1e-9)
            v = qMin(v, lim[i] / std::abs(u[i]));
    return v;
}

// One move's time from entry speed v0 to exit v1, cruising at most at vMax
// with acceleration a over len: a trapezoid, or a triangle when short.
double trapezoidSec(double len, double v0, double v1, double vMax, double a)
{
    if (len <= 0)
        return 0;
    if (a <= 0)
        return len / qMax(vMax, 1e-9);
    const double da = qMax(0.0, (vMax * vMax - v0 * v0) / (2 * a));
    const double dd = qMax(0.0, (vMax * vMax - v1 * v1) / (2 * a));
    if (da + dd <= len)
        return (vMax - v0) / a + (vMax - v1) / a + (len - da - dd) / qMax(vMax, 1e-9);
    const double vp = std::sqrt(qMax(qMax(v0 * v0, v1 * v1), (2 * a * len + v0 * v0 + v1 * v1) / 2));
    return (vp - v0) / a + (vp - v1) / a;
}
} // namespace

bool MotionLimits::operator==(const MotionLimits &o) const
{
    for (int i = 0; i < 3; ++i)
        if (rate[i] != o.rate[i] || accel[i] != o.accel[i])
            return false;
    return junctionDev == o.junctionDev;
}

MotionLimits motionLimits() { return g_limits; }
void setMotionLimits(const MotionLimits &m) { g_limits = m; }

MotionPlanner::MotionPlanner(const MotionLimits &m) : m_lim(m)
{
    for (int i = 0; i < 3; ++i) {
        if (m_lim.rate[i] <= 0)
            m_lim.rate[i] = 5000;
        if (m_lim.accel[i] <= 0)
            m_lim.accel[i] = i == 2 ? 200 : 400;
        m_lim.rate[i] /= 60.0;   // mm/s from here on
    }
    if (m_lim.junctionDev < 0)
        m_lim.junctionDev = 0.010;
}

void MotionPlanner::stop()
{
    m_atRest = true;
}

int MotionPlanner::add(double len, const double u0[3], const double u1[3], double vMax, double acc)
{
    double entry = 0;
    if (!m_atRest) {
        // GRBL's junction deviation: the corner is taken at the speed a
        // circle of radius-like deviation $11 allows with the acceleration
        // along the change of direction.
        const double cosT = -(m_prevDir[0] * u0[0] + m_prevDir[1] * u0[1] + m_prevDir[2] * u0[2]);
        const double cap = qMin(vMax, m_prevVMax);
        if (cosT > 0.999999) {
            entry = 0;                   // a full reversal
        } else if (cosT < -0.999999) {
            entry = cap;                 // straight on
        } else {
            double j[3] = {u0[0] - m_prevDir[0], u0[1] - m_prevDir[1], u0[2] - m_prevDir[2]};
            const double jl = std::sqrt(j[0] * j[0] + j[1] * j[1] + j[2] * j[2]);
            for (double &c : j)
                c /= jl;
            const double aj = alongDir(m_lim.accel, j);
            const double sinD2 = std::sqrt(0.5 * (1.0 - cosT));
            entry = qMin(cap, std::sqrt(aj * m_lim.junctionDev * sinD2 / (1.0 - sinD2)));
        }
    }
    m_len.push_back(len);
    m_acc.push_back(acc);
    m_vMax.push_back(vMax);
    m_entryMax.push_back(entry);
    for (int i = 0; i < 3; ++i)
        m_prevDir[i] = u1[i];
    m_prevVMax = vMax;
    m_atRest = false;
    return int(m_len.size()) - 1;
}

int MotionPlanner::line(double dx, double dy, double dz, double feed)
{
    const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1e-9)
        return -1;
    const double u[3] = {dx / len, dy / len, dz / len};
    double v = alongDir(m_lim.rate, u);
    if (feed > 0)
        v = qMin(v, feed / 60.0);
    return add(len, u, u, v, alongDir(m_lim.accel, u));
}

int MotionPlanner::arc(double px, double py, double pz, double x, double y, double z,
                       double ci, double cj, bool cw, double feed)
{
    const double kTau = 6.283185307179586;
    const double cx = px + ci, cy = py + cj;
    const double r = std::hypot(ci, cj);
    if (r < 1e-9)
        return line(x - px, y - py, z - pz, feed);
    const double a0 = std::atan2(py - cy, px - cx);
    const double a1 = std::atan2(y - cy, x - cx);
    double sweep = cw ? a0 - a1 : a1 - a0;
    while (sweep <= 1e-9) sweep += kTau;
    while (sweep > kTau)  sweep -= kTau;
    const double planar = r * sweep, dz = z - pz;
    const double len = std::hypot(planar, dz);
    if (len < 1e-9)
        return -1;
    // Tangents at both ends (the XY part turns with the arc, Z climbs evenly).
    const double s = cw ? -1.0 : 1.0, kxy = planar / len, kz = dz / len;
    const double u0[3] = {-s * std::sin(a0) * kxy, s * std::cos(a0) * kxy, kz};
    const double u1[3] = {-s * std::sin(a1) * kxy, s * std::cos(a1) * kxy, kz};
    const double aXY = qMin(m_lim.accel[0], m_lim.accel[1]);
    const double up[3] = {0, 0, 1};
    double acc = aXY;
    if (std::abs(kz) > 1e-9)
        acc = qMin(acc / qMax(kxy, 1e-9), alongDir(m_lim.accel, up) / std::abs(kz));
    double v = qMin(m_lim.rate[0], m_lim.rate[1]) / qMax(kxy, 1e-9);
    if (std::abs(kz) > 1e-9)
        v = qMin(v, m_lim.rate[2] / std::abs(kz));
    if (feed > 0)
        v = qMin(v, feed / 60.0);
    v = qMin(v, std::sqrt(aXY * r) / qMax(kxy, 1e-9));   // the axes' turn
    return add(len, u0, u1, v, acc);
}

QVector<double> MotionPlanner::times() const
{
    const int n = size();
    QVector<double> t(n, 0.0);
    if (n == 0)
        return t;
    std::vector<double> entry(m_entryMax);
    // Backward: every move can still stop by the end of the list.
    double exitV = 0;
    for (int i = n - 1; i >= 0; --i) {
        entry[i] = qMin(entry[i], std::sqrt(exitV * exitV + 2 * m_acc[i] * m_len[i]));
        exitV = entry[i];
    }
    // Forward: no move enters faster than the one before could speed up to.
    for (int i = 0; i + 1 < n; ++i)
        entry[i + 1] = qMin(entry[i + 1], std::sqrt(entry[i] * entry[i] + 2 * m_acc[i] * m_len[i]));
    for (int i = 0; i < n; ++i)
        t[i] = trapezoidSec(m_len[i], entry[i], i + 1 < n ? entry[i + 1] : 0.0, m_vMax[i], m_acc[i]);
    return t;
}

double MotionPlanner::total() const
{
    double sum = 0;
    for (double v : times())
        sum += v;
    return sum;
}

JobStats computeStats(const QVector<Op> &ops)
{
    JobStats s;
    MotionPlanner plan;
    const double kTau = 6.283185307179586;
    double px = 0, py = 0, pz = 0;
    bool have = false;
    auto grow = [&s](double x, double y, double z) {
        if (!s.hasBounds) {
            s.minX = s.maxX = x;
            s.minY = s.maxY = y;
            s.minZ = s.maxZ = z;
            s.hasBounds = true;
            return;
        }
        s.minX = qMin(s.minX, x); s.maxX = qMax(s.maxX, x);
        s.minY = qMin(s.minY, y); s.maxY = qMax(s.maxY, y);
        s.minZ = qMin(s.minZ, z); s.maxZ = qMax(s.maxZ, z);
    };
    bool spinning = false;
    for (const Op &op : ops) {
        if (op.kind == Op::Dwell || op.kind == Op::Spindle || op.kind == Op::Tool)
            plan.stop();   // GRBL finishes its moves before these
        if (op.kind == Op::Dwell)
            s.timeSec += qMax(0.0, op.feed);
        // The post's spin-up wait: every start from standstill, and a tool
        // change stops the spindle.
        if (op.kind == Op::Spindle) {
            if (op.ival > 0 && !spinning)
                s.timeSec += kSpindleSpinUpSec;
            spinning = op.ival > 0;
        } else if (op.kind == Op::Tool) {
            spinning = false;
        }
        if (op.kind != Op::Rapid && op.kind != Op::Feed && op.kind != Op::Arc)
            continue;
        grow(op.x, op.y, op.z);
        if (have) {
            double len = 0;
            if (op.kind == Op::Arc) {
                const double cx = px + op.ci, cy = py + op.cj;
                const double r = std::hypot(op.ci, op.cj);
                const double a0 = std::atan2(py - cy, px - cx);
                const double a1 = std::atan2(op.y - cy, op.x - cx);
                double sweep = op.cw ? a0 - a1 : a1 - a0;
                while (sweep <= 1e-9) sweep += kTau;   // coincident endpoints = full circle
                while (sweep > kTau)  sweep -= kTau;
                len = std::hypot(r * sweep, op.z - pz);
                // Sample the arc so a ring's full extents (not just its
                // endpoints) enter the bounds.
                for (int i = 1; i < 8; ++i) {
                    const double a = a0 + (op.cw ? -sweep : sweep) * i / 8.0;
                    grow(cx + r * std::cos(a), cy + r * std::sin(a), op.z);
                }
            } else {
                len = std::hypot(std::hypot(op.x - px, op.y - py), op.z - pz);
            }
            if (op.kind == Op::Rapid)
                s.rapidLen += len;
            else
                s.cutLen += len;
            if (op.kind == Op::Arc && op.feed > 0)
                plan.arc(px, py, pz, op.x, op.y, op.z, op.ci, op.cj, op.cw, op.feed);
            else if (op.kind == Op::Rapid || (op.kind == Op::Feed && op.feed > 0))
                plan.line(op.x - px, op.y - py, op.z - pz, op.kind == Op::Rapid ? 0.0 : op.feed);
        }
        px = op.x; py = op.y; pz = op.z;
        have = true;
    }
    s.timeSec += plan.total();
    return s;
}

QString statsSummary(const JobStats &s)
{
    if (!s.hasBounds)
        return QStringLiteral("empty program");
    const int t = int(s.timeSec + 0.5);
    return QStringLiteral("X %1..%2  Y %3..%4  Z %5..%6 mm\n"
                          "cut %7 mm + rapid %8 mm, est. %9:%10 min")
        .arg(s.minX, 0, 'f', 1).arg(s.maxX, 0, 'f', 1)
        .arg(s.minY, 0, 'f', 1).arg(s.maxY, 0, 'f', 1)
        .arg(s.minZ, 0, 'f', 1).arg(s.maxZ, 0, 'f', 1)
        .arg(s.cutLen, 0, 'f', 0).arg(s.rapidLen, 0, 'f', 0)
        .arg(t / 60).arg(t % 60, 2, 10, QChar('0'));
}

QHash<QString, double> timeByComment(const QVector<Op> &ops)
{
    QHash<QString, double> secByName;
    QVector<Op> prefix;   // where the machine is when a run starts
    QString cur;
    int start = 0, rpm = 0;
    bool havePos = false;
    Op pos = Op::rapid(0, 0, 0);
    auto close = [&](int end) {
        if (cur.isEmpty() || end <= start)
            return;
        QVector<Op> run = prefix;
        run += ops.mid(start, end - start);
        secByName[cur] += computeStats(run).timeSec - computeStats(prefix).timeSec;
    };
    for (int i = 0; i < ops.size(); ++i) {
        const Op &op = ops.at(i);
        if (op.kind == Op::Comment) {
            close(i);
            cur = op.text;
            start = i;
            // The machine's state carried in, so the run is charged for
            // its first travel and not for a spin-up that never happens.
            prefix.clear();
            if (rpm > 0)
                prefix.append(Op::spindle(rpm));
            if (havePos)
                prefix.append(pos);
        } else if (op.kind == Op::Rapid || op.kind == Op::Feed || op.kind == Op::Arc) {
            pos = Op::rapid(op.x, op.y, op.z);
            havePos = true;
        } else if (op.kind == Op::Spindle) {
            rpm = op.ival;
        } else if (op.kind == Op::Tool) {
            rpm = 0;
        }
    }
    close(ops.size());
    return secByName;
}

QString roughDuration(double sec)
{
    const int m = int(std::lround(qMax(0.0, sec) / 60.0));
    if (m < 1)
        return QStringLiteral("< 1 min");
    if (m < 60)
        return QStringLiteral("\u2248 %1 min").arg(m);
    if (m % 60 == 0)
        return QStringLiteral("\u2248 %1 h").arg(m / 60);
    return QStringLiteral("\u2248 %1 h %2 min").arg(m / 60).arg(m % 60);
}

QStringList airCutTransform(const QStringList &lines, double lift)
{
    static const QRegularExpression zWord(QStringLiteral("Z(-?[0-9]+\\.?[0-9]*)"));
    QStringList out;
    bool afterStart = false;
    for (const QString &raw : lines) {
        const QString l = raw.trimmed();
        // The spin-up wait right after a dropped start has nothing to wait for.
        const bool spinUp = afterStart && l.startsWith(QLatin1String("G4P"));
        afterStart = l.startsWith(QLatin1String("M03")) || l.startsWith(QLatin1String("M04"));
        if (spinUp)
            continue;
        // Drop spindle control: M03/M04 starts, bare S rpm changes, M05 stops.
        // (`M0 ;T` tool-change pauses survive — they contain no motion.)
        if (afterStart || l == QLatin1String("M05")
            || (l.size() > 1 && l.at(0) == QChar('S') && l.at(1).isDigit()))
            continue;
        if (l.startsWith(QChar('(')) || l.startsWith(QChar(';'))) {
            out << raw;   // comments untouched — a name could contain "Z1"
            continue;
        }
        QString shifted;
        int pos = 0;
        auto it = zWord.globalMatch(raw);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            shifted += raw.mid(pos, m.capturedStart() - pos);
            shifted += QStringLiteral("Z")
                + QString::number(m.captured(1).toDouble() + lift, 'f', 3);
            pos = m.capturedEnd();
        }
        shifted += raw.mid(pos);
        out << shifted;
    }
    return out;
}

QStringList coolantTransform(const QStringList &lines, bool mist)
{
    // Word M followed by a number: "M3", "M03 S12000", "G90 M5".
    static const QRegularExpression mWord(QStringLiteral("(?:^|[^A-Z])M0*([0-9]+)(?![0-9.])"));
    auto codesOf = [](const QString &raw) {
        QString code = raw.trimmed().toUpper();
        const int semi = code.indexOf(QChar(';'));
        if (semi >= 0)
            code = code.left(semi);
        if (code.startsWith(QChar('(')))
            code.clear();
        QVector<int> m;
        auto it = mWord.globalMatch(code);
        while (it.hasNext())
            m << it.next().captured(1).toInt();
        return m;
    };
    for (const QString &raw : lines) {
        const QVector<int> m = codesOf(raw);
        if (m.contains(7) || m.contains(8) || m.contains(9))
            return lines;   // the program switches it itself
    }
    const QString on = mist ? QStringLiteral("M7") : QStringLiteral("M8");
    QStringList out;
    bool running = false;
    for (const QString &raw : lines) {
        const QVector<int> m = codesOf(raw);
        if (m.contains(5) && running) {
            out << QStringLiteral("M9");
            running = false;
        }
        out << raw;
        if ((m.contains(3) || m.contains(4)) && !running) {
            out << on;
            running = true;
        }
    }
    return out;
}

QStringList safeRapidsTransform(const QStringList &lines)
{
    static const QRegularExpression word(QStringLiteral("([A-Z])\\s*(-?[0-9]*\\.?[0-9]+)"));
    QStringList out;
    int modal = -1;                                     // last G0..G3
    double z = std::numeric_limits<double>::infinity(); // unknown = at the top
    for (const QString &raw : lines) {
        const QString l = raw.trimmed();
        if (l.isEmpty() || l.startsWith(QChar('(')) || l.startsWith(QChar(';'))) {
            out << raw;
            continue;
        }
        if (l.startsWith(QLatin1String("M0")) && l.contains(QLatin1String(";T"))) {
            // Without a BitSetter the operator changes the bit and resumes
            // wherever they jogged it to, often just above the stock: Z is
            // unknown, so the next rapid goes up before it goes across.
            z = -std::numeric_limits<double>::infinity();
            out << raw;
            continue;
        }
        QString code = l;
        const int semi = code.indexOf(QChar(';'));
        if (semi >= 0)
            code = code.left(semi);
        bool onlyMotionWords = true, hasXY = false, hasZ = false;
        QString xs, ys, zs;
        double zt = 0;
        auto it = word.globalMatch(code);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            const QChar letter = m.captured(1).at(0);
            const QString val = m.captured(2);
            if (letter == QChar('G')) {
                const double g = val.toDouble();
                if (g == 0 || g == 1 || g == 2 || g == 3)
                    modal = int(g);
                else
                    onlyMotionWords = false;
            } else if (letter == QChar('X')) {
                xs = val; hasXY = true;
            } else if (letter == QChar('Y')) {
                ys = val; hasXY = true;
            } else if (letter == QChar('Z')) {
                zs = val; hasZ = true; zt = val.toDouble();
            } else {
                onlyMotionWords = false;
            }
        }
        if (modal == 0 && hasXY && hasZ && onlyMotionWords) {
            QString xy = QStringLiteral("G0");
            if (!xs.isEmpty())
                xy += QStringLiteral("X") + xs;
            if (!ys.isEmpty())
                xy += QStringLiteral("Y") + ys;
            const QString zl = QStringLiteral("G0Z") + zs;
            if (zt < z)
                out << xy << zl;     // across, then down
            else
                out << zl << xy;     // up, then across
        } else {
            out << raw;
        }
        if (hasZ)
            z = zt;
    }
    return out;
}

QString GrblPost::generate(const QVector<Op> &ops) const
{
    const int coordDigits = m_metric ? 3 : 4;
    Modal X{QStringLiteral("X"), coordDigits, {}};
    Modal Y{QStringLiteral("Y"), coordDigits, {}};
    Modal Z{QStringLiteral("Z"), coordDigits, {}};
    Modal F{QStringLiteral("F"), m_metric ? 1 : 3, {}};   // inch feeds are small numbers: one decimal was up to 1.3 % off
    Modal G{QStringLiteral("G"), 0, {}};

    QStringList out;
    // NB: `emit` is a Qt keyword macro (empty), so the lambda is named `line`.
    auto line = [&](const QString &s) { out << s; };

    // onOpen
    line(QStringLiteral("G90"));
    line(m_metric ? QStringLiteral("G21") : QStringLiteral("G20"));

    bool spindleOn = false;
    int lastRpm = -1;
    int lastTool = -1;
    QString lastToolKey;

    auto scale = [&](double mm) { return m_metric ? mm : mm / 25.4; };

    // The height the next rapid travels at, when the program moves there
    // before it cuts: put on a line of its own right after a tool change
    // (and at the start), so the bit is lifted clear before the spindle
    // starts and before anything moves across.
    auto clearZ = [&](int from) {
        for (int k = from; k < ops.size(); ++k) {
            const Op::Kind kind = ops.at(k).kind;
            if (kind == Op::Rapid)
                line(block({G.fmt(0), Z.fmt(scale(ops.at(k).z))}));
            if (kind == Op::Rapid || kind == Op::Feed || kind == Op::Arc || kind == Op::Tool)
                return;
        }
    };
    clearZ(0);

    for (int i = 0; i < ops.size(); ++i) {
        const Op &op = ops.at(i);
        switch (op.kind) {
        case Op::Comment: {
            QString t = op.text;
            // GRBL ends a comment at the first ')': "(Pocket (12))" leaves a
            // stray ')' that other senders stream and GRBL rejects.
            for (QChar &ch : t)
                if (ch == u'(' || ch == u')' || ch == u'\n' || ch == u'\r')
                    ch = u' ';
            if (t.length() > 12)
                t = t.left(9) + QStringLiteral("...");
            line(QStringLiteral("(") + t + QStringLiteral(")"));
            break;
        }
        case Op::Spindle: {
            if (op.ival <= 0) {
                if (spindleOn) { line(QStringLiteral("M05")); spindleOn = false; }
            } else if (!spindleOn || op.ival != lastRpm) {
                QString ln;
                if (!spindleOn) ln += QStringLiteral("M03");
                const bool starting = !spindleOn;
                ln += QStringLiteral("S") + QString::number(op.ival);
                line(ln);
                // From standstill: wait for the spindle to reach speed
                // before anything moves into the stock.
                // Speeding up a running spindle (same tool, higher rpm) takes
                // part of that time too.
                if (starting)
                    line(QStringLiteral("G4P") + QString::number(kSpindleSpinUpSec, 'f', 2));
                else if (op.ival > lastRpm)
                    line(QStringLiteral("G4P") + QString::number(
                        kSpindleSpinUpSec * double(op.ival - lastRpm) / op.ival, 'f', 2));
                spindleOn = true;
                lastRpm = op.ival;
            }
            break;
        }
        case Op::Tool: {
            // The same number with another bit (Op::text differs) is a change too.
            if (op.ival != lastTool || op.text != lastToolKey) {
                lastTool = op.ival;
                lastToolKey = op.text;
                if (spindleOn) { line(QStringLiteral("M05")); spindleOn = false; }
                // GRBL post: pause for a manual tool change, tool number in comment.
                line(QStringLiteral("M0 ;T") + QString::number(op.ival));
                // A probe macro that failed half way (ALARM on G38.2) can
                // leave GRBL in G91; "Continue anyway" must not run the rest
                // of the job as relative moves.
                line(m_metric ? QStringLiteral("G90G21") : QStringLiteral("G90G20"));
                // The sender runs a probe macro here (G53 moves, its own F),
                // so nothing modal survives: the next move must carry every word.
                X.last.clear(); Y.last.clear(); Z.last.clear(); F.last.clear(); G.last.clear();
                clearZ(i + 1);
            }
            break;
        }
        case Op::Dwell:
            // GRBL's G4 P is in seconds.
            if (op.feed > 0)
                line(QStringLiteral("G4P") + QString::number(op.feed, 'f', 2));
            break;
        case Op::Rapid:
            line(block({G.fmt(0), X.fmt(scale(op.x)), Y.fmt(scale(op.y)), Z.fmt(scale(op.z))}));
            break;
        case Op::Feed:
            line(block({G.fmt(1), X.fmt(scale(op.x)), Y.fmt(scale(op.y)),
                        Z.fmt(scale(op.z)), F.fmt(scale(op.feed))}));
            break;
        case Op::Arc: {
            // I/J are relative center offsets and must always be emitted; the
            // endpoint X/Y must also always be present (a modal-suppressed X/Y
            // on an arc would leave the target ambiguous), so bypass the modal
            // memory for them but keep it updated.
            const int cd = m_metric ? 3 : 4;
            QString ln = G.fmt(op.cw ? 2 : 3)
                + QStringLiteral("X") + QString::number(scale(op.x), 'f', cd)
                + QStringLiteral("Y") + QString::number(scale(op.y), 'f', cd);
            X.last = QString::number(scale(op.x), 'f', cd);
            Y.last = QString::number(scale(op.y), 'f', cd);
            const QString zw = Z.fmt(scale(op.z));
            ln += zw;
            ln += QStringLiteral("I") + QString::number(scale(op.ci), 'f', cd)
                + QStringLiteral("J") + QString::number(scale(op.cj), 'f', cd)
                + F.fmt(scale(op.feed));
            line(ln);
            break;
        }
        }
    }

    // onClose
    if (spindleOn)
        line(QStringLiteral("M05"));
    line(QStringLiteral("M02"));

    return out.join(QChar('\n')) + QChar('\n');
}

} // namespace c2d
