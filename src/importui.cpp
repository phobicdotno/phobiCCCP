#include "importui.h"
#include "canvas.h"
#include "c2ddocument.h"
#include "importers.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QMainWindow>
#include <QMessageBox>
#include <QMimeData>
#include <QStatusBar>
#include <QUndoCommand>
#include <QUndoStack>
#include <QUrl>

namespace c2d {

namespace {

class ImportCmd : public QUndoCommand
{
public:
    ImportCmd(Canvas *c, Document *d, const QVector<Element> &els, const QString &name)
        : m_c(c), m_d(d), m_els(els)
    {
        setText(QStringLiteral("import %1").arg(name));
    }
    void redo() override
    {
        for (const Element &e : m_els)
            m_d->addElement(e);
        refresh();
    }
    void undo() override
    {
        for (const Element &e : m_els)
            m_d->removeElementById(e.id);
        refresh();
    }

private:
    void refresh()
    {
        m_c->rebuild();
        emit m_c->documentChanged();
    }
    Canvas *m_c;
    Document *m_d;
    QVector<Element> m_els;
};

QString localFile(const QMimeData *mime)
{
    if (!mime || !mime->hasUrls())
        return QString();
    for (const QUrl &u : mime->urls()) {
        if (!u.isLocalFile())
            continue;
        const QString p = u.toLocalFile();
        const QString s = QFileInfo(p).suffix().toLower();
        if (isImportableFile(p) || s == QLatin1String("c2d"))
            return p;
    }
    return QString();
}

class DropFilter : public QObject
{
public:
    DropFilter(QWidget *parent, Canvas *canvas, Document *doc,
               std::function<void(const QString &)> openC2d)
        : QObject(parent), m_parent(parent), m_canvas(canvas), m_doc(doc),
          m_open(std::move(openC2d)) {}

    bool eventFilter(QObject *obj, QEvent *ev) override
    {
        if (ev->type() == QEvent::DragEnter || ev->type() == QEvent::DragMove) {
            auto *de = static_cast<QDragMoveEvent *>(ev);
            if (!localFile(de->mimeData()).isEmpty()) {
                de->acceptProposedAction();
                return true;
            }
        } else if (ev->type() == QEvent::Drop) {
            auto *de = static_cast<QDropEvent *>(ev);
            const QString p = localFile(de->mimeData());
            if (p.isEmpty())
                return false;
            de->acceptProposedAction();
            if (QFileInfo(p).suffix().compare(QLatin1String("c2d"), Qt::CaseInsensitive) == 0) {
                if (m_open) m_open(p);
            } else {
                importVectorFile(m_parent, m_canvas, m_doc, p);
            }
            return true;
        }
        return QObject::eventFilter(obj, ev);
    }

private:
    QWidget *m_parent;
    Canvas *m_canvas;
    Document *m_doc;
    std::function<void(const QString &)> m_open;
};

} // namespace

void importVectorFile(QWidget *parent, Canvas *canvas, Document *doc,
                      const QString &pathIn, const QString &filterIn)
{
    if (!doc || doc->filePath().isEmpty()) {
        QMessageBox::information(parent, QStringLiteral("Import"),
                                 QStringLiteral("Open a .c2d file first."));
        return;
    }
    QString path = pathIn;
    if (path.isEmpty()) {
        const QString filter = filterIn.isEmpty()
            ? QStringLiteral("Vector files (*.svg *.dxf);;SVG (*.svg);;DXF (*.dxf);;All files (*)")
            : filterIn + QStringLiteral(";;All files (*)");
        path = QFileDialog::getOpenFileName(parent, QStringLiteral("Import vector file"), {},
                                            filter);
        if (path.isEmpty())
            return;
    }

    ImportOptions opt;
    opt.stockWidth = doc->boardWidth();
    opt.stockHeight = doc->boardHeight();
    opt.layer = doc->defaultLayer();
    const ImportResult r = importFile(path, opt);
    const QString name = QFileInfo(path).fileName();

    if (!r.ok) {
        QMessageBox::warning(parent, QStringLiteral("Import failed"), r.error);
        return;
    }
    if (r.elements.isEmpty()) {
        QMessageBox::information(parent, QStringLiteral("Import"),
                                 QStringLiteral("%1 contains no importable geometry.\n%2")
                                     .arg(name, r.notes.join(QLatin1Char('\n'))));
        return;
    }
    if (canvas)
        canvas->undoStack()->push(new ImportCmd(canvas, doc, r.elements, name));
    else
        for (const Element &e : r.elements)
            doc->addElement(e);

    if (auto *mw = qobject_cast<QMainWindow *>(parent))
        mw->statusBar()->showMessage(name + QStringLiteral(": ") + r.summary(), 8000);
    if (r.skipped > 0 || !r.notes.isEmpty())
        QMessageBox::information(parent, QStringLiteral("Import"),
                                 r.summary() + QStringLiteral("\n\n")
                                     + r.notes.join(QLatin1Char('\n')));
}

void installImportDropHandler(QWidget *target, Canvas *canvas, Document *doc,
                              std::function<void(const QString &)> openC2d)
{
    auto *f = new DropFilter(target, canvas, doc, std::move(openC2d));
    target->setAcceptDrops(true);
    target->installEventFilter(f);
    if (canvas) {
        canvas->installEventFilter(f);
        if (canvas->viewport())
            canvas->viewport()->installEventFilter(f);
    }
}

} // namespace c2d
