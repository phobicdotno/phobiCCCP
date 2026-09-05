#pragma once
#include "element.h"
#include "heightmodel.h"

#include <QByteArray>
#include <QImage>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>

// Carbide Create Pro's "3D Modeling": a stack of relief *components* that is
// composited into one HeightModel (src/heightmodel.h) over the stock.
//
// Components come from closed vectors (a shape profile extruded from the
// vector's outline: flat, round, angled, smooth, dome — the profile is a
// function of the distance to the vector's boundary, computed with an exact
// Euclidean distance transform so it is right for any outline), from a
// grayscale image used as a heightmap, from an STL mesh rasterised top-down,
// or from an image tiled as a texture over a region. Each is combined in
// list order with Add / Subtract / Merge (max) / Multiply (fade).
//
// The composited grid follows the heightmodel.h convention: z <= 0 relative
// to the stock top, the model's highest point at 0, baseZ = -(total relief
// height), cells no component touches = NoModel.
namespace c2d {

class Document;

struct ModelComponent {
    enum Kind { FromVectors = 0, ImageHeightmap = 1, StlMesh = 2, Texture = 3 };
    enum Shape { Flat = 0, Round = 1, Angle = 2, Smooth = 3, Dome = 4 };
    enum Combine { Add = 0, Subtract = 1, Merge = 2, Multiply = 3 };
    enum Units { Millimetres = 0, Inches = 1 };

    QString id;                  // "{uuid}"
    QString name;
    Kind kind = FromVectors;
    bool enabled = true;
    Combine combine = Add;
    // Height, mm. FromVectors: the profile's peak above the base. Image /
    // Texture: the amplitude a white pixel maps to. STL: the mesh's total Z
    // extent after placement (0 = keep the mesh's own, unit-scaled, height).
    double height = 5.0;
    double baseHeight = 0.0;     // mm added under the whole region ("base height")

    // FromVectors: region = union of these elements' filled outlines.
    // Texture: target region (empty = everything modelled so far).
    QStringList vectorIds;
    Shape shape = Round;
    double angleDeg = 45.0;      // Angle profile: slope from the horizontal

    // Image / STL / Texture source. `data` is the file's bytes, embedded so
    // the .c2d stays self-contained; `sourceName` is only a label.
    QByteArray data;
    QString sourceName;
    bool invert = false;         // image: white = low instead of high
    double blurPx = 0.0;         // image: Gaussian sigma in source pixels
    double x = 0.0, y = 0.0;     // bottom-left of the placed image / mesh, mm
    double width = 0.0;          // placed width, mm (0 = natural: image px at 96 dpi, STL units)
    double tileWidth = 10.0;     // Texture: one tile's width, mm (height keeps the aspect)
    Units units = Millimetres;   // STL: how to read the file's coordinates

    static QString kindName(Kind k);
    static QString shapeName(Shape s);
    static QString combineName(Combine c);

    QJsonObject toJson() const;                       // without `data`
    static ModelComponent fromJson(const QJsonObject &o);
};

struct Model3D {
    QVector<ModelComponent> components;
    double resolution = 0.0;     // mm per cell; 0 = automatic (<= 4 M cells, >= 0.1 mm)

    bool hasEnabled() const;
    int indexOf(const QString &id) const;
    static QString newId();

    QJsonObject toJson() const;
    static Model3D fromJson(const QJsonObject &o);

    // Persistence in a .c2d container. Carbide Create's own `items` MODEL row
    // is a binary payload whose sample format is not documented, so it is
    // left untouched; ours lives in `sqlar` (the container's auxiliary-file
    // table CC ignores unknown names in): `phobi_model3d.json` holds the
    // component list, `phobi_model3d/<component id>` the raw image / STL
    // bytes of each component that has any. loadFrom() on a file without a
    // model leaves *this empty and returns true.
    bool loadFrom(const QString &c2dPath, QString *error = nullptr);
    bool saveTo(const QString &c2dPath, QString *error = nullptr) const;
};

// ---- compositing -----------------------------------------------------------

// Cell size actually used for a board: `requested` (0 = auto), never finer
// than 0.1 mm, coarsened until the grid holds <= 4 M cells.
double modelCellSize(double boardW, double boardH, double requested);

using ModelProgressFn = std::function<void(int percent)>;

// Composite the enabled components over the stock (0..boardW x 0..boardH).
// `elements` resolves the components' vectorIds. Returns an invalid model
// (cols == 0) when nothing is enabled or the board is empty, and a partial
// result if `cancel` is raised.
HeightModel buildHeightModel(const Model3D &model, const QVector<Element> &elements,
                             double boardW, double boardH,
                             ModelProgressFn progress = {},
                             std::atomic<bool> *cancel = nullptr);
HeightModel buildHeightModel(const Model3D &model, const Document &doc,
                             ModelProgressFn progress = {},
                             std::atomic<bool> *cancel = nullptr);

// Building blocks, exposed for the tests.

// Exact Euclidean distance transform (Felzenszwalb & Huttenlocher, two 1D
// passes): for each cell with inside[i] != 0 the distance, in cells, to the
// nearest cell with inside == 0 (cells outside the grid count as outside).
// Outside cells get 0.
QVector<float> distanceTransform(const QVector<unsigned char> &inside, int cols, int rows);

// Grayscale 0..1 field of an image (luma; `invert` flips it; Gaussian blur of
// `sigmaPx`). Row 0 is the image's BOTTOM row so it reads like a Y-up grid.
struct GrayField {
    int cols = 0, rows = 0;
    QVector<float> v;
    float at(int c, int r) const { return v.at(r * cols + c); }
    bool valid() const { return cols > 0 && rows > 0 && v.size() == cols * rows; }
};
GrayField grayField(const QImage &img, bool invert, double sigmaPx);

// STL parsing (binary or ASCII, auto-detected). Vertices in file units.
struct StlTriangle { float v[3][3]; };
bool parseStl(const QByteArray &bytes, QVector<StlTriangle> *tris, QString *error = nullptr);

// Shaded top-down picture of a model: light from the upper left, colour by
// height (dark floor to light peak), NoModel cells in the view background.
// Row 0 of the image is the top of the board (max Y).
QImage renderHeightModel(const HeightModel &hm);

// ---- provider ----------------------------------------------------------------

// Per-document owner of a Model3D plus its lazily-composited grid. The Model
// tab attaches its store to the open document; documents nobody attached to
// (headless CLI) get a store that loads the model from the file on demand.
class ModelStore
{
public:
    ModelStore() = default;
    ~ModelStore();
    ModelStore(const ModelStore &) = delete;
    ModelStore &operator=(const ModelStore &) = delete;

    static ModelStore *forDocument(const Document &doc);

    void attach(const Document *doc);   // this store now answers for `doc`
    void detach();
    const Document *document() const { return m_doc; }

    Model3D model;
    void invalidate() { m_dirty = true; }
    bool dirty() const { return m_dirty; }

    // The composited grid: rebuilt synchronously when dirty. nullptr when the
    // model has nothing enabled or no document is attached.
    const HeightModel *heightModel();
    void setHeightModel(const HeightModel &hm);   // result of an asynchronous rebuild
    const HeightModel &cached() const { return m_hm; }

private:
    const Document *m_doc = nullptr;
    QString m_loadedFrom;      // file a lazily-created store read its model from
    HeightModel m_hm;
    bool m_dirty = true;
    bool m_lazy = false;
    friend struct ModelRegistry;
};

// Register the heightmodel.h provider: heightModelFor(doc) returns the
// document's composited model through ModelStore::forDocument(). Idempotent.
void installModelProvider();

} // namespace c2d
