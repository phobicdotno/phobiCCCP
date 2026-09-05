#pragma once
#include <QRectF>
#include <QVector>
#include <cmath>
#include <functional>

// Shared 3D relief interface (Carbide Create Pro's "3D modeling").
//
// A HeightModel is a regular grid of surface heights over part of the stock.
// Convention, fixed here so the modeller and the 3D toolpaths agree:
//   * work coordinates in mm, Y up, like every element in the document;
//   * z(x, y) is the model's TOP SURFACE relative to the stock top (Z=0):
//     0 = untouched stock, negative = below the top. A relief that stands
//     `h` mm proud of its base therefore has base = -h and peaks up to 0;
//   * cells outside the grid, or marked NoModel, are "no model here": 3D
//     toolpaths treat them as `baseZ` (the model's floor);
//   * cell (c, r) covers [originX + c*cell, +cell) x [originY + r*cell, +cell).
namespace c2d {

class Document;

struct HeightModel {
    static constexpr float NoModel = 1e30f;

    double originX = 0, originY = 0;   // mm, bottom-left corner of cell (0,0)
    double cell = 0.5;                 // mm per cell (square)
    int cols = 0, rows = 0;
    double baseZ = 0;                  // model floor (mm, <= 0)
    QVector<float> z;                  // rows*cols, row-major from the bottom

    bool valid() const { return cols > 0 && rows > 0 && z.size() == cols * rows; }
    QRectF bounds() const { return QRectF(originX, originY, cols * cell, rows * cell); }
    void resize(int c, int r, float fill = NoModel)
    {
        cols = c; rows = r;
        z = QVector<float>(c * r, fill);
    }
    float at(int c, int r) const
    {
        if (c < 0 || r < 0 || c >= cols || r >= rows) return NoModel;
        return z.at(r * cols + c);
    }
    float &ref(int c, int r) { return z[r * cols + c]; }

    // Bilinear sample of the surface at a work coordinate; NoModel cells and
    // points outside the grid return baseZ.
    double sample(double x, double y) const
    {
        if (!valid()) return baseZ;
        const double fx = (x - originX) / cell - 0.5, fy = (y - originY) / cell - 0.5;
        const int c0 = int(std::floor(fx)), r0 = int(std::floor(fy));
        const double tx = fx - c0, ty = fy - r0;
        auto v = [&](int c, int r) {
            const float h = at(c, r);
            return h == NoModel ? baseZ : double(h);
        };
        const double a = v(c0, r0) * (1 - tx) + v(c0 + 1, r0) * tx;
        const double b = v(c0, r0 + 1) * (1 - tx) + v(c0 + 1, r0 + 1) * tx;
        return a * (1 - ty) + b * ty;
    }
};

// The modeller registers how a document's composited relief is obtained;
// the CAM side only ever asks through heightModelFor(). Returns nullptr when
// no provider is registered or the document has no 3D model.
using HeightModelProvider = std::function<const HeightModel *(const Document &)>;
void setHeightModelProvider(HeightModelProvider provider);
const HeightModel *heightModelFor(const Document &doc);

} // namespace c2d
