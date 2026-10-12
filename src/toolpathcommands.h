#pragma once
#include "c2ddocument.h"
#include "canvas.h"
#include <QUndoCommand>

// Document-level undo commands for the toolpath lifecycle (create, delete,
// duplicate, reorder). Parameter edits, renames and the enabled flag go
// through Canvas::editToolpath (TpEditCmd). Each command mutates the
// Document and raises Canvas::documentChanged so the panels, the preview
// overlay and the dirty flag follow; the scene itself holds no toolpath
// items, so no rebuild is needed and the vector selection survives.
namespace c2d {

class TpInsertCmd : public QUndoCommand
{
public:
    TpInsertCmd(Canvas *c, Document *d, const Toolpath &t, int index, const QString &what)
        : m_c(c), m_d(d), m_t(t), m_index(index)
    { setText(QStringLiteral("%1 toolpath %2").arg(what, t.json.value("name").toString())); }
    void redo() override { m_d->insertToolpath(m_index, m_t); emit m_c->documentChanged(); }
    void undo() override { m_d->removeToolpath(m_t.uuid); emit m_c->documentChanged(); }
private:
    Canvas *m_c; Document *m_d; Toolpath m_t; int m_index;
};

class TpRemoveCmd : public QUndoCommand
{
public:
    TpRemoveCmd(Canvas *c, Document *d, const Toolpath &t)
        : m_c(c), m_d(d), m_t(t), m_index(d->toolpathIndex(t.uuid))
    { setText(QStringLiteral("delete toolpath %1").arg(t.json.value("name").toString())); }
    void redo() override { m_d->removeToolpath(m_t.uuid); emit m_c->documentChanged(); }
    void undo() override { m_d->insertToolpath(m_index, m_t); emit m_c->documentChanged(); }
private:
    Canvas *m_c; Document *m_d; Toolpath m_t; int m_index;
};

class TpMoveCmd : public QUndoCommand
{
public:
    TpMoveCmd(Canvas *c, Document *d, const QString &uuid, int from, int to)
        : m_c(c), m_d(d), m_uuid(uuid), m_from(from), m_to(to)
    { setText(QStringLiteral("move toolpath %1").arg(to < from ? "up" : "down")); }
    void redo() override { m_d->moveToolpath(m_uuid, m_to); emit m_c->documentChanged(); }
    void undo() override { m_d->moveToolpath(m_uuid, m_from); emit m_c->documentChanged(); }
private:
    Canvas *m_c; Document *m_d; QString m_uuid; int m_from, m_to;
};

} // namespace c2d
