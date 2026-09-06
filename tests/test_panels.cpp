// Panel <-> canvas selection handover.
//
// The toolpath list and the canvas both have a notion of "the selection", and
// they used to disagree: picking a shape on the canvas left the toolpath row
// selected and its amber halo painted, so the two panes both claimed to be the
// selection and the row looked stuck. Clicking a shape must hand the selection
// over — row deselected, halo cleared — without that being a one-way door.
//
// Plain asserts, no framework; exits 0 on success, 77 (skip) with no sample.

#include "../src/c2ddocument.h"
#include "../src/canvas.h"
#include "../src/toolpathpanel.h"

#include <QApplication>
#include <QDir>
#include <QAbstractItemDelegate>
#include <QFile>
#include <QStyleOptionViewItem>
#include <QTreeWidget>

#include <cstdio>
#include <cstdlib>

static int g_checks = 0;

static void check(bool cond, const char *what)
{
    ++g_checks;
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    const QString sample = argc > 1
        ? QString::fromLocal8Bit(argv[1])
        : QDir::homePath() + QStringLiteral("/Documents/c2d-samples/"
                                            "CarbideCreateAllPossibilities.c2d");
    if (!QFile::exists(sample)) {
        std::fprintf(stderr, "note: sample %s not found, panel checks skipped\n",
                     sample.toUtf8().constData());
        return 77;
    }

    c2d::Document doc;
    QString err;
    check(doc.load(sample, &err), "sample loads");
    check(!doc.toolpaths().isEmpty(), "sample has toolpaths");
    check(!doc.elements().isEmpty(), "sample has elements");

    c2d::Canvas canvas;
    c2d::ToolpathPanel panel(&canvas);
    canvas.setDocument(&doc);
    panel.setDocument(&doc);

    // The panel selects its first row, which lights up that toolpath's shapes.
    check(!panel.currentUuid().isEmpty(), "a toolpath is current after load");
    const QStringList lit = canvas.vectorHighlight();
    check(!lit.isEmpty(), "the selected toolpath's vectors are outlined");

    // Pick a shape the toolpath does not cut: the canvas takes the selection,
    // the row deselects and the halo goes out.
    QString other;
    for (const c2d::Element &e : doc.elements())
        if (!lit.contains(e.id)) {
            other = e.id;
            break;
        }
    check(!other.isEmpty(), "the document has a shape outside that toolpath");
    canvas.selectElements({other});
    check(canvas.vectorHighlight().isEmpty(),
          "clicking a shape clears the toolpath halo");
    check(canvas.selectedElementIds() == QStringList{other},
          "the canvas holds the selection instead");

    // Not a one-way door: re-selecting a row lights its shapes up again. The
    // toolpath also stayed *current*, so the parameter editor and "Assign
    // selected vectors" still act on what the user was editing.
    check(!panel.currentUuid().isEmpty(), "the toolpath stays current");
    panel.refresh();
    check(!canvas.vectorHighlight().isEmpty(),
          "selecting a toolpath again brings its outline back");

    // The type and the vector count are not the user's to type over: they are
    // what the toolpath is and what it references. Double-clicking them used
    // to open an editor whose result was then silently thrown away.
    {
        auto *tree = panel.findChild<QTreeWidget *>();
        check(tree != nullptr, "the panel has its list");
        QStyleOptionViewItem opt;
        for (int col = 1; col <= 2; ++col) {
            QAbstractItemDelegate *d = tree->itemDelegateForColumn(col);
            check(d != nullptr, "a delegate guards the derived column");
            check(d->createEditor(tree, opt, tree->model()->index(0, col)) == nullptr,
                  "the derived column opens no editor");
        }
        QAbstractItemDelegate *d0 = tree->itemDelegateForColumn(0);
        check(d0 == nullptr || d0->createEditor(tree, opt, tree->model()->index(0, 0)) != nullptr,
              "the name column is still editable");
    }

    std::printf("test_panels: %d checks OK\n", g_checks);
    return 0;
}
