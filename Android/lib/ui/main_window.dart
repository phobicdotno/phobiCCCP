// The pad app's one window, laid out as phobiCCCP's main window
// (mainwindow.cpp): menu bar, logo and file name in the middle and the
// 1 · Design / 2 · Toolpaths / 3 · Machine switch in the header; the tool row;
// the drawing with its side panel and zoom bar; the status line at the bottom.

import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../app_state.dart';
import '../fill.dart';
import '../model.dart';
import 'canvas.dart';
import 'icons.dart';
import 'machine_page.dart';
import 'panels.dart';
import 'send_dialog.dart';
import 'theme.dart';
import 'widgets.dart';

/// Kept with pubspec.yaml's version and the desktop's VERSION.
const appVersion = '1.3.30';

class MainWindow extends StatefulWidget {
  final AppState app;
  const MainWindow(this.app, {super.key});
  @override
  State<MainWindow> createState() => _MainWindowState();
}

class _MainWindowState extends State<MainWindow> {
  AppState get app => widget.app;

  Design? _d;
  final _undo = <Design>[], _redo = <Design>[];
  Design? _beforeMove;
  int _mode = 0;
  DrawTool _tool = DrawTool.select;
  bool _snap = true, _fullScreen = false;
  Set<String> _sel = {};
  int? _tp;

  /// Toolpath cards whose shape thumbnails are open, by index (kept with the
  /// card when it moves, as the desktop keeps them by toolpath).
  Set<int> _shapesOpen = {};
  void _remapOpen(int Function(int k) f) => _shapesOpen = {for (final k in _shapesOpen) if (f(k) >= 0) f(k)};
  List<List<double>>? _pathDraft;
  final _zoom = CanvasZoom();
  String _status = '';
  bool _saving = false;
  int _notices = 0;
  int _opens = 0;

  /// Said once with the next status after the first edit of a design whose
  /// file a computer will not rebuild from the pad (Design.desktopOnly).
  String? _copyNote;
  bool _copyNoted = false;

  @override
  void initState() {
    super.initState();
    final last = app.design(app.lastDesign);
    _d = last != null && !last.deleted ? last : app.designs.firstOrNull;
    final d = _d;
    _status = d == null
        ? 'File › New or Open to begin'
        : d.desktopOnly.isNotEmpty
            ? _copyWords(d)
            : 'Select — tap to select, drag to move or to select an area';
    _notices = app.noticeCount;
    _opens = app.openCount;
    app.addListener(_synced);
  }

  @override
  void dispose() {
    app.removeListener(_synced);
    super.dispose();
  }

  // A newer copy came in from a phobiCCCP (edited and saved there, or on
  // another pad), or the computer's copy replaced ours after both changed:
  // show it in place of ours.
  void _synced() {
    final cur = _d;
    if (!mounted || _saving) return;
    if (app.noticeCount != _notices) {
      _notices = app.noticeCount;
      _status = app.notice;
    }
    // Sent from a computer to edit here: it opens in place of this one,
    // which is already saved (every edit here is).
    final sent = app.toOpen;
    if (app.openCount != _opens && sent != null) {
      _opens = app.openCount;
      final d = app.design(sent.id) ?? sent;
      setState(() {
        _open(d);
        _mode = 0;
        _status = d.desktopOnly.isNotEmpty ? '“${d.name}” from ${app.openFrom} · ${_copyWords(d)}' : '“${d.name}” from ${app.openFrom}';
      });
      return;
    }
    if (cur == null) {
      setState(() {});
      return;
    }
    final d = app.design(cur.id);
    if (d == null || d.isSameCopy(cur)) {
      setState(() {});
      return;
    }
    setState(() {
      if (d.deleted) {
        _open(null);
        _status = '“${cur.name}” was deleted on another pad or computer';
      } else {
        _d = d;
        _beforeMove = null;
        _sel = _sel.where((id) => d.shape(id) != null).toSet();
        // Undoing past it would put the old copy back everywhere.
        _undo.clear();
        _redo.clear();
        if (_notices == app.noticeCount && !_status.contains(d.name)) _status = '“${d.name}” was updated from phobiCCCP';
      }
    });
  }

  void _open(Design? d) {
    _d = d;
    _beforeMove = null;
    _undo.clear();
    _redo.clear();
    _sel = {};
    _tp = null;
    _pathDraft = null;
    _zoom.reset();
    _copyNote = null;
    _copyNoted = false;
    if (d != null) app.lastDesign = d.id;
  }

  /// What an edit here means for a design one computer keeps more of.
  String _copyWords(Design d) =>
      '${d.desktopOnly} keeps more of “${d.name}” than the pad can hold (curves, tabs, parameters…), '
      'so edits here reach it as a copy beside its file';

  /// Save [d] as our edit, without taking our own save for one from elsewhere.
  Design _save(Design d) {
    _saving = true;
    try {
      return app.saveDesign(d);
    } finally {
      _saving = false;
    }
  }

  /// A newer copy came in after this edit was begun: it is shown instead, as
  /// saving the edit would silently undo it everywhere.
  bool _stale() {
    final cur = _d;
    final stored = cur == null ? null : app.design(cur.id);
    if (cur == null || stored == null || stored.isSameCopy(cur)) return false;
    _synced();
    _say('“${stored.name}” changed on another pad or computer; your last change was not kept');
    return true;
  }

  /// [d] is no longer the design on screen (a newer copy came in, or it was
  /// closed or deleted) while a dialog about it was open: the dialog's edit is
  /// dropped, as saving it would put the old copy back everywhere.
  bool _replacedSince(Design d) {
    final cur = _d;
    if (cur != null && cur.id == d.id && cur.isSameCopy(d) && !_stale()) return false;
    _say('“${d.name}” changed while the dialog was open; that change was not kept');
    return true;
  }

  /// [d] (what a panel was last built from) is no longer the design on
  /// screen: a sync replaced it before the next frame. An edit made from it
  /// would put the old copy back everywhere, over the other device's change.
  bool _changedUnder(Design d) {
    final cur = _d;
    if (cur != null && cur.id == d.id && cur.isSameCopy(d)) return false;
    _say('The design just changed on another pad or computer; that tap was not kept');
    setState(() {});
    return true;
  }

  void _commit(Design next, {Design? before}) {
    if (_stale()) return;
    final prev = before ?? _d;
    if (prev != null) _undo.add(prev);
    if (_undo.length > 100) _undo.removeAt(0);
    _redo.clear();
    final saved = _save(next);
    setState(() => _d = saved);
    if (saved.desktopOnly.isNotEmpty && !_copyNoted) {
      // Shown now, and added to what the edit itself says right after.
      _copyNoted = true;
      final note = _copyNote = _copyWords(saved);
      setState(() => _status = note);
      WidgetsBinding.instance.addPostFrameCallback((_) => _copyNote = null);
    }
  }

  void _undoRedo(bool undo) {
    final from = undo ? _undo : _redo, to = undo ? _redo : _undo;
    final cur = _d;
    if (from.isEmpty || cur == null || _stale()) return;
    to.add(cur);
    final d = from.removeLast();
    final saved = _save(d);
    setState(() {
      _d = saved;
      _sel = _sel.where((id) => d.shape(id) != null).toSet();
    });
  }

  void _say(String s) {
    final note = _copyNote;
    _copyNote = null;
    setState(() => _status = note == null ? s : (s.isEmpty ? note : '$s · $note'));
  }

  void _setTool(DrawTool t) {
    setState(() {
      _tool = t;
      _pathDraft = t == DrawTool.path ? [] : null;
      _status = switch (t) {
        DrawTool.select => 'Select — tap to select, drag to move or to select an area',
        DrawTool.hand => 'Pan — drag to move the view',
        DrawTool.area => 'Area — tap inside an area, like a fill: selects just that area (not the shapes inside it)',
        DrawTool.circle => 'Circle — drag from the center out',
        DrawTool.rect => 'Rectangle — drag corner to corner',
        DrawTool.polygon => 'Polygon — drag from the center out',
        DrawTool.text => 'Text — tap where the text starts',
        DrawTool.path => 'Path — tap the points; tap the first again to close, the last again to finish',
        DrawTool.point => 'Point — tap to place a drill point',
      };
    });
  }

  void _addShape(Shape s) {
    final d = _d;
    if (d == null) return;
    _commit(d.copyWith(shapes: [...d.shapes, s]));
    setState(() {
      _sel = {s.id};
      _tool = DrawTool.select;
      _pathDraft = null;
    });
    _say('Added ${s.describe().toLowerCase()}');
  }

  /// Area (fill tool), as on the desktop: the area around the tap, bounded
  /// by every closed shape. Rings that are existing shapes select those;
  /// any other ring (where shapes overlap) is added as a closed path.
  void _fillAt(double x, double y) {
    final d = _d;
    if (d == null) return;
    final closed = [
      for (final s in d.shapes)
        if (shapeRing(s) case final r?) (s.id, r)
    ];
    final face = fillFace([for (final c in closed) c.$2], (x, y));
    if (face.isEmpty) {
      setState(() => _sel = {});
      return;
    }
    final ids = <String>{};
    final fresh = <List<Pt>>[];
    for (final ring in face) {
      final match = closed.where((c) => sameRing(c.$2, ring)).firstOrNull;
      if (match != null) {
        ids.add(match.$1);
      } else {
        fresh.add(ring);
      }
    }
    if (fresh.isNotEmpty) {
      var next = d;
      for (final ring in fresh) {
        final s = Shape(id: next.nextShapeId(), type: Shape.path, points: [for (final p in ring) [p.$1, p.$2]], closed: true);
        next = next.copyWith(shapes: [...next.shapes, s]);
        ids.add(s.id);
      }
      _commit(next);
      _say('Area — the overlap was added as a closed path, so a toolpath cuts only this area');
    }
    // Lettering standing in the area: on the computer its glyphs are islands
    // of the face, so a pocket leaves them standing. The pad has no glyph
    // outlines, only a guess at the text's box, so it selects text only
    // where it is sure to match: the box (widened for wide capitals and the
    // tails of g, p, y) wholly in the face, and no letter with a closed
    // inside (A, O, e, 8...), whose inside the computer keeps uncut and a
    // pocket around the selected text would clear. The rest gets a note.
    const closedLetters = 'ABDOPQRabdegopq04689#%&@';
    var unsure = 0;
    for (final s in d.shapes) {
      if (s.type != Shape.text) continue;
      final size = s.size ?? 10;
      final b = [s.x, s.y - 0.3 * size, s.x + size * math.max(1, s.label?.length ?? 1), s.y + size];
      final corners = <Pt>[(b[0], b[1]), (b[2], b[1]), (b[2], b[3]), (b[0], b[3])];
      bool inBox(Pt p) => p.$1 > b[0] && p.$1 < b[2] && p.$2 > b[1] && p.$2 < b[3];
      if (!corners.any((c) => ringContains(face.first, c)) && !face.first.any(inBox)) continue;
      if (face.skip(1).any((h) => corners.every((c) => ringContains(h, c)))) continue;   // on an island
      final whole = corners.every((c) => ringContains(face.first, c)) &&
          !face.first.any(inBox) &&
          face.skip(1).every((h) => !corners.any((c) => ringContains(h, c)) && !h.any(inBox));
      // Anything past plain ASCII (Ø, é, ©...) may have a closed inside too.
      final label = s.label ?? '';
      final closed = label.split('').any(closedLetters.contains) || !RegExp(r'^[ -~]*$').hasMatch(label);
      if (whole && !closed) {
        ids.add(s.id);
      } else {
        unsure++;
      }
    }
    if (unsure > 0) {
      _say('Area — $unsure text item(s) not selected: near the edge, or with closed letters '
          '(A, O, e…) the pad cannot keep uncut. Select it by hand to keep the letters standing (their insides get cut)');
    }
    setState(() => _sel = ids);
  }

  Future<void> _tapAt(double x, double y) async {
    final d = _d;
    if (d == null) return;
    switch (_tool) {
      case DrawTool.area:
        _fillAt(x, y);
      case DrawTool.point:
        _addShape(Shape(id: d.nextShapeId(), type: Shape.point, x: x, y: y));
      case DrawTool.text:
        final t = await _askText();
        // The design may have been replaced while the box was open (another
        // one opened from the computer): the text was for this one only.
        final now = _d;
        if (t != null && now != null && now.id == d.id && mounted) {
          _addShape(Shape(id: now.nextShapeId(), type: Shape.text, x: x, y: y, label: t.$1, size: t.$2));
        }
      case DrawTool.path:
        final pts = _pathDraft ?? [];
        final slop = 8 / (_zoom.fit * _zoom.zoom);
        bool near(List<double> p) => (p[0] - x).abs() < slop && (p[1] - y).abs() < slop;
        // Snapping can bring two taps onto one point: no segment of no length.
        final closedPts = dropRepeats(pts, closed: true), openPts = dropRepeats(pts);
        if (closedPts.length >= 3 && near(pts.first)) {
          _addShape(Shape(id: d.nextShapeId(), type: Shape.path, points: closedPts, closed: true));
        } else if (openPts.length >= 2 && near(pts.last)) {
          _addShape(Shape(id: d.nextShapeId(), type: Shape.path, points: openPts, closed: false));
        } else if (pts.isNotEmpty && pts.last[0] == x && pts.last[1] == y) {
          _say('Path: that point is already there — tap somewhere else, or the first point to close');
        } else {
          setState(() => _pathDraft = [...pts, [x, y]]);
          _say('Path: ${pts.length + 1} points — tap the first to close, the last again to finish');
        }
      default:
        break;
    }
  }

  void _deleteSelected() {
    final d = _d;
    if (d == null || _sel.isEmpty) return;
    final gone = _sel;
    _commit(d.copyWith(
      shapes: d.shapes.where((s) => !gone.contains(s.id)).toList(),
      toolpaths: [for (final t in d.toolpaths) t.copyWith(shapes: t.shapes.where((x) => !gone.contains(x)).toList())],
    ));
    setState(() => _sel = {});
    _say('Deleted ${gone.length} vector${gone.length == 1 ? '' : 's'}');
  }

  // ---- toolpaths -------------------------------------------------------------

  void _newToolpath(String type) {
    final d = _d;
    if (d == null) return;
    final shapes = _sel.where((id) => d.shape(id) != null).toList();
    if (shapes.isEmpty) {
      _say('Select the vectors to cut first, then pick ${tpTypeLabel(type)}');
      return;
    }
    final tool = app.tools.defaultTool(d.material, type) ?? 201;
    final n = d.toolpaths.where((t) => t.type == type).length + 1;
    // As the computer starts a new toolpath (toolpathfactory.cpp): 2.54 mm
    // deep, an engraving 0.5 mm, named after the type the way it names them.
    final depth = type == 'engrave' ? 0.5 : 2.54;
    final stem = const {
      'profile': 'Contour Toolpath',
      'pocket': 'Pocket Toolpath',
      'drill': 'Drilling Toolpath',
      'engrave': 'Engrave',
    }[type] ?? tpTypeLabel(type);
    final tp = ToolpathSpec(name: '$stem $n', type: type, shapes: shapes, tool: tool, depth: depth);
    _commit(d.copyWith(toolpaths: [...d.toolpaths, tp]));
    setState(() => _tp = _d!.toolpaths.length - 1);
    final open = openPathsLeftOut(d, tp);
    _say('${tp.name}: ${shapes.length} vector${shapes.length == 1 ? '' : 's'}'
        '${open > 0 ? ' (${openPathsWords(open)})' : ''}');
  }

  void _setToolpath(int i, ToolpathSpec t) {
    final d = _d!;
    _commit(d.copyWith(toolpaths: [for (var k = 0; k < d.toolpaths.length; k++) k == i ? t : d.toolpaths[k]]));
  }

  Future<void> _pickTool(int i) async {
    final d = _d!;
    final t = d.toolpaths[i];
    final have = app.tools.toolsFor(d.material);
    if (have.isEmpty) {
      _say('The tool library comes from phobiCCCP once the pad has found it');
      return;
    }
    final n = await showPDialog<int>(context,
        title: 'Tool for ${t.name}',
        width: 520,
        content: SizedBox(
          height: 420,
          child: ListView(children: [
            for (final tool in have)
              InkWell(
                onTap: () => Navigator.pop(context, tool.number),
                child: Container(
                  color: tool.number == t.tool ? C.selCard : null,
                  padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 6),
                  child: Row(children: [
                    ToolGlyph(tool.kind, height: 36),
                    const SizedBox(width: 12),
                    Expanded(
                      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                        Text('T${tool.number} Ø ${fmt(tool.diameter)} mm${tool.flutes > 0 ? ' · ${tool.flutes} fl' : ''}',
                            style: const TextStyle(fontWeight: FontWeight.bold)),
                        Text(tool.name, style: const TextStyle(color: C.label)),
                      ]),
                    ),
                  ]),
                ),
              ),
          ]),
        ),
        actions: (ctx) => [TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Cancel'))]);
    if (n == null || _replacedSince(d)) return;
    _setToolpath(i, t.copyWith(tool: n));
  }

  // ---- dialogs -----------------------------------------------------------------

  Future<(String, double)?> _askText() {
    var text = '';
    var h = 20.0;
    return showPDialog<(String, double)>(context,
        title: 'Text',
        content: StatefulBuilder(
          builder: (ctx, set) => Column(mainAxisSize: MainAxisSize.min, children: [
            FormRow('Text', LineBox(onChanged: (v) => text = v, autofocus: true, maxLength: Shape.maxText), labelWidth: 96),
            FormRow('Height', NumBox(h, (v) => set(() => h = v), min: 0.5, max: 1000, decimals: 2), labelWidth: 96),
          ]),
        ),
        actions: (ctx) => [
              TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Cancel')),
              TextButton(onPressed: () => Navigator.pop(ctx, text.trim().isEmpty ? null : (text, h)), child: const Text('OK')),
            ]);
  }

  Future<void> _newDesign() async {
    var name = 'Design ${app.designs.length + 1}';
    var stock = const Stock();
    var material = 'softwood';
    final ok = await showPDialog<bool>(context,
        title: 'New design',
        content: StatefulBuilder(
          builder: (ctx, set) => Column(mainAxisSize: MainAxisSize.min, children: [
            FormRow('Name', LineBox(initial: name, onChanged: (v) => name = v, autofocus: true), labelWidth: 96),
            StockFields(
                stock: stock,
                material: material,
                tools: app.tools,
                onStock: (s) => set(() => stock = s),
                onMaterial: (m) => set(() => material = m)),
          ]),
        ),
        actions: (ctx) => [
              TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Cancel')),
              TextButton(onPressed: () => Navigator.pop(ctx, true), child: const Text('OK')),
            ]);
    if (ok != true) return;
    final d = app.newDesign(name, stock, material);
    setState(() => _open(d));
    _say('New design “${d.name}”: ${fmt(stock.width)} × ${fmt(stock.height)} × ${fmt(stock.thickness)} mm');
  }

  Future<void> _openDialog() async {
    // One copy for the count and the cards: a sync replacing the list while
    // the dialog is up (designs deleted elsewhere) indexed past its end.
    final list = List.of(app.designs);
    final picked = await showPDialog<Design>(context,
        title: 'Open',
        width: 760,
        content: SizedBox(
          height: 480,
          child: list.isEmpty
              ? const Center(child: Text('No designs yet. File › New makes one.', style: TextStyle(color: C.sub)))
              : GridView.builder(
                  gridDelegate: const SliverGridDelegateWithMaxCrossAxisExtent(
                      maxCrossAxisExtent: 240, mainAxisSpacing: 10, crossAxisSpacing: 10, childAspectRatio: 1.15),
                  itemCount: list.length,
                  itemBuilder: (ctx, i) {
                    final x = list[i];
                    final cur = x.id == _d?.id;
                    return InkWell(
                      onTap: () => Navigator.pop(ctx, x),
                      child: Container(
                        decoration: BoxDecoration(
                            color: C.card,
                            borderRadius: BorderRadius.circular(8),
                            border: Border.all(color: cur ? C.amber : C.cardBorder, width: cur ? 2 : 1)),
                        clipBehavior: Clip.antiAlias,
                        child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
                          Expanded(child: DesignThumbnail(x)),
                          Padding(
                            padding: const EdgeInsets.fromLTRB(8, 6, 8, 6),
                            child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                              Text(x.name.isEmpty ? 'Untitled' : x.name,
                                  overflow: TextOverflow.ellipsis, style: const TextStyle(fontWeight: FontWeight.bold)),
                              Text('${fmt(x.stock.width)} × ${fmt(x.stock.height)} × ${fmt(x.stock.thickness)} mm · ${x.toolpaths.length} toolpaths',
                                  overflow: TextOverflow.ellipsis, style: const TextStyle(fontSize: 11.5, color: C.sub)),
                            ]),
                          ),
                        ]),
                      ),
                    );
                  },
                ),
        ),
        actions: (ctx) => [TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Cancel'))]);
    // The design as stored now: a sync may have brought a newer copy while
    // the dialog was up, and editing the older one would be refused.
    final d = picked == null ? null : app.design(picked.id) ?? picked;
    if (d != null && d.deleted) {
      _say('“${d.name}” was deleted on another pad or computer');
      return;
    }
    if (d != null) {
      setState(() => _open(d));
      _say(d.desktopOnly.isEmpty ? 'Opened “${d.name}”' : 'Opened “${d.name}” · ${_copyWords(d)}');
    }
  }

  Future<void> _setup() async {
    final d = _d;
    if (d == null) return;
    var name = d.name;
    var stock = d.stock;
    var material = d.material;
    final ok = await showPDialog<bool>(context,
        title: 'Document setup',
        content: StatefulBuilder(
          builder: (ctx, set) => Column(mainAxisSize: MainAxisSize.min, children: [
            FormRow('Name', LineBox(initial: name, onChanged: (v) => name = v), labelWidth: 96),
            StockFields(
                stock: stock,
                material: material,
                tools: app.tools,
                onStock: (s) => set(() => stock = s),
                onMaterial: (m) => set(() => material = m)),
          ]),
        ),
        actions: (ctx) => [
              TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Cancel')),
              TextButton(onPressed: () => Navigator.pop(ctx, true), child: const Text('OK')),
            ]);
    final newName = name.trim();
    if (ok == true && !_replacedSince(d)) {
      _commit(d.copyWith(name: newName.isEmpty ? d.name : newName, stock: stock, material: material));
    }
  }

  Future<void> _delete() async {
    final d = _d;
    if (d == null) return;
    final ok = await showPDialog<bool>(context,
        title: 'Delete “${d.name}”?',
        content: const Text('It is removed from this pad and from every phobiCCCP it syncs with.'),
        actions: (ctx) => [
              TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Cancel')),
              TextButton(onPressed: () => Navigator.pop(ctx, true), child: const Text('Delete')),
            ]);
    if (ok != true) return;
    app.deleteDesign(d.id);
    setState(() => _open(app.designs.firstOrNull));
    _say('Deleted “${d.name}”');
  }

  Future<void> _addComputer() async {
    var typed = '';
    final host = await showPDialog<String>(context,
        title: 'Add a computer',
        content: Column(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.start, children: [
          LineBox(onChanged: (v) => typed = v, autofocus: true),
          const SizedBox(height: 6),
          const Text('Its name or address, for example a Tailscale name or 192.168.1.20.', style: TextStyle(color: C.sub, fontSize: 12)),
        ]),
        actions: (ctx) => [
              TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Cancel')),
              TextButton(onPressed: () => Navigator.pop(ctx, typed), child: const Text('Add')),
            ]);
    if (host != null && host.trim().isNotEmpty) {
      app.addHost(host);
      _say('Looking for $host…');
    }
  }

  void _about() => showPDialog<void>(context,
      title: 'About PhobiCCCP',
      content: const Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Logo(size: 64),
        SizedBox(width: 16),
        Expanded(
          child: Text(
              'PhobiCCCP for pads $appVersion\n\nDesign on the pad and cut on any phobiCCCP on the same network. '
              'Designs stay in step with every phobiCCCP while the app is open, both ways.'),
        ),
      ]));

  Future<void> _sendDialog() async {
    final d = _d;
    if (d == null) return;
    final msg = await showSendDialog(context, app, d);
    if (msg != null) _say(msg);
  }

  // ---- layout ---------------------------------------------------------------------

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: C.window,
      body: SafeArea(
        child: Column(children: [
          _header(),
          if (_mode < 2) _toolbar(),
          Expanded(
            child: _mode == 2
                ? MachinePage(app: app, design: _d, onStatus: _say, onAddComputer: _addComputer)
                : _canvasPage(),
          ),
          _statusBar(),
        ]),
      ),
    );
  }

  Widget _header() {
    final d = _d;
    return Container(
      height: 46,
      decoration: const BoxDecoration(color: C.window, border: Border(bottom: BorderSide(color: C.border))),
      padding: const EdgeInsets.fromLTRB(4, 2, 10, 2),
      child: Stack(alignment: Alignment.center, children: [
        Row(children: [
          _menu('File', [
            _Item('New design…', _newDesign),
            _Item('Open…', _openDialog),
            _Item('Duplicate', d == null
                ? null
                : () {
                    final dup = app.duplicateDesign(d.id);
                    if (dup != null) setState(() => _open(dup));
                  }),
            _Item('Delete…', d == null ? null : _delete),
            null,
            _Item('Document setup…', d == null ? null : _setup),
            null,
            _Item('Send to phobiCCCP…', d == null ? null : _sendDialog),
            _Item('Sync now', app.syncNow),
          ]),
          _menu('Edit', [
            _Item('Undo', _undo.isEmpty ? null : () => _undoRedo(true)),
            _Item('Redo', _redo.isEmpty ? null : () => _undoRedo(false)),
            null,
            _Item('Delete', _sel.isEmpty ? null : _deleteSelected),
            _Item('Select all', d == null ? null : () => setState(() => _sel = {for (final s in d.shapes) s.id})),
          ]),
          _menu('View', [
            _Item('1 · Design', () => setState(() => _mode = 0), checked: _mode == 0),
            _Item('2 · Toolpaths', () => setState(() => _mode = 1), checked: _mode == 1),
            _Item('3 · Machine', () => setState(() => _mode = 2), checked: _mode == 2),
            null,
            _Item('Fit', _zoom.reset),
            _Item('Snap to grid', () => setState(() => _snap = !_snap), checked: _snap),
          ]),
          _menu('Machine', [
            for (final c in app.computers.where((c) => c.info != null)) ...[
              _Item(c.name, null),
              for (final m in c.info!.machines)
                _Item('    ${m.name}', c.reachable(app.now) && !m.active
                    ? () async {
                        try {
                          await app.selectMachine(c, m.id);
                          _say('${c.name} now uses ${m.name}');
                        } catch (e) {
                          _say('Could not switch: $e');
                        }
                      }
                    : null,
                    checked: m.active),
              null,
            ],
            _Item('Add computer…', _addComputer),
          ]),
          _menu('About', [_Item('About PhobiCCCP…', _about)]),
          const Spacer(),
          _modeSwitch(),
          const SizedBox(width: 8),
          InkWell(
            borderRadius: BorderRadius.circular(4),
            onTap: () {
              setState(() => _fullScreen = !_fullScreen);
              SystemChrome.setEnabledSystemUIMode(_fullScreen ? SystemUiMode.immersiveSticky : SystemUiMode.edgeToEdge);
            },
            child: const Padding(padding: EdgeInsets.all(6), child: PIcon('fullscreen', size: 22)),
          ),
        ]),
        IgnorePointer(
          child: Row(mainAxisSize: MainAxisSize.min, children: [
            const Logo(size: 24),
            const SizedBox(width: 8),
            const Text.rich(TextSpan(children: [
              TextSpan(text: 'phobi', style: TextStyle(fontSize: 15, fontWeight: FontWeight.w600)),
              TextSpan(text: 'CCCP', style: TextStyle(fontSize: 15, fontWeight: FontWeight.w600, color: C.amber)),
              TextSpan(text: '  v$appVersion', style: TextStyle(fontSize: 11, color: C.sub)),
            ])),
            if (d != null) ...[
              const SizedBox(width: 8),
              Text('·  ${d.name.isEmpty ? 'Untitled' : d.name}', style: const TextStyle(fontSize: 13, color: C.fileName)),
            ],
            if (app.syncing) ...[
              const SizedBox(width: 8),
              const SizedBox(width: 12, height: 12, child: CircularProgressIndicator(strokeWidth: 1.5, color: C.sub)),
            ],
          ]),
        ),
      ]),
    );
  }

  Widget _menu(String title, List<_Item?> items) => PopupMenuButton<int>(
        tooltip: '',
        position: PopupMenuPosition.under,
        offset: const Offset(0, 2),
        onSelected: (i) => items[i]?.onTap?.call(),
        itemBuilder: (_) => [
          for (var i = 0; i < items.length; i++)
            if (items[i] == null)
              const PopupMenuDivider(height: 9)
            else
              PopupMenuItem<int>(
                value: i,
                height: 34,
                enabled: items[i]!.onTap != null,
                padding: const EdgeInsets.fromLTRB(12, 0, 28, 0),
                child: Row(children: [
                  SizedBox(width: 22, child: items[i]!.checked ? const Text('✓', style: TextStyle(fontSize: 14)) : null),
                  // A menu is at most 240 wide inside its padding: a long
                  // entry (or large text) is cut short, not drawn past it.
                  Flexible(
                    child: Text(items[i]!.text,
                      overflow: TextOverflow.ellipsis,
                      style: TextStyle(
                          fontSize: 14,
                          color: items[i]!.onTap != null ? C.text : (items[i]!.text.startsWith(' ') ? C.disabled : C.label),
                          fontWeight: items[i]!.onTap == null && !items[i]!.text.startsWith(' ') ? FontWeight.w600 : null)),
                  ),
                ]),
              ),
        ],
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 7),
          child: Text.rich(TextSpan(children: [
            TextSpan(text: title[0], style: const TextStyle(decoration: TextDecoration.underline, decorationColor: C.text)),
            TextSpan(text: title.substring(1)),
          ]), style: const TextStyle(fontSize: 14)),
        ),
      );

  Widget _modeSwitch() {
    const names = ['1 · Design', '2 · Toolpaths', '3 · Machine'];
    return Container(
      padding: const EdgeInsets.all(3),
      decoration: BoxDecoration(color: C.base, border: Border.all(color: C.border), borderRadius: BorderRadius.circular(8)),
      child: Row(mainAxisSize: MainAxisSize.min, children: [
        for (var i = 0; i < 3; i++) ...[
          if (i > 0) const SizedBox(width: 3),
          Material(
            color: _mode == i ? C.modeOn : Colors.transparent,
            borderRadius: BorderRadius.circular(6),
            child: InkWell(
              borderRadius: BorderRadius.circular(6),
              onTap: () => setState(() {
                _mode = i;
                _pathDraft = null;
                if (_tool == DrawTool.path) _tool = DrawTool.select;
                _status = const [
                  'Design — draw and edit vectors',
                  'Toolpaths — select vectors, then pick a toolpath above the drawing',
                  'Machine — pick a phobiCCCP and a machine, then Start or Send',
                ][i];
              }),
              child: Padding(
                padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 6),
                child: Text(names[i],
                    style: TextStyle(
                        fontSize: 14,
                        color: _mode == i ? C.text : C.modeText,
                        fontWeight: _mode == i ? FontWeight.bold : FontWeight.normal)),
              ),
            ),
          ),
        ],
      ]),
    );
  }

  Widget _toolbar() {
    final d = _d;
    final has = d != null;
    Widget tool(DrawTool t, String icon, String tip) =>
        ToolBtn(icon: PIcon(icon, size: 22, enabled: has), tip: tip, checked: _tool == t, enabled: has, onTap: () => _setTool(t));
    Widget sep() => const SizedBox(width: 14);
    final items = <Widget>[
      tool(DrawTool.select, 'select', 'Select'),
      tool(DrawTool.hand, 'hand', 'Pan'),
      tool(DrawTool.area, 'area', 'Area: tap inside an area to select just it, like a fill, for a pocket'),
      if (_mode == 0) ...[
        tool(DrawTool.circle, 'circle', 'Circle'),
        tool(DrawTool.rect, 'rect', 'Rectangle'),
        tool(DrawTool.polygon, 'polygon', 'Polygon'),
        tool(DrawTool.text, 'text', 'Text'),
        tool(DrawTool.path, 'path', 'Path'),
        tool(DrawTool.point, 'point', 'Point (drill here)'),
      ] else ...[
        sep(),
        for (final t in ToolpathSpec.types)
          ToolBtn(
              icon: TpIcon(t, size: 26, enabled: has),
              label: tpTypeLabel(t),
              tip: 'New ${tpTypeLabel(t)} from the selected vectors',
              enabled: has,
              onTap: () => _newToolpath(t)),
      ],
      sep(),
      TextAction('Delete', icon: 'delete', enabled: _sel.isNotEmpty, onTap: _deleteSelected),
      TextAction('Snap', icon: 'snap', checked: _snap, onTap: () => setState(() => _snap = !_snap)),
      TextAction('Fit', icon: 'fit', enabled: has, onTap: _zoom.reset),
      sep(),
      TextAction('Undo', enabled: _undo.isNotEmpty, onTap: () => _undoRedo(true)),
      TextAction('Redo', enabled: _redo.isNotEmpty, onTap: () => _undoRedo(false)),
    ];
    return Container(
      color: C.window,
      padding: const EdgeInsets.symmetric(horizontal: 4, vertical: 4),
      alignment: Alignment.centerLeft,
      child: SingleChildScrollView(scrollDirection: Axis.horizontal, child: Row(children: items)),
    );
  }

  Widget _canvasPage() {
    final d = _d;
    return LayoutBuilder(builder: (context, box) {
      final right = (box.maxWidth * 0.31).clamp(360.0, 520.0);
      return Row(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        Expanded(
          child: d == null
              ? Container(
                  color: C.viewBg,
                  alignment: Alignment.center,
                  child: Column(mainAxisSize: MainAxisSize.min, children: [
                    const Text('No design open', style: TextStyle(fontSize: 16, color: C.label)),
                    const SizedBox(height: 12),
                    Row(mainAxisSize: MainAxisSize.min, children: [
                      PushBtn('New design…', onTap: _newDesign),
                      const SizedBox(width: 10),
                      PushBtn('Open…', onTap: _openDialog),
                    ]),
                  ]),
                )
              : DesignCanvas(
                  design: d,
                  tool: _tool,
                  snap: _snap,
                  zoom: _zoom,
                  selected: _sel,
                  highlighted: _mode == 1 && _tp != null && _tp! < d.toolpaths.length ? d.toolpaths[_tp!].shapes.toSet() : const {},
                  leftOf: _mode == 1 && _tp != null && _tp! < d.toolpaths.length ? _tp : null,
                  pathDraft: _pathDraft ?? const [],
                  onSelect: (ids) => setState(() => _sel = ids),
                  onMove: (ids, dx, dy) => setState(() {
                    _beforeMove ??= _d;
                    _d = _d!.copyWith(shapes: [for (final s in _d!.shapes) ids.contains(s.id) ? s.movedWithin(dx, dy) : s]);
                  }),
                  onMoveDone: () {
                    final before = _beforeMove;
                    _beforeMove = null;
                    // A newer copy came in mid-drag: it replaced the moved one.
                    if (before == null || _d == null || before.id != _d!.id) return;
                    _commit(_d!, before: before);
                  },
                  onDraw: (dr) {
                    // The design on screen now, not the one this was built from: a sync
                    // may have brought shapes since, and their ids are taken.
                    final s = shapeFromDraft(dr, (_d ?? d).nextShapeId());
                    if (s != null) _addShape(s);
                  },
                  onTapAt: _tapAt,
                ),
        ),
        Container(width: 10, color: C.border),
        SizedBox(
          width: right,
          child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
            Expanded(child: d == null ? const SizedBox() : (_mode == 0 ? _designPanel(d) : _toolpathPanel(d))),
            Padding(
                padding: const EdgeInsets.fromLTRB(6, 6, 6, 6),
                child: Align(
                    alignment: Alignment.centerLeft,
                    child: FittedBox(fit: BoxFit.scaleDown, alignment: Alignment.centerLeft, child: _zoomBar()))),
          ]),
        ),
      ]);
    });
  }

  Widget _designPanel(Design d) => Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        const PanelHead('Properties', 'properties'),
        Expanded(
          child: PropertiesPanel(
            design: d,
            selected: _sel,
            onChange: (s) {
              if (_changedUnder(d)) return;
              _commit(d.copyWith(shapes: [for (final x in d.shapes) x.id == s.id ? s : x]));
            },
          ),
        ),
        Expanded(child: DocumentTabs(design: d, tools: app.tools, selected: _sel, onPick: (id) => setState(() => _sel = {id}))),
      ]);

  Widget _toolpathPanel(Design d) => Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        const PanelHead('Toolpaths', 'toolpaths'),
        Expanded(
          child: ToolpathsPanel(
            design: d,
            tools: app.tools,
            current: _tp,
            selectedShapes: _sel,
            onPick: (i) => setState(() => _tp = i),
            shapesOpen: _shapesOpen,
            onToggleShapes: (i) => setState(() => _shapesOpen.contains(i) ? _shapesOpen.remove(i) : _shapesOpen.add(i)),
            onNew: _newToolpath,
            onChange: (i, t) {
              if (!_changedUnder(d)) _setToolpath(i, t);
            },
            onDuplicate: (i) {
              if (_changedUnder(d)) return;
              final t = d.toolpaths[i];
              // Right after the original, as on the computer: list order is cut order.
              _commit(d.copyWith(toolpaths: [...d.toolpaths]..insert(i + 1, t.copyWith(name: '${t.name} copy'))));
              _remapOpen((k) => k > i ? k + 1 : k);
              setState(() => _tp = i + 1);
            },
            onDelete: (i) {
              if (_changedUnder(d)) return;
              _commit(d.copyWith(toolpaths: [...d.toolpaths]..removeAt(i)));
              _remapOpen((k) => k == i ? -1 : (k > i ? k - 1 : k));
              setState(() => _tp = null);
            },
            onMove: (i, to) {
              if (_changedUnder(d)) return;
              final l = [...d.toolpaths];
              final t = l.removeAt(i);
              l.insert(to, t);
              _commit(d.copyWith(toolpaths: l));
              _remapOpen((k) {
                if (k == i) return to;
                final r = k > i ? k - 1 : k;
                return r >= to ? r + 1 : r;
              });
              setState(() => _tp = to);
            },
            onPickTool: _pickTool,
          ),
        ),
      ]);

  Widget _zoomBar() {
    Widget btn(String t, VoidCallback onTap) => SizedBox(
          width: 52,
          height: 40,
          child: Material(
            color: C.button,
            borderRadius: BorderRadius.circular(6),
            child: InkWell(
                borderRadius: BorderRadius.circular(6),
                onTap: onTap,
                child: Center(child: Text(t, style: const TextStyle(fontSize: 20, fontWeight: FontWeight.w600)))),
          ),
        );
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 4),
      decoration: BoxDecoration(color: const Color(0xD716181D), borderRadius: BorderRadius.circular(8)),
      child: Row(mainAxisSize: MainAxisSize.min, children: [
        btn('−', () => _zoom.zoomBy(1 / 1.25)),
        const SizedBox(width: 6),
        btn('+', () => _zoom.zoomBy(1.25)),
        ListenableBuilder(
          listenable: _zoom,
          builder: (context, _) {
            final c = _zoom.cursor;
            return Padding(
              padding: const EdgeInsets.symmetric(horizontal: 12),
              child: Text(
                  '${(_zoom.fit * _zoom.zoom * 100).round()}%   X ${c == null ? '—' : c.$1.toStringAsFixed(1)}   Y ${c == null ? '—' : c.$2.toStringAsFixed(1)}',
                  style: const TextStyle(color: C.label, fontSize: 13)),
            );
          },
        ),
      ]),
    );
  }

  Widget _statusBar() => Container(
        height: 26,
        color: C.base,
        padding: const EdgeInsets.symmetric(horizontal: 6),
        alignment: Alignment.centerLeft,
        child: Text(_status, overflow: TextOverflow.ellipsis, style: const TextStyle(fontSize: 12.5, fontWeight: FontWeight.w500)),
      );
}

class _Item {
  final String text;
  final VoidCallback? onTap;
  final bool checked;
  const _Item(this.text, this.onTap, {this.checked = false});
}
