#pragma once
#include <QPointF>
#include <QString>
#include <QVector>

namespace c2d {

// Finger-joint box: the flat panels of a box (bottom, front and back sides,
// left and right ends, optional lid) as closed outlines laid out on the
// stock, fingers cut so every joint interlocks. Pure geometry, no document.
//
// Every wall of the box is split between the panels that meet at it: along
// each joint the fingers of one panel are the gaps of the other, and each
// corner block belongs to one panel only (the bottom or lid where it
// touches, otherwise the front/back side), so the parts fill the box walls
// exactly once.
struct BoxSpec {
    double width = 150;       // X, along the front (mm)
    double depth = 100;       // Y, along the ends (mm)
    double height = 60;       // Z (mm)
    bool insideSize = false;  // the sizes are the inside of the box
    double thickness = 6;     // material (mm)
    double fingerWidth = 18;  // wanted finger width; each joint rounds it to an odd count
    double bitDiameter = 3.175; // spacing between parts is at least twice this
    double clearance = 0.1;   // fit gap between mating fingers (mm)
    bool lid = false;
    double stockWidth = 0;    // > 0: start a new row before a part would pass it
};

struct BoxEdge {
    int matePanel = -1;       // -1: free edge (open top)
    int mateEdge = -1;
    // Interior of the edge (between the two corner blocks), split in an odd
    // number of equal parts: true = finger out to the panel's edge, false = gap.
    QVector<bool> fingers;
};

struct BoxPanel {
    QString name;             // "Bottom", "Front", "Back", "Left end", "Right end", "Lid"
    double width = 0;         // outer size of the part (mm)
    double height = 0;
    // Edges in order: bottom, right, top, left of the part as laid out.
    BoxEdge edges[4];
    // Corner blocks in order: bottom-right, top-right, top-left, bottom-left
    // (the end of edge k): true = this part has it.
    bool corners[4] = {false, false, false, false};
    QPointF origin;           // bottom-left of the part on the stock
    QVector<QPointF> outline; // closed (last point not repeated), absolute mm
};

struct BoxResult {
    QVector<BoxPanel> panels;
    double outerW = 0, outerD = 0, outerH = 0;
    QString error;            // non-empty: nothing to draw
};

// Odd number of fingers that best fits `length` with fingers near `wanted`.
int boxFingerCount(double length, double wanted);
// Finger width default for a thickness: about 3 x thickness.
double boxDefaultFinger(double thickness);

BoxResult makeFingerBox(const BoxSpec &spec);

} // namespace c2d
