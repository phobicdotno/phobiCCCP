#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

// A GRBL post-processor: turns an ordered list of CAM operations into plain
// `.nc` g-code, matching the dialect Carbide Create's own "GRBL" post emits
// (documented in shapeoko-c2d/docs/GCODE-AND-CRYPTO.md). This is the plaintext
// path — the same kind of program CC hands to Carbide Motion — so its output can
// drive a Shapeoko directly over serial with no .egc and no Carbide Motion.
namespace c2d {

struct Op {
    enum Kind { Rapid, Feed, Arc, Spindle, Tool, Comment } kind;
    double x = 0, y = 0, z = 0;   // Rapid/Feed/Arc target (mm)
    double feed = 0;              // Feed rate (mm/min) for Feed/Arc
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
};

// Aggregate statistics over an op list: work extents, cut/rapid distance and a
// feed-rate time estimate — surfaced before a program is streamed to the
// machine so the operator can sanity-check envelope and duration.
struct JobStats {
    double cutLen = 0, rapidLen = 0;   // mm
    double timeSec = 0;                // rapids assumed 5000 mm/min, no accel model
    double minX = 0, maxX = 0, minY = 0, maxY = 0, minZ = 0, maxZ = 0;
    bool hasBounds = false;
};
JobStats computeStats(const QVector<Op> &ops);
QString statsSummary(const JobStats &s);   // two lines: extents / lengths + time

// Dry-run rehearsal transform: every Z word lifted by `lift` mm (a constant
// shift keeps modal Z words consistent) and all spindle lines dropped, so a
// program can be traced in the air above the stock.
QStringList airCutTransform(const QStringList &lines, double lift);

class GrblPost
{
public:
    explicit GrblPost(bool metric = true) : m_metric(metric) {}
    QString generate(const QVector<Op> &ops) const;

private:
    bool m_metric;
};

} // namespace c2d
