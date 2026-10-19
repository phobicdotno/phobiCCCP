// The right-hand panels, as the desktop has them: Properties over the
// Document tab in Design, the Toolpaths list and form in Toolpaths
// (propertiespanel.cpp, toolpathpanel.cpp, toolpathform.cpp).

import 'package:flutter/material.dart';

import '../model.dart';
import 'icons.dart';
import 'theme.dart';
import 'widgets.dart';

// ---- Design: Properties ------------------------------------------------------

class PropertiesPanel extends StatelessWidget {
  final Design design;
  final Set<String> selected;
  final ValueChanged<Shape> onChange;
  const PropertiesPanel({super.key, required this.design, required this.selected, required this.onChange});

  @override
  Widget build(BuildContext context) {
    final shapes = design.shapes.where((s) => selected.contains(s.id)).toList();
    final s = shapes.length == 1 ? shapes.first : null;
    final rows = <Widget>[];
    String title;
    if (s == null) {
      title = shapes.isEmpty ? 'no selection' : '${shapes.length} selected';
      for (final l in ['Center', 'Radius', 'W × H', 'Sides']) {
        rows.add(
          Padding(
            padding: const EdgeInsets.symmetric(vertical: 6),
            child: Text(l, style: const TextStyle(color: C.disabled, fontSize: 13)),
          ),
        );
      }
    } else {
      title = _typeName(s);
      final b = s.bounds();
      final cx = (b[0] + b[2]) / 2, cy = (b[1] + b[3]) / 2;
      Widget pair(Widget a, Widget b) => Row(
        children: [
          Expanded(child: a),
          const SizedBox(width: 6),
          Expanded(child: b),
        ],
      );
      switch (s.type) {
        case Shape.rect:
          final w = s.w ?? 0, h = s.h ?? 0;
          rows.add(
            FormRow(
              'Center',
              pair(
                NumBox(s.x + w / 2, (v) => onChange(s.copyWith(x: v - w / 2))),
                NumBox(s.y + h / 2, (v) => onChange(s.copyWith(y: v - h / 2))),
              ),
            ),
          );
          rows.add(
            FormRow(
              'W × H',
              pair(
                NumBox(w, (v) => onChange(s.copyWith(x: s.x + (w - v) / 2, w: v)), min: 0.01),
                NumBox(h, (v) => onChange(s.copyWith(y: s.y + (h - v) / 2, h: v)), min: 0.01),
              ),
            ),
          );
        case Shape.circle:
        case Shape.polygon:
          rows.add(
            FormRow(
              'Center',
              pair(NumBox(s.x, (v) => onChange(s.copyWith(x: v))), NumBox(s.y, (v) => onChange(s.copyWith(y: v)))),
            ),
          );
          rows.add(FormRow('Radius', NumBox(s.r ?? 0, (v) => onChange(s.copyWith(r: v)), min: 0.01)));
          if (s.type == Shape.polygon) {
            rows.add(
              FormRow(
                'Sides',
                NumBox(
                  (s.sides ?? 6).toDouble(),
                  (v) => onChange(s.copyWith(sides: v.round())),
                  min: 3,
                  max: 64,
                  decimals: 0,
                  suffix: '',
                ),
              ),
            );
          }
        case Shape.text:
          rows.add(
            FormRow(
              'Position',
              pair(NumBox(s.x, (v) => onChange(s.copyWith(x: v))), NumBox(s.y, (v) => onChange(s.copyWith(y: v)))),
            ),
          );
          rows.add(FormRow('Text', _TextBox(s.label ?? '', (t) => onChange(s.copyWith(label: t)))));
          rows.add(
            FormRow('Height', NumBox(s.size ?? 10, (v) => onChange(s.copyWith(size: v)), min: 0.5, max: 1000, decimals: 2)),
          );
        default:
          rows.add(
            FormRow(
              'Center',
              pair(NumBox(cx, (v) => onChange(s.moved(v - cx, 0))), NumBox(cy, (v) => onChange(s.moved(0, v - cy)))),
            ),
          );
          if (s.type == Shape.path) {
            rows.add(
              FormRow('Size', Text('${fmt(b[2] - b[0])} × ${fmt(b[3] - b[1])} mm', style: const TextStyle(color: C.label))),
            );
          }
      }
    }
    return ListView(
      padding: const EdgeInsets.all(10),
      children: [
        Text(
          title,
          style: TextStyle(fontWeight: FontWeight.bold, color: s == null ? C.label : C.text),
        ),
        const SizedBox(height: 6),
        ...rows,
      ],
    );
  }

  static String _typeName(Shape s) => switch (s.type) {
    Shape.rect => 'Rectangle',
    Shape.circle => 'Circle',
    Shape.polygon => 'Polygon',
    Shape.text => 'Text',
    Shape.path => s.closed == true ? 'Closed path' : 'Path',
    Shape.point => 'Point',
    _ => s.type,
  };
}

class _TextBox extends StatefulWidget {
  final String value;
  final ValueChanged<String> onChanged;
  const _TextBox(this.value, this.onChanged);
  @override
  State<_TextBox> createState() => _TextBoxState();
}

class _TextBoxState extends State<_TextBox> {
  late final _c = TextEditingController(text: widget.value);
  @override
  void didUpdateWidget(_TextBox old) {
    super.didUpdateWidget(old);
    if (_c.text != widget.value && old.value != widget.value) _c.text = widget.value;
  }

  @override
  void dispose() {
    _c.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => SizedBox(
    height: 30,
    child: TextField(
      controller: _c,
      style: const TextStyle(fontSize: 13),
      onSubmitted: (t) => t.trim().isEmpty ? null : widget.onChanged(t),
      onTapOutside: (_) {
        if (_c.text.trim().isNotEmpty && _c.text != widget.value) widget.onChanged(_c.text);
      },
    ),
  );
}

// ---- Design: Document / Shapes tabs -------------------------------------------

class DocumentTabs extends StatefulWidget {
  final Design design;
  final ToolLibrary tools;
  final Set<String> selected;
  final ValueChanged<String> onPick;
  const DocumentTabs({super.key, required this.design, required this.tools, required this.selected, required this.onPick});
  @override
  State<DocumentTabs> createState() => _DocumentTabsState();
}

class _DocumentTabsState extends State<DocumentTabs> {
  int _tab = 0;

  String _info() {
    final d = widget.design;
    final counts = <String, int>{};
    for (final s in d.shapes) {
      counts[s.type] = (counts[s.type] ?? 0) + 1;
    }
    final b = StringBuffer()
      ..writeln('== params ==')
      ..writeln('height = ${fmt(d.stock.height)}')
      ..writeln('material = ${widget.tools.materialName(d.material)}')
      ..writeln('num_toolpaths = ${d.toolpaths.length}')
      ..writeln('thickness = ${fmt(d.stock.thickness)}')
      ..writeln('width = ${fmt(d.stock.width)}')
      ..writeln()
      ..writeln('== elements (${d.shapes.length}) ==');
    for (final k in counts.keys.toList()..sort()) {
      b.writeln('$k: ${counts[k]}');
    }
    b
      ..writeln()
      ..writeln('== toolpaths (${d.toolpaths.length}) ==');
    for (final t in d.toolpaths) {
      b.writeln('${t.name}  [${t.type}]');
    }
    return b.toString();
  }

  Widget _tabBtn(int i, String icon, String text) {
    final on = _tab == i;
    return GestureDetector(
      onTap: () => setState(() => _tab = i),
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 7),
        decoration: BoxDecoration(
          color: on ? C.window : C.base,
          border: Border(
            top: BorderSide(color: on ? C.cardBorder : C.border),
            left: BorderSide(color: on ? C.cardBorder : C.border),
            right: BorderSide(color: on ? C.cardBorder : C.border),
          ),
        ),
        child: Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            PIcon(icon, size: 18),
            const SizedBox(width: 8),
            Text(text, style: const TextStyle(fontWeight: FontWeight.bold, fontSize: 13)),
          ],
        ),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final d = widget.design;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Container(
          color: C.window,
          padding: const EdgeInsets.only(top: 8),
          child: Row(children: [_tabBtn(0, 'document', 'Document'), _tabBtn(1, 'properties', 'Shapes')]),
        ),
        Expanded(
          child: Container(
            color: C.base,
            child: _tab == 0
                ? SingleChildScrollView(
                    padding: const EdgeInsets.all(4),
                    child: Text(
                      _info(),
                      style: const TextStyle(fontFamily: kMono, fontSize: 12, color: C.text, height: 1.17),
                    ),
                  )
                : ListView(
                    children: [
                      for (final s in d.shapes)
                        InkWell(
                          onTap: () => widget.onPick(s.id),
                          child: Container(
                            color: widget.selected.contains(s.id) ? C.amber : null,
                            padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 6),
                            child: Text(
                              s.describe(),
                              style: TextStyle(
                                fontSize: 13,
                                color: widget.selected.contains(s.id) ? const Color(0xFF1A1A1A) : C.text,
                              ),
                            ),
                          ),
                        ),
                    ],
                  ),
          ),
        ),
      ],
    );
  }
}

// ---- Toolpaths -------------------------------------------------------------------

String tpTypeLabel(String t) => const {'profile': 'Contour', 'pocket': 'Pocket', 'drill': 'Drill', 'engrave': 'Engrave'}[t] ?? t;

class ToolpathsPanel extends StatelessWidget {
  final Design design;
  final ToolLibrary tools;
  final int? current;
  final Set<String> selectedShapes;
  final ValueChanged<int?> onPick;
  final void Function(String type) onNew;
  final void Function(int i, ToolpathSpec t) onChange;
  final void Function(int i) onDuplicate, onDelete;
  final void Function(int i, int to) onMove;
  final void Function(int i) onPickTool;

  const ToolpathsPanel({
    super.key,
    required this.design,
    required this.tools,
    required this.current,
    required this.selectedShapes,
    required this.onPick,
    required this.onNew,
    required this.onChange,
    required this.onDuplicate,
    required this.onDelete,
    required this.onMove,
    required this.onPickTool,
  });

  Widget _txt(String t, VoidCallback? onTap) => InkWell(
    borderRadius: BorderRadius.circular(4),
    onTap: onTap,
    child: Padding(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 8),
      child: Text(t, style: TextStyle(fontSize: 14.5, color: onTap == null ? C.disabled : C.text)),
    ),
  );

  @override
  Widget build(BuildContext context) {
    final tps = design.toolpaths;
    final i = current != null && current! < tps.length ? current : null;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(6, 4, 6, 0),
          child: FittedBox(
            fit: BoxFit.scaleDown,
            alignment: Alignment.centerLeft,
            child: SizedBox(
              width: 460,
              child: Row(
                children: [
                  PopupMenuButton<String>(
                    tooltip: '',
                    position: PopupMenuPosition.under,
                    onSelected: onNew,
                    itemBuilder: (_) => [
                      for (final t in ToolpathSpec.types)
                        PopupMenuItem(
                          value: t,
                          height: 40,
                          child: Row(children: [TpIcon(t, size: 22), const SizedBox(width: 10), Text(tpTypeLabel(t))]),
                        ),
                    ],
                    child: const Padding(
                      padding: EdgeInsets.symmetric(horizontal: 10, vertical: 8),
                      child: Row(
                        mainAxisSize: MainAxisSize.min,
                        children: [
                          Text('New', style: TextStyle(fontSize: 14.5)),
                          SizedBox(width: 6),
                          Caret(size: 8, color: C.text),
                        ],
                      ),
                    ),
                  ),
                  _txt('Duplicate', i == null ? null : () => onDuplicate(i)),
                  _txt('Delete', i == null ? null : () => onDelete(i)),
                  const Spacer(),
                  _txt('▲', i == null || i == 0 ? null : () => onMove(i, i - 1)),
                  _txt('▼', i == null || i == tps.length - 1 ? null : () => onMove(i, i + 1)),
                ],
              ),
            ),
          ),
        ),
        Expanded(
          child: ListView(
            padding: const EdgeInsets.fromLTRB(6, 0, 6, 10),
            children: [
              for (var k = 0; k < tps.length; k++) _card(k, tps[k], k == i),
              if (tps.isEmpty)
                const Padding(
                  padding: EdgeInsets.all(10),
                  child: Text(
                    'Select shapes, then pick a toolpath above the drawing (or New).',
                    style: TextStyle(color: C.sub, fontSize: 13),
                  ),
                ),
              if (i != null) _form(context, i, tps[i]),
            ],
          ),
        ),
      ],
    );
  }

  Widget _card(int k, ToolpathSpec t, bool sel) {
    final tool = tools.tool(t.tool);
    return GestureDetector(
      onTap: () => onPick(sel ? null : k),
      child: Container(
        margin: const EdgeInsets.symmetric(vertical: 3),
        height: 54,
        decoration: BoxDecoration(
          color: sel ? C.selCard : C.card,
          borderRadius: BorderRadius.circular(8),
          border: Border.all(color: sel ? C.amber : C.cardBorder, width: sel ? 2 : 1),
        ),
        padding: const EdgeInsets.symmetric(horizontal: 10),
        child: Row(
          children: [
            GestureDetector(
              onTap: () => onChange(k, t.copyWith(enabled: !t.enabled)),
              child: Container(
                width: 26,
                height: 26,
                decoration: BoxDecoration(
                  color: t.enabled ? C.amber : Colors.transparent,
                  borderRadius: BorderRadius.circular(5),
                  border: Border.all(color: t.enabled ? C.amber : C.disabled, width: 1.6),
                ),
                child: t.enabled ? const Icon(Icons.check_rounded, size: 20, color: C.base) : null,
              ),
            ),
            const SizedBox(width: 10),
            TpIcon(t.type, size: 30, enabled: t.enabled),
            const SizedBox(width: 10),
            Expanded(
              child: Column(
                mainAxisAlignment: MainAxisAlignment.center,
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    t.name.isEmpty ? tpTypeLabel(t.type) : t.name,
                    overflow: TextOverflow.ellipsis,
                    style: TextStyle(fontWeight: FontWeight.bold, fontSize: 13.5, color: t.enabled ? C.nameOn : C.disabled),
                  ),
                  Text(
                    '${tpTypeLabel(t.type)} · ${t.shapes.length} vector${t.shapes.length == 1 ? '' : 's'} · ${fmt(t.depth)} mm deep',
                    overflow: TextOverflow.ellipsis,
                    style: const TextStyle(fontSize: 11.5, color: C.sub),
                  ),
                ],
              ),
            ),
            if (tool != null) ...[
              ToolGlyph(tool.kind, height: 34),
              const SizedBox(width: 6),
              Text('Ø${fmt(tool.diameter)}', style: const TextStyle(color: C.label, fontSize: 13.5)),
            ],
          ],
        ),
      ),
    );
  }

  Widget _form(BuildContext context, int i, ToolpathSpec t) {
    final tool = tools.tool(t.tool);
    final inSel = selectedShapes.where((id) => design.shape(id) != null).toList();
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        const SizedBox(height: 6),
        Row(
          children: [
            Expanded(child: _NameBox(t.name, (n) => onChange(i, t.copyWith(name: n)))),
            const SizedBox(width: 8),
            Switch(
              value: t.enabled,
              onChanged: (v) => onChange(i, t.copyWith(enabled: v)),
            ),
          ],
        ),
        PCard(
          padding: const EdgeInsets.all(10),
          child: Row(
            children: [
              ToolGlyph(tool?.kind ?? 'endmill', height: 44),
              const SizedBox(width: 12),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      tool == null
                          ? 'T${t.tool}'
                          : 'T${tool.number} Ø ${fmt(tool.diameter)} mm${tool.flutes > 0 ? ' · ${tool.flutes} fl' : ''}',
                      style: const TextStyle(fontWeight: FontWeight.bold, fontSize: 14),
                    ),
                    Text(tool?.name ?? 'not in the tool library yet', style: const TextStyle(color: C.label, fontSize: 13)),
                  ],
                ),
              ),
              PushBtn('Change…', height: 30, fontSize: 13, onTap: () => onPickTool(i)),
            ],
          ),
        ),
        PCard(
          title: 'Depth',
          child: Column(children: [_row('End', StepperField(t.depth, (v) => onChange(i, t.copyWith(depth: v)), max: 1000))]),
        ),
        if (t.type == 'profile')
          PCard(
            title: 'Cutting',
            child: _row(
              'Side',
              SizedBox(
                width: 218,
                child: Combo<String>(
                  value: t.side,
                  items: const [('outside', 'Outside'), ('inside', 'Inside'), ('on', 'On the line')],
                  onChanged: (v) => onChange(i, t.copyWith(side: v)),
                ),
              ),
            ),
          ),
        PCard(
          title: 'Vectors',
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              Padding(
                padding: const EdgeInsets.symmetric(horizontal: 4, vertical: 4),
                child: Text('${t.shapes.length} assigned · ${inSel.length} selected', style: const TextStyle(color: C.label)),
              ),
              PushBtn(
                'Assign selected vectors',
                height: 34,
                fontSize: 13,
                onTap: inSel.isEmpty ? null : () => onChange(i, t.copyWith(shapes: inSel)),
              ),
            ],
          ),
        ),
      ],
    );
  }

  Widget _row(String label, Widget field) => Padding(
    padding: const EdgeInsets.symmetric(vertical: 3),
    child: Row(
      children: [
        Expanded(
          child: Padding(
            padding: const EdgeInsets.only(left: 4),
            child: Text(label, style: const TextStyle(color: C.label, fontSize: 14.5)),
          ),
        ),
        field,
      ],
    ),
  );
}

class _NameBox extends StatefulWidget {
  final String value;
  final ValueChanged<String> onChanged;
  const _NameBox(this.value, this.onChanged);
  @override
  State<_NameBox> createState() => _NameBoxState();
}

class _NameBoxState extends State<_NameBox> {
  late final _c = TextEditingController(text: widget.value);
  @override
  void didUpdateWidget(_NameBox old) {
    super.didUpdateWidget(old);
    if (old.value != widget.value) _c.text = widget.value;
  }

  @override
  void dispose() {
    _c.dispose();
    super.dispose();
  }

  void _done() {
    final n = _c.text.trim();
    if (n.isNotEmpty && n != widget.value) widget.onChanged(n);
  }

  @override
  Widget build(BuildContext context) => TextField(
    controller: _c,
    style: const TextStyle(fontSize: 19, fontWeight: FontWeight.w600),
    decoration: const InputDecoration(
      contentPadding: EdgeInsets.symmetric(horizontal: 4, vertical: 6),
      border: InputBorder.none,
      enabledBorder: InputBorder.none,
      fillColor: C.base,
    ),
    onSubmitted: (_) => _done(),
    onTapOutside: (_) => _done(),
  );
}
