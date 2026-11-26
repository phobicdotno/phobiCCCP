#include "probecycles.h"

#include <algorithm>
#include <cmath>

namespace c2d {

static QString mm(double v) { return QString::number(v, 'f', 3); }
static QString feed(double v) { return QString::number(v, 'f', 0); }

QStringList holeSearchLines(char axis, double search, const ProbeFeeds &f, double startAbs)
{
    const QChar a = QChar::fromLatin1(axis);
    const QString far = mm(search), start = mm(startAbs);
    // Started off the middle, the wall can be closer than any back-off: go
    // back the way the pin came, to the start, never a set distance.
    return {QStringLiteral("G91"),
            // + wall (G38.3: a miss is reported, not an alarm)
            QStringLiteral("G38.3 %1%2 F%3").arg(a, far, feed(f.fastFeed)),
            QStringLiteral("G90"),
            QStringLiteral("G53 G0 %1%2").arg(a, start),
            QStringLiteral("G91"),
            // - wall
            QStringLiteral("G38.3 %1-%2 F%3").arg(a, far, feed(f.fastFeed)),
            QStringLiteral("G90"),
            QStringLiteral("G53 G0 %1%2").arg(a, start)};
}

QStringList holeTouchLines(char axis, double plus, double minus, double startAbs, const ProbeFeeds &f)
{
    const QChar a = QChar::fromLatin1(axis);
    // The search travelled the whole chord: half of it is free on each side.
    const double b = std::max(0.0, std::min(f.retract, (plus - minus) / 2.0));
    const QString again = mm(b + 2.0), start = mm(startAbs);
    return {QStringLiteral("G90"),
            QStringLiteral("G53 G0 %1%2").arg(a, mm(plus - b)),
            QStringLiteral("G91"),
            QStringLiteral("G38.2 %1%2 F%3").arg(a, again, feed(f.slowFeed)),
            QStringLiteral("G90"),
            QStringLiteral("G53 G0 %1%2").arg(a, start),
            QStringLiteral("G53 G0 %1%2").arg(a, mm(minus + b)),
            QStringLiteral("G91"),
            QStringLiteral("G38.2 %1-%2 F%3").arg(a, again, feed(f.slowFeed)),
            QStringLiteral("G90"),
            QStringLiteral("G53 G0 %1%2").arg(a, start)};
}

QStringList bossProbeLines(char axis, double diameter, double clearance, double depth,
                           const ProbeFeeds &f, double startAbs)
{
    const QChar a = QChar::fromLatin1(axis);
    const double outD = diameter / 2.0 + clearance;
    // Toward the boss: the clearance and as much again into a boss that is
    // smaller than entered, never past its middle.
    const double inD = std::min(outD, clearance * 2.0 + 2.0);
    const QString out = mm(outD), in = mm(inD), down = mm(depth);
    const QString back = mm(f.retract), again = mm(f.retract + 2.0);
    QStringList l{QStringLiteral("G91")};
    for (int s : {1, -1}) {
        const QString o = s > 0 ? QString() : QStringLiteral("-");   // outward
        const QString t = s > 0 ? QStringLiteral("-") : QString();   // toward the boss
        l << QStringLiteral("G0 %1%2%3").arg(a, o, out)
          << QStringLiteral("G0 Z-%1").arg(down)
          << QStringLiteral("G38.3 %1%2%3 F%4").arg(a, t, in, feed(f.fastFeed))
          << QStringLiteral("G0 %1%2%3").arg(a, o, back)
          << QStringLiteral("G38.2 %1%2%3 F%4").arg(a, t, again, feed(f.slowFeed))
          << QStringLiteral("G0 %1%2%3").arg(a, o, back)
          << QStringLiteral("G0 Z%1").arg(down)
          // back over the start: the contact, not the start, is where the
          // touch left the bit
          << QStringLiteral("G90")
          << QStringLiteral("G53 G0 %1%2").arg(a, mm(startAbs))
          << QStringLiteral("G91");
    }
    l << QStringLiteral("G90");
    return l;
}

QStringList plateProbeLines(double travel, const ProbeFeeds &f)
{
    return {QStringLiteral("G91"),
            QStringLiteral("G38.3 Z-%1 F%2").arg(mm(travel), feed(f.fastFeed)),
            QStringLiteral("G0 Z%1").arg(mm(f.retract)),
            QStringLiteral("G38.2 Z-%1 F%2").arg(mm(f.retract + 2.0), feed(f.slowFeed)),
            QStringLiteral("G0 Z5"),
            QStringLiteral("G90")};
}

ProbeCentre probeCentre(double xPlus, double xMinus, double yPlus, double yMinus,
                        double tipDiameter, bool boss)
{
    ProbeCentre c;
    c.x = (xPlus + xMinus) / 2.0;
    c.y = (yPlus + yMinus) / 2.0;
    const double tip = boss ? -tipDiameter : tipDiameter;
    c.diameterX = std::abs(xPlus - xMinus) + tip;
    c.diameterY = std::abs(yPlus - yMinus) + tip;
    return c;
}

} // namespace c2d
