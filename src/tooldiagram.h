#pragma once
#include <QIcon>
#include <QJsonObject>
#include <QWidget>

class QPainter;

namespace c2d {

struct LibraryTool;

// A side-view line drawing of a cutter: the same outline for every tool of a
// kind (flat end mill, ball, V-bit, engraver, corner-rounding), not to
// scale, with the tool's own numbers on dimension lines: cutting Ø, shank Ø,
// cut length, and the angle or radius where the kind has one.
void paintToolDiagram(QPainter &p, const QRectF &box, const LibraryTool &t, bool dimensions);
QIcon toolKindIcon(const LibraryTool &t);   // the outline alone, for list rows
// One glyph per toolpath kind (Toolpaths-mode bar, toolpath cards, the
// running job): what the cut looks like from above, amber where material goes.
QIcon toolpathIcon(const QString &type);
// The bit a toolpath's CC `tool` object names, as far as its outline needs:
// kind, diameter, angle.
LibraryTool toolpathBit(const QJsonObject &tool);

class ToolDiagram : public QWidget
{
public:
    explicit ToolDiagram(QWidget *parent = nullptr);
    void setTool(const LibraryTool *t);
    QSize sizeHint() const override { return {230, 300}; }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    const LibraryTool *m_tool = nullptr;
};

} // namespace c2d
