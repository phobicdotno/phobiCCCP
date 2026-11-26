#pragma once
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>
#include <vector>

// A GRBL post-processor: turns an ordered list of CAM operations into plain
// `.nc` g-code, matching the dialect Carbide Create's own "GRBL" post emits
// (documented in shapeoko-c2d/docs/GCODE-AND-CRYPTO.md). This is the plaintext
// path — the same kind of program CC hands to Carbide Motion — so its output can
// drive a Shapeoko directly over serial with no .egc and no Carbide Motion.
namespace c2d {

// Seconds GRBL waits after an M03 that starts the spindle from standstill
// (start of the job, after a tool change) before the first move, so the bit
// is at speed before it plunges. A speed change on a running spindle does
// not wait.
constexpr double kSpindleSpinUpSec = 3.0;

struct Op {
    enum Kind { Rapid, Feed, Arc, Spindle, Tool, Comment, Dwell } kind;
    double x = 0, y = 0, z = 0;   // Rapid/Feed/Arc target (mm)
    double feed = 0;              // Feed rate (mm/min) for Feed/Arc; seconds for Dwell
    int ival = 0;                 // Spindle rpm, or Tool number
    QString text;                 // Comment text
    double ci = 0, cj = 0;        // Arc: center offset from start (I, J)
    bool cw = false;              // Arc: G2 (clockwise) vs G3

    static Op rapid(double x, double y, double z) { return {Rapid, x, y, z, 0, 0, {}, 0, 0, false}; }
    static Op feedTo(double x, double y, double z, double f) { return {Feed, x, y, z, f, 0, {}, 0, 0, false}; }
    static Op arcTo(double x, double y, double z, double i, double j, bool cw, double f)
    { return {Arc, x, y, z, f, 0, {}, i, j, cw}; }
    static Op spindle(int rpm) { Op o; o.kind = Spindle; o.ival = rpm; return o; }
    static Op tool(int n)      { Op o; o.kind = Tool;    o.ival = n;   return o; }
    static Op comment(const QString &t) { Op o; o.kind = Comment; o.text = t; return o; }
    // Pause in place (G4), e.g. at the bottom of a drilled hole.
    static Op dwell(double sec) { Op o; o.kind = Dwell; o.feed = sec; return o; }
};

// Aggregate statistics over an op list: work extents, cut/rapid distance and a
// feed-rate time estimate — surfaced before a program is streamed to the
// machine so the operator can sanity-check envelope and duration.
struct JobStats {
    double cutLen = 0, rapidLen = 0;   // mm
    double timeSec = 0;                // planned like GRBL: feeds, rapids, acceleration, corners
    double minX = 0, maxX = 0, minY = 0, maxY = 0, minZ = 0, maxZ = 0;
    bool hasBounds = false;
};
// What the machine can do, as GRBL's $$ says: the time estimates plan the
// moves with it the way GRBL's planner does. Unknown settings keep the
// Shapeoko defaults the app assumes (rapids 5000 mm/min).
struct MotionLimits {
    double rate[3] = {5000, 5000, 5000};   // $110-$112, max rate per axis (mm/min)
    double accel[3] = {400, 400, 200};     // $120-$122, acceleration per axis (mm/s²)
    double junctionDev = 0.010;            // $11, junction deviation (mm)
    bool operator==(const MotionLimits &o) const;
    bool operator!=(const MotionLimits &o) const { return !(*this == o); }
};
// The limits every estimate uses: the machine's last $$ read, kept in the
// machine profile so they hold offline too.
MotionLimits motionLimits();
void setMotionLimits(const MotionLimits &m);

// GRBL-like time planner: moves are added in order, each runs at most at its
// feed and the axes' max rates, speeds up and slows down with the axes'
// acceleration, and takes corners at the junction-deviation speed. times()
// plans the whole list (a backward and a forward pass, O(n)).
class MotionPlanner {
public:
    explicit MotionPlanner(const MotionLimits &m = motionLimits());
    // A straight move by (dx, dy, dz) mm at `feed` mm/min, a rapid when feed
    // <= 0. Returns the move's index, -1 for a move of no length.
    int line(double dx, double dy, double dz, double feed);
    // G2/G3 from (px,py,pz) to (x,y,z) around the center (px+ci, py+cj).
    // Run at most at sqrt(a*r), the speed the axes can turn it at.
    int arc(double px, double py, double pz, double x, double y, double z,
            double ci, double cj, bool cw, double feed);
    void stop();                     // the machine comes to rest (dwell, spindle, M0)
    int size() const { return int(m_len.size()); }
    QVector<double> times() const;   // seconds per move
    double total() const;

private:
    int add(double len, const double u0[3], const double u1[3], double vMax, double acc);
    MotionLimits m_lim;
    std::vector<double> m_len, m_acc, m_vMax, m_entryMax;
    double m_prevDir[3] = {0, 0, 0};
    double m_prevVMax = 0;
    bool m_atRest = true;
};

JobStats computeStats(const QVector<Op> &ops);
QString statsSummary(const JobStats &s);   // two lines: extents / lengths + time

// computeStats' time split by toolpath: each toolpath's ops open with a
// comment carrying its name, and every op up to the next comment is charged
// to it (pooled toolpaths come in several runs, which add up). A run starts
// where the machine is, so it pays its first travel but not a spin-up the
// spindle already had. Keyed by the comment text (the toolpath's name).
QHash<QString, double> timeByComment(const QVector<Op> &ops);

// A time estimate in plain words for a list: "< 1 min", "≈ 4 min",
// "≈ 1 h 12 min".
QString roughDuration(double sec);

// Dry-run rehearsal transform: every Z word lifted by `lift` mm (a constant
// shift keeps modal Z words consistent) and all spindle lines dropped, so a
// program can be traced in the air above the stock.
QStringList airCutTransform(const QStringList &lines, double lift);

// Rapids never move down and sideways at once: a G0 with both XY and Z is
// split so the machine rises first and then travels, or travels first and
// then descends. Z starts out as "at the top" (the sender rises there before
// the program), so the first move into a job is XY across, then Z down.
// After every `M0 ;T` tool change it is unknown - without a BitSetter the
// bit is wherever the operator jogged it - so the next rapid rises first.
QStringList safeRapidsTransform(const QStringList &lines);

// Dust collector / coolant switched with the spindle (GRBL M8 flood, M7 mist,
// M9 off; a relay on the flood pin often runs the dust collector): the
// on-code right after every spindle start (M3/M4, so after a tool change
// too) and M9 right before every spindle stop (M5), so the end of the job
// and the park for a tool change happen with it off. A program that already
// switches it itself is left alone.
QStringList coolantTransform(const QStringList &lines, bool mist = false);

class GrblPost
{
public:
    explicit GrblPost(bool metric = true) : m_metric(metric) {}
    QString generate(const QVector<Op> &ops) const;

private:
    bool m_metric;
};

} // namespace c2d
