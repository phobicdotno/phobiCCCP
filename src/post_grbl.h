#pragma once
#include <QString>
#include <QVector>

// A GRBL post-processor: turns an ordered list of CAM operations into plain
// `.nc` g-code, matching the dialect Carbide Create's own "GRBL" post emits
// (documented in shapeoko-c2d/docs/GCODE-AND-CRYPTO.md). This is the plaintext
// path - the same kind of program CC hands to Carbide Motion - so its output can
// drive a Shapeoko directly over serial with no .egc and no Carbide Motion.
namespace c2d {

struct Op {
    enum Kind { Rapid, Feed, Spindle, Tool, Comment } kind;
    double x = 0, y = 0, z = 0;   // Rapid/Feed target (mm)
    double feed = 0;              // Feed rate (mm/min) for Feed
    int ival = 0;                 // Spindle rpm, or Tool number
    QString text;                 // Comment text

    static Op rapid(double x, double y, double z) { return {Rapid, x, y, z, 0, 0, {}}; }
    static Op feedTo(double x, double y, double z, double f) { return {Feed, x, y, z, f, 0, {}}; }
    static Op spindle(int rpm) { Op o; o.kind = Spindle; o.ival = rpm; return o; }
    static Op tool(int n)      { Op o; o.kind = Tool;    o.ival = n;   return o; }
    static Op comment(const QString &t) { Op o; o.kind = Comment; o.text = t; return o; }
};

class GrblPost
{
public:
    explicit GrblPost(bool metric = true) : m_metric(metric) {}
    QString generate(const QVector<Op> &ops) const;

private:
    bool m_metric;
};

} // namespace c2d
