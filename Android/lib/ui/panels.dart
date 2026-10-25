// The right-hand panels, as the desktop has them: Properties over the
// Document tab in Design, the Toolpaths list and form in Toolpaths
// (propertiespanel.cpp, toolpathpanel.cpp, toolpathform.cpp).

import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';

import '../model.dart';
import 'canvas.dart';
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
                // The corner has to stay inside ±10000 mm too (padlink check()).
                NumBox(s.x + w / 2, (v) => onChange(s.copyWith(x: v - w / 2)), min: -10000 + w / 2, max: 10000 - w / 2),
                NumBox(s.y + h / 2, (v) => onChange(s.copyWith(y: v - h / 2)), min: -10000 + h / 2, max: 10000 - h / 2),
              ),
            ),
          );
          rows.add(
            FormRow(
              'W × H',
              pair(
                // Grown about its centre: both sides stay inside ±10000 mm,
                // and no side over 10000 (the computer refuses more). One
                // from the computer already past the edge can still shrink,
                // its corner kept within ±10000 (the computer checks that).
                NumBox(w, (v) => onChange(s.copyWith(x: (s.x + (w - v) / 2).clamp(-10000.0, 10000.0), w: v)), min: 0.01,
                    max: math.max(w, math.min(10000, 2 * math.min(10000 - (s.x + w / 2), (s.x + w / 2) + 10000)))),
                NumBox(h, (v) => onChange(s.copyWith(y: (s.y + (h - v) / 2).clamp(-10000.0, 10000.0), h: v)), min: 0.01,
                    max: math.max(h, math.min(10000, 2 * math.min(10000 - (s.y + h / 2), (s.y + h / 2) + 10000)))),
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
              // Every point has to stay inside ±10000 mm (padlink checkShape),
              // not just the centre: the computer refuses the whole design.
              pair(
                NumBox(cx, (v) => onChange(s.movedWithin(v - cx, 0)), min: -10000 + (cx - b[0]), max: 10000 - (b[2] - cx)),
                NumBox(cy, (v) => onChange(s.movedWithin(0, v - cy)), min: -10000 + (cy - b[1]), max: 10000 - (b[3] - cy)),
              ),
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
      inputFormatters: [Utf16LengthLimit(Shape.maxText)],
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

/// How many of [t]'s vectors are open paths it cannot cut: a pocket and an
/// inside or outside contour need an area, so phobiCCCP leaves open paths
/// out of them (only a contour on the line follows one).
int openPathsLeftOut(Design d, ToolpathSpec t) {
  if (!(t.type == 'pocket' || (t.type == 'profile' && t.side != 'on'))) return 0;
  return t.shapes.where((id) {
    final s = d.shape(id);
    return s != null && s.type == Shape.path && s.closed != true;
  }).length;
}

/// How many of [t]'s vectors are points: only a drill cuts a point, and
/// phobiCCCP leaves them out of every other toolpath ("no vectors").
int pointsLeftOut(Design d, ToolpathSpec t) {
  if (t.type == 'drill') return 0;
  return t.shapes.where((id) => d.shape(id)?.type == Shape.point).length;
}

String pointsWords(int n) => '$n point${n == 1 ? '' : 's'} left out: only a drill cuts points';

String openPathsWords(int n) =>
    '$n open path${n == 1 ? '' : 's'} left out: only a contour on the line cuts open paths';

String tpTypeLabel(String t) => const {'profile': 'Contour', 'pocket': 'Pocket', 'drill': 'Drill', 'engrave': 'Engrave'}[t] ?? t;

class ToolpathsPanel extends StatelessWidget {
  final Design design;
  final ToolLibrary tools;
  final int? current;
  final Set<String> selectedShapes;
  final ValueChanged<int?> onPick;

  /// Cards whose "v" is open: a thumbnail per shape, each with a bin.
  final Set<int> shapesOpen;
  final ValueChanged<int> onToggleShapes;
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
    this.shapesOpen = const {},
    required this.onToggleShapes,
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
          // Dragging anywhere scrolls, with a mouse too; a card comes loose to
          // be moved only after a still hold of kHoldToMove (see _Hold).
          child: ScrollConfiguration(
            behavior: ScrollConfiguration.of(context).copyWith(dragDevices: PointerDeviceKind.values.toSet()),
            child: _HoldArea(child: CustomScrollView(
              slivers: [
                SliverPadding(
                  padding: const EdgeInsets.symmetric(horizontal: 6),
                  sliver: SliverReorderableList(
                    itemCount: tps.length,
                    itemBuilder: (context, k) => _Hold(key: ValueKey(k), index: k, child: _card(k, tps[k], k == i)),
                    proxyDecorator: (child, _, _) => Material(type: MaterialType.transparency, child: child),
                    onReorderItem: (from, to) {
                      if (to != from) onMove(from, to);
                    },
                  ),
                ),
                SliverPadding(
                  padding: const EdgeInsets.fromLTRB(6, 0, 6, 10),
                  sliver: SliverList.list(
                    children: [
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
            )),
          ),
        ),
      ],
    );
  }

  Widget _card(int k, ToolpathSpec t, bool sel) {
    final tool = tools.tool(t.tool);
    final open = shapesOpen.contains(k);
    return GestureDetector(
      onTap: () => onPick(sel ? null : k),
      child: Container(
        margin: const EdgeInsets.symmetric(vertical: 3),
        decoration: BoxDecoration(
          color: sel ? C.selCard : C.card,
          borderRadius: BorderRadius.circular(8),
          border: Border.all(color: sel ? C.amber : C.cardBorder, width: sel ? 2 : 1),
        ),
        padding: const EdgeInsets.symmetric(horizontal: 10),
        child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        SizedBox(height: 50, child: Row(
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
        )),
        // "v": open or close a thumbnail of every shape this toolpath cuts.
        GestureDetector(
          behavior: HitTestBehavior.opaque,
          onTap: () => onToggleShapes(k),
          child: SizedBox(
            height: 18,
            child: Icon(open ? Icons.expand_less_rounded : Icons.expand_more_rounded, size: 18, color: C.sub),
          ),
        ),
        if (open)
          Padding(
            padding: const EdgeInsets.only(bottom: 8),
            child: Wrap(spacing: 6, runSpacing: 6, children: [
              for (var n = 0; n < t.shapes.length; n++) _thumb(k, t, n),
              _plus(k, t),
            ]),
          ),
        ]),
      ),
    );
  }

  /// After the last shape: a dotted "+" the size of a shape tile that adds
  /// the shapes selected on the drawing (the ones not already in it).
  Widget _plus(int k, ToolpathSpec t) => GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTap: () {
          final add = [for (final id in selectedShapes) if (design.shape(id) != null && !t.shapes.contains(id)) id];
          if (add.isNotEmpty) onChange(k, t.copyWith(shapes: [...t.shapes, ...add]));
        },
        child: const SizedBox(
          width: 64,
          height: 64,
          child: CustomPaint(painter: _DottedTile(), child: Icon(Icons.add_rounded, size: 26, color: C.nameOn)),
        ),
      );

  /// One shape of toolpath [k] as a small picture, with a bin that takes it
  /// out of the toolpath (the shape stays in the drawing).
  Widget _thumb(int k, ToolpathSpec t, int n) => SizedBox(
        width: 64,
        height: 64,
        child: Stack(children: [
          Positioned.fill(
            child: Container(
              decoration: BoxDecoration(
                color: C.base,
                borderRadius: BorderRadius.circular(6),
                border: Border.all(color: const Color(0xFF3A404B)),
              ),
              child: CustomPaint(painter: ShapeThumbPainter(design, k, t.shapes[n])),
            ),
          ),
          Positioned(
            top: 2,
            right: 2,
            child: GestureDetector(
              behavior: HitTestBehavior.opaque,
              onTap: () => onChange(k, t.copyWith(shapes: [...t.shapes]..removeAt(n))),
              child: Container(
                width: 22,
                height: 22,
                decoration: BoxDecoration(color: C.button.withAlpha(220), borderRadius: BorderRadius.circular(4)),
                child: const Icon(Icons.delete_outline_rounded, size: 16, color: C.nameOn),
              ),
            ),
          ),
        ]),
      );

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
        // At the top, as on the computer: the first thing done to a new toolpath.
        PCard(
          title: 'Vectors',
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              Padding(
                padding: const EdgeInsets.symmetric(horizontal: 4, vertical: 4),
                child: Text('${t.shapes.length} assigned · ${inSel.length} selected', style: const TextStyle(color: C.label)),
              ),
              if (openPathsLeftOut(design, t) > 0)
                Padding(
                  padding: const EdgeInsets.symmetric(horizontal: 4, vertical: 4),
                  child: Text(openPathsWords(openPathsLeftOut(design, t)), style: const TextStyle(color: C.amber)),
                ),
              if (pointsLeftOut(design, t) > 0)
                Padding(
                  padding: const EdgeInsets.symmetric(horizontal: 4, vertical: 4),
                  child: Text(pointsWords(pointsLeftOut(design, t)), style: const TextStyle(color: C.amber)),
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
          child: Column(children: [_row('End', StepperField(t.depth, (v) => onChange(i, t.copyWith(depth: v)), min: 0.01, max: 1000, decimals: 3))]),
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
        if (t.type == 'profile')
          PCard(
            title: 'Tabs',
            child: Column(children: [
              _row('Tabs', Switch(value: t.tabs, onChanged: (v) => onChange(i, t.copyWith(tabs: v)))),
              if (t.tabs) ...[
                _row('Height', StepperField(t.tabHeight ?? 3, (v) => onChange(i, t.copyWith(tabHeight: v)), min: 0.1, max: 50, decimals: 2)),
                _row('Width', StepperField(t.tabWidth ?? 12, (v) => onChange(i, t.copyWith(tabWidth: v)), min: 1, max: 30, decimals: 1)),
              ],
            ]),
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

/// How long a toolpath card must be held still before it can be dragged to a
/// new place. Every move starts the wait again, as on the desktop
/// (toolpathpanel.cpp CardList): time spent scrolling never counts towards it,
/// but a scroll that stops with the finger still down picks up the card under
/// the finger once it has been still for this long.
const kHoldToMove = Duration(milliseconds: 500);

/// One card. Held still from the touch on, its own recognizer takes the touch
/// from the list and the card comes loose; once the touch moves, the list
/// scrolls and _HoldArea follows it from there.
class _Hold extends StatefulWidget {
  final int index;
  final Widget child;
  const _Hold({super.key, required this.index, required this.child});
  @override
  State<_Hold> createState() => _HoldState();
}

class _HoldState extends State<_Hold> {
  _HoldAreaState? _area;

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    _area?._cards.remove(this);
    _area = context.findAncestorStateOfType<_HoldAreaState>()?.._cards.add(this);
  }

  @override
  void dispose() {
    _area?._cards.remove(this);
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => Listener(
        onPointerDown: (e) {
          final list = SliverReorderableList.maybeOf(context);
          if (list == null) return;
          _area?._down(e);
          list.startItemDragReorder(index: widget.index, event: e, recognizer: _StillHold(_area, debugOwner: this));
        },
        child: widget.child,
      );
}

/// Takes the touch after it has been still for kHoldToMove from the start;
/// when it moves first (or the list takes it to scroll), _HoldArea is told to
/// follow it instead.
class _StillHold extends DelayedMultiDragGestureRecognizer {
  final _HoldAreaState? area;
  _StillHold(this.area, {super.debugOwner}) : super(delay: kHoldToMove);

  @override
  void acceptGesture(int pointer) {
    area?._forget(pointer);
    super.acceptGesture(pointer);
  }

  @override
  void rejectGesture(int pointer) {
    area?._follow(pointer);
    super.rejectGesture(pointer);
  }
}

/// Around the toolpath list: follows a touch that began on a card while it
/// scrolls the list, starting the wait again at every move, and when the
/// finger has then been still for kHoldToMove, stops the scroll and starts
/// moving the card under it, as the desktop does.
class _HoldArea extends StatefulWidget {
  final Widget child;
  const _HoldArea({required this.child});
  @override
  State<_HoldArea> createState() => _HoldAreaState();
}

class _HoldAreaState extends State<_HoldArea> {
  // A drag started here has no touch of its own in the gesture arena (the
  // list's scroll won that one), so it gets a made-up pointer, and the real
  // touch's moves are passed on to it under that number.
  static var _nextPointer = 1 << 30;

  final _cards = <_HoldState>{};
  int? _pointer, _carried;
  bool _following = false;
  PointerEvent? _last;
  Timer? _timer;

  @override
  void dispose() {
    _timer?.cancel();
    super.dispose();
  }

  void _down(PointerDownEvent e) {
    if (_pointer != null) return;
    _pointer = e.pointer;
    _last = e;
    _following = false;
    _carried = null;
  }

  void _forget(int pointer) {
    if (pointer != _pointer) return;
    _timer?.cancel();
    _pointer = null;
    _following = false;
  }

  void _follow(int pointer) {
    // Also told after the touch is lifted (a tap): then there is nothing to follow.
    if (pointer != _pointer || _carried != null) return;
    _following = true;
    _restart();
  }

  void _restart() {
    _timer?.cancel();
    _timer = Timer(kHoldToMove, _pickUp);
  }

  void _move(PointerMoveEvent e) {
    if (e.pointer != _pointer) return;
    _last = e;
    final carried = _carried;
    if (carried != null) {
      GestureBinding.instance.pointerRouter.route(e.copyWith(pointer: carried));
    } else if (_following) {
      _restart();
    }
  }

  void _end(PointerEvent e) {
    if (e.pointer != _pointer) return;
    _timer?.cancel();
    final carried = _carried;
    if (carried != null) {
      GestureBinding.instance.pointerRouter.route(
          e is PointerUpEvent ? e.copyWith(pointer: carried) : (e as PointerCancelEvent).copyWith(pointer: carried));
    }
    _pointer = null;
    _carried = null;
    _following = false;
  }

  void _pickUp() {
    final e = _last;
    if (!mounted || e == null || _pointer == null || _carried != null) return;
    for (final card in _cards) {
      final box = card.context.findRenderObject();
      if (box is! RenderBox || !box.attached || !(Offset.zero & box.size).contains(box.globalToLocal(e.position))) continue;
      final list = SliverReorderableList.maybeOf(card.context);
      if (list == null) return;
      // The scroll this touch was driving stops here, so the card moves, not the list.
      final position = Scrollable.maybeOf(card.context)?.position;
      if (position != null && position.hasPixels) position.jumpTo(position.pixels);
      final pointer = _nextPointer++;
      _carried = pointer;
      list.startItemDragReorder(
          index: card.widget.index,
          event: PointerDownEvent(pointer: pointer, position: e.position, kind: e.kind, device: e.device, buttons: kPrimaryButton),
          recognizer: ImmediateMultiDragGestureRecognizer(debugOwner: this));
      // Alone in its arena, it wins it straight away.
      GestureBinding.instance.gestureArena.close(pointer);
      return;
    }
  }

  @override
  Widget build(BuildContext context) =>
      Listener(onPointerMove: _move, onPointerUp: _end, onPointerCancel: _end, child: widget.child);
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

/// One shape of toolpath [index], fitted to the thumbnail. Where the pockets
/// above already cleared part of it, only what is left is amber over a faint
/// outline, as on the canvas when the toolpath is selected.
class ShapeThumbPainter extends CustomPainter {
  final Design design;
  final int index;
  final String id;
  ShapeThumbPainter(this.design, this.index, this.id);

  @override
  void paint(Canvas canvas, Size size) {
    final s = design.shape(id);
    if (s == null) {
      final tp = TextPainter(
          text: const TextSpan(text: '?', style: TextStyle(color: Color(0xFFC0392B), fontSize: 18)),
          textDirection: TextDirection.ltr)
        ..layout();
      tp.paint(canvas, Offset((size.width - tp.width) / 2, (size.height - tp.height) / 2));
      return;
    }
    final b = s.bounds();
    final v = StockView(
        Stock(width: math.max(b[2] - b[0], 0.01), height: math.max(b[3] - b[1], 0.01)), size, margin: 9);
    Offset map(double x, double y) => v.toScreen(x - b[0], y - b[1]);
    final local = s.moved(-b[0], -b[1]);
    final area = shapeArea(s, map);
    final cover = area == null ? null : clearedAbove(design, index, map);
    if (area != null && cover != null) {
      drawShapeOutline(
          canvas,
          v,
          local,
          Paint()
            ..color = C.disabled
            ..style = PaintingStyle.stroke
            ..strokeWidth = 1);
      final left = Path.combine(PathOperation.difference, area, cover);
      canvas.drawPath(left, Paint()..color = C.amber.withAlpha(0x50));
      canvas.drawPath(
          left,
          Paint()
            ..color = C.amber
            ..style = PaintingStyle.stroke
            ..strokeWidth = 1.4);
      return;
    }
    if (area != null) drawShapeOutline(canvas, v, local, Paint()..color = C.amber.withAlpha(0x40));
    drawShapeOutline(
        canvas,
        v,
        local,
        Paint()
          ..color = C.nameOn
          ..style = PaintingStyle.stroke
          ..strokeWidth = 1.4);
  }

  @override
  bool shouldRepaint(ShapeThumbPainter old) => old.design != design || old.index != index || old.id != id;
}

/// The dotted outline of the "+" tile.
class _DottedTile extends CustomPainter {
  const _DottedTile();

  @override
  void paint(Canvas canvas, Size size) {
    final outline = Path()..addRRect(RRect.fromRectAndRadius((Offset.zero & size).deflate(1), const Radius.circular(6)));
    final pen = Paint()
      ..color = C.sub
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.4;
    for (final m in outline.computeMetrics()) {
      for (var d = 0.0; d < m.length; d += 7) {
        canvas.drawPath(m.extractPath(d, math.min(d + 4, m.length)), pen);
      }
    }
  }

  @override
  bool shouldRepaint(_DottedTile old) => false;
}
