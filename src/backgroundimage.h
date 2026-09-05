#pragma once
#include <QByteArray>
#include <QImage>
#include <QPixmap>
#include <QRectF>
#include <QString>
#include <QTransform>

class QPainter;

// Carbide Create's tracing background image. CC keeps it inside the .c2d
// container: the encoded picture in `sqlar/background.png` (stored raw, sz ==
// length) and the placement in `params`: background_visible, background_scale,
// background_position_x/_y (mm), background_rotation (deg), background_opacity
// (0-1). This struct mirrors those fields and paints the picture in CC space
// (mm, Y-up, origin at the board's bottom-left).
//
// Scale convention: CC's preview.svg is rendered at 96 dpi, so scale 1.0 is
// taken to mean one image pixel = 1/96 in = 0.264583 mm; the Adjust dialog
// works in "width in mm" and derives the scale from that.
namespace c2d {

struct BackgroundImage
{
    static constexpr double kMmPerPixel = 25.4 / 96.0;   // at scale 1.0

    QImage image;            // decoded picture; null when no background is set
    QByteArray pngData;      // the bytes stored in sqlar (PNG-encoded)
    bool visible = false;
    double x = 0, y = 0;     // bottom-left corner of the unrotated image, mm
    double scale = 1.0;      // CC background_scale
    double rotationDeg = 0;  // about the image centre, CCW positive (Y-up)
    double opacity = 0.5;    // 0-1
    bool locked = false;     // Adjust dialog refuses moves/resizes while set
    // Set when the background is set, cleared or adjusted here. Without it a
    // save would overwrite a background we merely failed to decode, and would
    // add an empty background row to documents that never had one.
    bool userChanged = false;

    bool isNull() const { return image.isNull(); }
    double mmPerPixel() const { return kMmPerPixel * scale; }
    double widthMm() const { return image.width() * mmPerPixel(); }
    double heightMm() const { return image.height() * mmPerPixel(); }
    void setWidthMm(double w);                 // keeps the aspect ratio
    QRectF rectMm() const;                     // unrotated placement, mm (Y-up)

    // Image pixel coordinates (Y-down, origin top-left) -> CC mm (Y-up),
    // including the rotation. Used by the painter and by "trace background".
    QTransform pixelToMm() const;

    // Load a picture file (any format QImage reads); re-encodes to PNG for the
    // container unless it already is one. Keeps placement fields.
    bool setImageFile(const QString &path, QString *error = nullptr);
    void setImage(const QImage &img);
    void clear();

    // Persistence in a .c2d container (params + sqlar rows). loadFrom() on a
    // file without a picture leaves *this cleared and returns true.
    bool loadFrom(const QString &c2dPath, QString *error = nullptr);
    bool saveTo(const QString &c2dPath, QString *error = nullptr) const;

    // Paint into a painter whose coordinate system is CC mm, Y-up (the canvas
    // scene). No-op when null or invisible.
    void paint(QPainter *p) const;

private:
    mutable QPixmap m_cache;   // image converted for fast repeated drawing
    mutable qint64 m_cacheKey = 0;
};

} // namespace c2d
