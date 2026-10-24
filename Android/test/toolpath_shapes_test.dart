// The "v" under a toolpath card: a thumbnail per shape with a bin, and
// stacked pockets showing only what is left for the lower one
// (toolpathpanel.cpp on the desktop).

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/ui/canvas.dart';
import 'package:phobicccp/ui/panels.dart';
import 'package:phobicccp/ui/theme.dart';

void main() {
  Offset map(double x, double y) => Offset(x, -y);
  const shapes = [
    Shape(id: 'big', type: Shape.rect, x: 0, y: 0, w: 100, h: 100),
    Shape(id: 'small', type: Shape.rect, x: 20, y: 20, w: 20, h: 20),
    Shape(id: 'far', type: Shape.circle, x: 300, y: 300, r: 10),
  ];
  ToolpathSpec tp(String type, List<String> ids, double depth, {bool on = true}) =>
      ToolpathSpec(name: type, type: type, shapes: ids, tool: 201, depth: depth, enabled: on);

  test('a pocket over an earlier, as deep pocket has only what is left to cut', () {
    final d = Design(shapes: shapes, toolpaths: [tp('pocket', ['small'], 5), tp('pocket', ['big'], 5)]);
    final cover = clearedAbove(d, 1, map);
    expect(cover, isNotNull);
    final left = Path.combine(PathOperation.difference, toolpathArea(d, d.toolpaths[1], map), cover!);
    expect(left.contains(map(30, 30)), isFalse);   // the small pocket cut it
    expect(left.contains(map(70, 70)), isTrue);
    expect(clearedAbove(d, 0, map), isNull);        // nothing above the first
  });

  test('a shallower, switched-off or unrelated toolpath above takes nothing off', () {
    expect(clearedAbove(Design(shapes: shapes, toolpaths: [tp('pocket', ['small'], 2), tp('pocket', ['big'], 5)]), 1, map), isNull);
    expect(clearedAbove(Design(shapes: shapes, toolpaths: [tp('pocket', ['small'], 5, on: false), tp('pocket', ['big'], 5)]), 1, map),
        isNull);
    expect(clearedAbove(Design(shapes: shapes, toolpaths: [tp('profile', ['small'], 5), tp('pocket', ['big'], 5)]), 1, map), isNull);
    expect(clearedAbove(Design(shapes: shapes, toolpaths: [tp('pocket', ['far'], 5), tp('pocket', ['big'], 5)]), 1, map), isNull);
    expect(clearedAbove(Design(shapes: shapes, toolpaths: [tp('pocket', ['small'], 5), tp('profile', ['big'], 5)]), 1, map), isNull);
  });

  testWidgets('v opens a thumbnail per shape and its bin takes that shape out', (t) async {
    var d = Design(shapes: shapes, toolpaths: [tp('pocket', ['big', 'small', 'far'], 3)]);
    final open = <int>{};
    await t.pumpWidget(MaterialApp(
      theme: phobiTheme(),
      home: Scaffold(
        body: StatefulBuilder(
          builder: (context, setState) => SizedBox(
            width: 420,
            child: ToolpathsPanel(
              design: d,
              tools: const ToolLibrary(),
              current: null,
              selectedShapes: const {},
              onPick: (_) {},
              shapesOpen: open,
              onToggleShapes: (i) => setState(() => open.contains(i) ? open.remove(i) : open.add(i)),
              onNew: (_) {},
              onChange: (i, s) => setState(() => d = d.copyWith(toolpaths: [s])),
              onDuplicate: (_) {},
              onDelete: (_) {},
              onMove: (_, _) {},
              onPickTool: (_) {},
            ),
          ),
        ),
      ),
    ));
    expect(find.byIcon(Icons.delete_outline_rounded), findsNothing);
    await t.tap(find.byIcon(Icons.expand_more_rounded));
    await t.pump();
    expect(find.byIcon(Icons.delete_outline_rounded), findsNWidgets(3));
    await t.tap(find.byIcon(Icons.delete_outline_rounded).at(1));
    await t.pump();
    expect(d.toolpaths[0].shapes, ['big', 'far']);
    expect(d.shapes.length, 3);   // the shape stays in the drawing
    expect(find.byIcon(Icons.delete_outline_rounded), findsNWidgets(2));
    await t.tap(find.byIcon(Icons.expand_less_rounded));
    await t.pump();
    expect(find.byIcon(Icons.delete_outline_rounded), findsNothing);
  });
}
