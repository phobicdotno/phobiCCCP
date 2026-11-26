#include "chipload.h"
#include "toollibrary.h"
#include <cmath>

namespace c2d {

double chiploadOf(double feed, double rpm, int flutes)
{
    if (feed <= 0 || rpm <= 0 || flutes <= 0)
        return 0;
    return feed / (rpm * flutes);
}

ChiploadRange chiploadRangeFrom(double libFeed, double libRpm, int flutes)
{
    ChiploadRange r;
    const double c = chiploadOf(libFeed, libRpm, flutes);
    if (c <= 0)
        return r;
    r.mid = c;
    r.low = c * 0.6;
    r.high = c * 1.4;
    r.fromLibrary = true;
    return r;
}

ChiploadRange chiploadTable(double diameter, const QString &materialId)
{
    // Middle chipload in mm/tooth for a light router like a Shapeoko
    // (lower than the cutter makers' charts for big machines), by bit Ø:
    //                 <=1.6 mm  <=3.2 mm  <=6.35 mm  bigger
    //   wood / MDF      0.015     0.035     0.060     0.080
    //   plastics        0.020     0.040     0.070     0.090   (acrylic, HDPE, PVC)
    //   aluminium       0.008     0.018     0.030     0.040
    // The #102 1/8" 2-flute in MDF from the library (1270 mm/min at 18000 rpm)
    // is 0.035, the same as the table. The range is ±40 % around it.
    static const double wood[] = {0.015, 0.035, 0.060, 0.080};
    static const double plastic[] = {0.020, 0.040, 0.070, 0.090};
    static const double alu[] = {0.008, 0.018, 0.030, 0.040};
    ChiploadRange r;
    if (diameter <= 0)
        return r;
    const int i = diameter <= 1.6 ? 0 : diameter <= 3.2 ? 1 : diameter <= 6.35 ? 2 : 3;
    const QString m = materialId.toLower();
    const double *row = m == QLatin1String("aluminium") || m == QLatin1String("aluminum") ? alu
                      : (m == QLatin1String("acrylic") || m == QLatin1String("hdpe")
                         || m == QLatin1String("pvc")) ? plastic
                                                       : wood;
    r.mid = row[i];
    r.low = r.mid * 0.6;
    r.high = r.mid * 1.4;
    return r;
}

ChiploadRange chiploadRangeFor(const ToolLibrary *lib, int toolNumber, double diameter,
                               int flutes, const QString &materialId)
{
    if (lib) {
        const LibraryTool *t = lib->byNumber(toolNumber);
        // Same number but another size: a user's own tool, not this one.
        if (t && std::fabs(t->diameter - diameter) < 0.01) {
            if (const ToolFeeds *f = t->feedsFor(materialId); f && f->valid()) {
                const ChiploadRange r = chiploadRangeFrom(f->feed, f->rpm, t->flutes > 0 ? t->flutes : flutes);
                if (r.valid())
                    return r;
            }
        }
    }
    return chiploadTable(diameter, materialId);
}

ChiploadVerdict classifyChipload(double chipload, const ChiploadRange &r)
{
    if (chipload <= 0 || !r.valid())
        return ChiploadVerdict::Unknown;
    if (chipload < r.low)
        return ChiploadVerdict::TooLow;
    if (chipload > r.high)
        return ChiploadVerdict::TooHigh;
    return ChiploadVerdict::Ok;
}

QString chiploadText(double chipload, const ChiploadRange &r)
{
    const ChiploadVerdict v = classifyChipload(chipload, r);
    if (v == ChiploadVerdict::Unknown)
        return QString();
    const QString what = v == ChiploadVerdict::TooLow ? QStringLiteral("too low (rubbing, heat)")
                       : v == ChiploadVerdict::TooHigh ? QStringLiteral("too high (bit may break)")
                                                       : QStringLiteral("OK");
    return QStringLiteral("Chipload %1 mm/tooth: %2").arg(chipload, 0, 'f', 3).arg(what);
}

double suggestedFeed(const ChiploadRange &r, double rpm, int flutes)
{
    if (!r.valid() || rpm <= 0 || flutes <= 0)
        return 0;
    return std::round(r.mid * rpm * flutes / 10.0) * 10.0;
}

double defaultRpmFor(const QString &materialId)
{
    const QString m = materialId.toLower();
    return m == QLatin1String("aluminium") || m == QLatin1String("aluminum") ? 10000 : 18000;
}

} // namespace c2d
