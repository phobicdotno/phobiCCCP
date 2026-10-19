// The design editor, laid out for a pad: the drawing fills the left, shapes,
// their properties and the toolpaths sit in a panel on the right. On a narrow
// screen the panel goes under the drawing.

import 'package:flutter/material.dart';

import '../app_state.dart';
import '../model.dart';
import 'canvas.dart';
import 'send_dialog.dart';
import 'theme.dart';
import 'widgets.dart';

double _r(double v) => (v * 100).roundToDouble() / 100;

class EditorScreen extends StatefulWidget {
  final AppState app;
  final String id;
  const EditorScreen(this.app, this.id, {super.key});
  @override
  State<EditorScreen> createState() => _EditorScreenState();
}

class _EditorScreenState extends State<EditorScreen> {
  late Design _d = widget.app.design(widget.id) ?? Design(id: widget.id);
  final _zoom = CanvasZoom();
  String? _selected;
  List<List<double>>? _pathDraft;
  bool _pathClosed = true;
  int? _highlightToolpath;

  AppState get app => widget.app;

  @override
  void initState() {
    super.initState();
    app.addListener(_synced);
  }

  @override
  void dispose() {
    app.removeListener(_synced);
    super.dispose();
  }

  // A newer copy came in from a phobiCCCP (edited and saved there, or on
  // another pad): show it in place of ours.
  void _synced() {
    final d = app.design(widget.id);
    if (d == null || d.modified <= _d.modified || !mounted) return;
    setState(() {
      _d = d;
      if (!d.shapes.any((s) => s.id == _selected)) _selected = null;
    });
    if (d.deleted) Navigator.of(context).maybePop();
  }

  void _commit(Design next) => setState(() => _d = app.saveDesign(next));

  void _addShape(Shape s) {
    _commit(_d.copyWith(shapes: [..._d.shapes, s]));
    setState(() => _selected = s.id);
  }

  void _deleteShape(String id) {
    _commit(_d.copyWith(
      shapes: _d.shapes.where((s) => s.id != id).toList(),
      toolpaths: [for (final t in _d.toolpaths) t.copyWith(shapes: t.shapes.where((x) => x != id).toList())],
    ));
    setState(() => _selected = null);
  }

  void _select(String? id) => setState(() => _selected = id);

  void _startPath() => setState(() {
        _selected = null;
        _pathDraft = [];
      });

  void _replaceShape(Shape s) => _commit(_d.copyWith(shapes: [for (final x in _d.shapes) x.id == s.id ? s : x]));

  @override
  Widget build(BuildContext context) {
    final wide = isWide(context);
    final canvas = Stack(children: [
      Positioned.fill(
        child: DesignCanvas(
          design: _d,
          selected: _selected,
          zoom: _zoom,
          highlighted: _highlightToolpath == null || _highlightToolpath! >= _d.toolpaths.length
              ? const {}
              : _d.toolpaths[_highlightToolpath!].shapes.toSet(),
          pathDraft: _pathDraft ?? const [],
          onSelect: _select,
          onMove: (id, dx, dy) => setState(() => _d = _d.copyWith(shapes: [for (final s in _d.shapes) s.id == id ? s.moved(dx, dy) : s])),
          onMoveDone: () => _commit(_d),
          onAddPoint: _pathDraft == null ? null : (x, y) => setState(() => _pathDraft = [..._pathDraft!, [_r(x), _r(y)]]),
        ),
      ),
      Positioned(
        right: 12,
        bottom: 12,
        child: ListenableBuilder(
          listenable: _zoom,
          builder: (context, _) => _zoom.zoomed
              ? FloatingActionButton.small(
                  heroTag: 'fit', onPressed: _zoom.fit, backgroundColor: Brand.surfaceHigh, tooltip: 'Fit', child: const Icon(Icons.fit_screen))
              : const SizedBox.shrink(),
        ),
      ),
      if (_pathDraft != null)
        Positioned(
          left: 12,
          right: 12,
          top: 12,
          child: Material(
            color: Brand.surface,
            borderRadius: BorderRadius.circular(8),
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 4),
              child: Row(children: [
                Expanded(child: Text('Tap to add points (${_pathDraft!.length})')),
                Checkbox(value: _pathClosed, onChanged: (v) => setState(() => _pathClosed = v ?? true)),
                const Text('Closed'),
                TextButton(onPressed: () => setState(() => _pathDraft = null), child: const Text('Cancel')),
                TextButton(
                  onPressed: _pathDraft!.length < 2
                      ? null
                      : () {
                          final pts = _pathDraft!;
                          setState(() => _pathDraft = null);
                          _addShape(Shape(id: _d.nextShapeId(), type: Shape.path, points: pts, closed: _pathClosed && pts.length >= 3));
                        },
                  child: const Text('Done'),
                ),
              ]),
            ),
          ),
        ),
    ]);

    final panel = _Panel(this);
    return Scaffold(
      appBar: AppBar(
        title: Text(_d.name.isEmpty ? 'Untitled' : _d.name),
        actions: [
          IconButton(onPressed: _editDesign, icon: const Icon(Icons.tune), tooltip: 'Name, stock and material'),
          Padding(
            padding: const EdgeInsets.only(right: 8),
            child: FilledButton.icon(
                onPressed: () => showSendDialog(context, app, _d), icon: const Icon(Icons.send), label: const Text('Send')),
          ),
        ],
      ),
      body: wide
          ? Row(children: [
              Expanded(child: canvas),
              Container(width: 400, color: Brand.surface, child: panel),
            ])
          : Column(children: [
              Expanded(flex: 3, child: canvas),
              Expanded(flex: 2, child: Container(color: Brand.surface, child: panel)),
            ]),
    );
  }

  Future<void> _editDesign() async {
    final next = await showDialog<Design>(context: context, builder: (_) => _DesignDialog(app, _d));
    if (next != null) _commit(next);
  }

  Future<void> _editToolpath(int? index) async {
    setState(() => _highlightToolpath = index);
    final res = await showDialog<Object>(
        context: context,
        builder: (_) => _ToolpathDialog(
            app.tools, _d, index == null ? null : _d.toolpaths[index], _selected == null ? const [] : [_selected!]));
    setState(() => _highlightToolpath = null);
    if (res == null) return;
    final list = [..._d.toolpaths];
    if (res == 'delete' && index != null) {
      list.removeAt(index);
    } else if (res is ToolpathSpec) {
      index == null ? list.add(res) : list[index] = res;
    }
    _commit(_d.copyWith(toolpaths: list));
  }
}

class _Panel extends StatelessWidget {
  final _EditorScreenState e;
  const _Panel(this.e);

  @override
  Widget build(BuildContext context) {
    final d = e._d;
    final cx = d.stock.width / 2, cy = d.stock.height / 2;
    final u = (d.stock.width < d.stock.height ? d.stock.width : d.stock.height) / 4;
    final sel = e._selected == null ? null : d.shape(e._selected!);
    Widget add(IconData icon, String label, VoidCallback on) =>
        ActionChip(avatar: Icon(icon, size: 18), label: Text(label), onPressed: on);
    return DefaultTabController(
      length: 2,
      child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(12, 12, 12, 4),
          child: Wrap(spacing: 8, runSpacing: 8, children: [
            add(Icons.crop_square, 'Rectangle',
                () => e._addShape(Shape(id: d.nextShapeId(), type: Shape.rect, x: _r(cx - u), y: _r(cy - u / 2), w: _r(u * 2), h: _r(u)))),
            add(Icons.circle_outlined, 'Circle', () => e._addShape(Shape(id: d.nextShapeId(), type: Shape.circle, x: _r(cx), y: _r(cy), r: _r(u / 2)))),
            add(Icons.hexagon_outlined, 'Polygon',
                () => e._addShape(Shape(id: d.nextShapeId(), type: Shape.polygon, x: _r(cx), y: _r(cy), r: _r(u / 2), sides: 6))),
            add(Icons.text_fields, 'Text',
                () => e._addShape(Shape(id: d.nextShapeId(), type: Shape.text, x: _r(cx - u), y: _r(cy), label: 'Text', size: _r(u / 2)))),
            add(Icons.polyline_outlined, 'Path', e._startPath),
            add(Icons.add, 'Drill point', () => e._addShape(Shape(id: d.nextShapeId(), type: Shape.point, x: _r(cx), y: _r(cy)))),
          ]),
        ),
        if (sel != null) _ShapeProperties(sel, key: ValueKey(sel.id), onChanged: e._replaceShape, onDelete: () => e._deleteShape(sel.id)),
        TabBar(tabs: [Tab(text: 'Shapes (${d.shapes.length})'), Tab(text: 'Toolpaths (${d.toolpaths.length})')]),
        Expanded(
          child: TabBarView(children: [
            ListView(children: [
              for (final s in d.shapes)
                ListTile(
                  dense: true,
                  selected: s.id == e._selected,
                  selectedColor: Brand.gold,
                  title: Text(s.describe()),
                  onTap: () => e._select(s.id),
                ),
            ]),
            ListView(children: [
              Padding(
                padding: const EdgeInsets.all(8),
                child: OutlinedButton.icon(
                    onPressed: d.shapes.isEmpty ? null : () => e._editToolpath(null), icon: const Icon(Icons.add), label: const Text('Add toolpath')),
              ),
              for (var i = 0; i < d.toolpaths.length; i++)
                ListTile(
                  dense: true,
                  title: Text('${i + 1}. ${d.toolpaths[i].name}'),
                  subtitle: Text(_toolpathLine(d.toolpaths[i], e.app.tools)),
                  onTap: () => e._editToolpath(i),
                  trailing: Row(mainAxisSize: MainAxisSize.min, children: [
                    IconButton(
                        icon: const Icon(Icons.arrow_upward),
                        tooltip: 'Earlier',
                        onPressed: i == 0 ? null : () => e._commit(d.copyWith(toolpaths: [...d.toolpaths]..insert(i - 1, d.toolpaths[i])..removeAt(i + 1)))),
                    IconButton(
                        icon: const Icon(Icons.arrow_downward),
                        tooltip: 'Later',
                        onPressed: i == d.toolpaths.length - 1
                            ? null
                            : () => e._commit(d.copyWith(toolpaths: [...d.toolpaths]..insert(i + 2, d.toolpaths[i])..removeAt(i)))),
                  ]),
                ),
            ]),
          ]),
        ),
      ]),
    );
  }

  static String _toolpathLine(ToolpathSpec t, ToolLibrary tools) {
    final tool = tools.tool(t.tool)?.name ?? 'tool #${t.tool}';
    return '${ToolpathSpec.typeLabel(t.type)}${t.type == 'profile' ? ' ${t.side}' : ''}, ${fmt(t.depth)} mm deep, $tool, '
        '${t.shapes.length} shape(s)';
  }
}

/// The selected shape's numbers, edited in place.
class _ShapeProperties extends StatelessWidget {
  final Shape s;
  final ValueChanged<Shape> onChanged;
  final VoidCallback onDelete;
  const _ShapeProperties(this.s, {super.key, required this.onChanged, required this.onDelete});

  @override
  Widget build(BuildContext context) {
    final what = switch (s.type) { Shape.rect => 'corner', Shape.text => 'start', _ => 'centre' };
    Widget pair(Widget a, Widget b) => Row(children: [Expanded(child: a), const SizedBox(width: 8), Expanded(child: b)]);
    return Container(
      margin: const EdgeInsets.fromLTRB(12, 8, 12, 8),
      padding: const EdgeInsets.all(12),
      decoration: BoxDecoration(color: Brand.surfaceHigh, borderRadius: BorderRadius.circular(8)),
      child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        Row(children: [
          Expanded(child: Text(s.describe(), style: const TextStyle(color: Brand.gold, fontWeight: FontWeight.w600))),
          IconButton(onPressed: onDelete, icon: const Icon(Icons.delete_outline), tooltip: 'Delete shape'),
        ]),
        const SizedBox(height: 8),
        if (s.type != Shape.path)
          pair(NumberField('X ($what)', s.x, (v) => onChanged(s.copyWith(x: v))), NumberField('Y ($what)', s.y, (v) => onChanged(s.copyWith(y: v)))),
        const SizedBox(height: 8),
        ...switch (s.type) {
          Shape.rect => [
              pair(NumberField('Width', s.w ?? 10, (v) => onChanged(s.copyWith(w: v)), min: 0.01),
                  NumberField('Height', s.h ?? 10, (v) => onChanged(s.copyWith(h: v)), min: 0.01))
            ],
          Shape.circle => [NumberField('Diameter', (s.r ?? 5) * 2, (v) => onChanged(s.copyWith(r: v / 2)), min: 0.02)],
          Shape.polygon => [
              pair(NumberField('Radius', s.r ?? 5, (v) => onChanged(s.copyWith(r: v)), min: 0.01),
                  NumberField('Sides', (s.sides ?? 6).toDouble(), (v) => onChanged(s.copyWith(sides: v.round())), min: 3, max: 100))
            ],
          Shape.text => [
              TextFormField(
                initialValue: s.label,
                decoration: const InputDecoration(labelText: 'Text', border: OutlineInputBorder(), isDense: true),
                onChanged: (t) => t.trim().isEmpty ? null : onChanged(s.copyWith(label: t.length > 500 ? t.substring(0, 500) : t)),
              ),
              const SizedBox(height: 8),
              NumberField('Letter height', s.size ?? 10, (v) => onChanged(s.copyWith(size: v)), min: 0.5, max: 2000),
            ],
          Shape.path => [
              Row(children: [
                Checkbox(
                    value: s.closed == true,
                    onChanged: (s.points?.length ?? 0) < 3 ? null : (v) => onChanged(s.copyWith(closed: v ?? false))),
                const Text('Closed'),
                const Spacer(),
                const Text('Drag it on the drawing to move it.', style: TextStyle(color: Brand.textDim, fontSize: 12)),
              ])
            ],
          _ => <Widget>[],
        },
      ]),
    );
  }
}

class _DesignDialog extends StatefulWidget {
  final AppState app;
  final Design d;
  const _DesignDialog(this.app, this.d);
  @override
  State<_DesignDialog> createState() => _DesignDialogState();
}

class _DesignDialogState extends State<_DesignDialog> {
  late final _name = TextEditingController(text: widget.d.name);
  late Stock _stock = widget.d.stock;
  late String _material = widget.d.material;

  @override
  Widget build(BuildContext context) => AlertDialog(
        title: const Text('Design'),
        content: SizedBox(
          width: 520,
          child: Column(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.stretch, children: [
            TextField(controller: _name, decoration: const InputDecoration(labelText: 'Name', border: OutlineInputBorder())),
            const SizedBox(height: 16),
            StockFields(
                stock: _stock,
                material: _material,
                tools: widget.app.tools,
                onStock: (s) => setState(() => _stock = s),
                onMaterial: (m) => setState(() => _material = m)),
          ]),
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
          FilledButton(
              onPressed: () => Navigator.pop(context, widget.d.copyWith(name: _name.text.trim(), stock: _stock, material: _material)),
              child: const Text('OK')),
        ],
      );
}

/// Pops a ToolpathSpec, 'delete', or nothing.
class _ToolpathDialog extends StatefulWidget {
  final ToolLibrary tools;
  final Design d;
  final ToolpathSpec? spec;
  final List<String> preselect;
  const _ToolpathDialog(this.tools, this.d, this.spec, this.preselect);
  @override
  State<_ToolpathDialog> createState() => _ToolpathDialogState();
}

class _ToolpathDialogState extends State<_ToolpathDialog> {
  late ToolpathSpec _t = widget.spec ??
      ToolpathSpec(
        name: 'Toolpath ${widget.d.toolpaths.length + 1}',
        type: 'profile',
        shapes: widget.preselect.isNotEmpty ? widget.preselect : widget.d.shapes.map((s) => s.id).toList(),
        tool: widget.tools.defaultTool(widget.d.material, 'profile') ?? 201,
        depth: widget.d.stock.thickness < 3 ? widget.d.stock.thickness : 3,
      );
  late final _name = TextEditingController(text: _t.name);

  @override
  Widget build(BuildContext context) {
    final d = widget.d, tools = widget.tools;
    final available = tools.toolsFor(d.material);
    final toolOk = available.isEmpty || available.any((x) => x.number == _t.tool);
    return AlertDialog(
      title: Text(widget.spec == null ? 'New toolpath' : 'Toolpath'),
      content: SizedBox(
        width: 520,
        child: SingleChildScrollView(
          child: Column(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.stretch, children: [
            TextField(controller: _name, decoration: const InputDecoration(labelText: 'Name', border: OutlineInputBorder())),
            const SizedBox(height: 12),
            SegmentedButton<String>(
              segments: [for (final ty in ToolpathSpec.types) ButtonSegment(value: ty, label: Text(ToolpathSpec.typeLabel(ty)))],
              selected: {_t.type},
              onSelectionChanged: (v) => setState(() {
                final ty = v.first;
                final keep = ty != 'engrave' && available.any((x) => x.number == _t.tool);
                _t = _t.copyWith(type: ty, tool: keep ? _t.tool : tools.defaultTool(d.material, ty) ?? _t.tool);
              }),
            ),
            if (_t.type == 'profile') ...[
              const SizedBox(height: 12),
              SegmentedButton<String>(
                segments: const [
                  ButtonSegment(value: 'outside', label: Text('Outside')),
                  ButtonSegment(value: 'inside', label: Text('Inside')),
                  ButtonSegment(value: 'on', label: Text('On the line')),
                ],
                selected: {_t.side},
                onSelectionChanged: (v) => setState(() => _t = _t.copyWith(side: v.first)),
              ),
            ],
            const SizedBox(height: 12),
            NumberField('Depth below the top (mm)', _t.depth, (v) => setState(() => _t = _t.copyWith(depth: v)), min: 0, max: 1000),
            if (_t.depth > d.stock.thickness)
              Text('Deeper than the stock (${fmt(d.stock.thickness)} mm): it cuts into the spoilboard.',
                  style: const TextStyle(color: Brand.amber, fontSize: 12)),
            const SizedBox(height: 12),
            if (available.isEmpty) ...[
              NumberField('Tool number', _t.tool.toDouble(), (v) => setState(() => _t = _t.copyWith(tool: v.round())), min: 1, max: 1e9),
              const Text('The tool list comes from phobiCCCP\'s library once the pad has found it.',
                  style: TextStyle(color: Brand.textDim, fontSize: 12)),
            ] else
              DropdownButtonFormField<int>(
                initialValue: toolOk ? _t.tool : null,
                isExpanded: true,
                decoration: InputDecoration(labelText: 'Tool (${tools.materialName(d.material)})', border: const OutlineInputBorder()),
                items: [
                  for (final x in available)
                    DropdownMenuItem(value: x.number, child: Text('${x.name}${x.diameter > 0 ? ' · Ø${fmt(x.diameter)}' : ''}')),
                ],
                onChanged: (v) => v == null ? null : setState(() => _t = _t.copyWith(tool: v)),
              ),
            const SizedBox(height: 12),
            const Text('Shapes', style: TextStyle(color: Brand.textDim, fontSize: 12)),
            for (final s in d.shapes)
              CheckboxListTile(
                dense: true,
                contentPadding: EdgeInsets.zero,
                controlAffinity: ListTileControlAffinity.leading,
                value: _t.shapes.contains(s.id),
                title: Text(s.describe()),
                onChanged: (v) => setState(() => _t = _t.copyWith(
                    shapes: v == true ? [..._t.shapes, s.id] : _t.shapes.where((x) => x != s.id).toList())),
              ),
          ]),
        ),
      ),
      actions: [
        if (widget.spec != null) TextButton(onPressed: () => Navigator.pop(context, 'delete'), child: const Text('Delete')),
        TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
        FilledButton(
          onPressed: _t.shapes.isEmpty || !toolOk
              ? null
              : () => Navigator.pop(
                  context, _t.copyWith(name: _name.text.trim().isEmpty ? ToolpathSpec.typeLabel(_t.type) : _name.text.trim())),
          child: const Text('OK'),
        ),
      ],
    );
  }
}
