// After a two-finger pinch, the finger left on the screen only pans: it
// picked up and dragged the shape under it (and the Rect tool drew).
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/ui/canvas.dart';

void main() {
  for (final tool in [DrawTool.select, DrawTool.rect]) {
    testWidgets('pinch then one finger: $tool', (t) async {
      var d = Design(name: 'J', shapes: const [Shape(id: 's1', type: Shape.rect, x: 100, y: 50, w: 100, h: 100)]);
      final zoom = CanvasZoom();
      late StateSetter set;
      final drawn = <Draft>[];
      var selected = <String>{};
      await t.pumpWidget(MaterialApp(home: Scaffold(body: SizedBox(width: 700, height: 500,
          child: StatefulBuilder(builder: (c, s) { set = s; return DesignCanvas(design: d, tool: tool, zoom: zoom,
              selected: selected, onSelect: (i) => set(() => selected = i),
              onMove: (ids, dx, dy) => set(() => d = d.copyWith(shapes: [for (final x in d.shapes) ids.contains(x.id) ? x.moved(dx, dy) : x])),
              onMoveDone: () {}, onDraw: drawn.add, onTapAt: (_, _) {}); })))));
      final v = StockView(d.stock, const Size(700, 500));
      final c = v.toScreen(150, 100);
      final a = await t.startGesture(c - const Offset(60, 0), pointer: 1);
      final b = await t.startGesture(c + const Offset(60, 0), pointer: 2);
      for (var i = 0; i < 10; i++) {
        await a.moveBy(const Offset(-4, 0));
        await b.moveBy(const Offset(4, 0));
        await t.pump();
      }
      await b.up();
      await t.pump();
      for (var i = 0; i < 20; i++) { await a.moveBy(const Offset(0, 4)); await t.pump(); }
      await a.up();
      await t.pump();
      expect(d.shapes.first.x == 100 && d.shapes.first.y == 50 && drawn.isEmpty, isTrue);
    });
  }
}
