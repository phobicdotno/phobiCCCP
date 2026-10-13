#pragma once
#include <QPixmap>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QListWidget;

namespace c2d {

class Document;
struct LibraryTool;
struct Op;

// Tool `number` as the document's toolpaths embed it (CC's tool object):
// diameter, angle, flutes, cut length, kind and name. `users` gets the names
// of the enabled toolpaths that use it.
LibraryTool toolFromToolpaths(const Document &doc, int number, QStringList *users = nullptr);
// A side-view diagram with dimension lines, `w` x `h` logical px, for dialogs.
QPixmap toolPicture(const LibraryTool &t, int w, int h, double devicePixelRatio);

// The tools a program uses, in the order the spindle picks them up, one icon
// each (the library's kind outline) with number and diameter. The one in the
// spindle at the preview's playback position is highlighted; clicking one
// jumps the preview to its tool change.
class ToolStrip : public QWidget
{
    Q_OBJECT
public:
    explicit ToolStrip(QWidget *parent = nullptr);
    void setJob(const Document &doc, const QVector<Op> &ops);
    void setCurrent(int number);
    QSize sizeHint() const override { return {118, 400}; }

signals:
    void toolClicked(int number);

private:
    QListWidget *m_list;
};

} // namespace c2d
