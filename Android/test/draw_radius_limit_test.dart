// Drawing a circle or hexagon wider than 10000 mm: the computer refuses a
// radius over 10000 even with every coordinate within range, so it is capped.
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/ui/canvas.dart';

void main() {
  for (final tool in [DrawTool.circle, DrawTool.polygon]) {
    testWidgets('draw corner to corner on 10 m stock: $tool', (t) async {
      final d = Design(name: 'J', stock: const Stock(width: 10000, height: 2000, thickness: 19));
      final zoom = CanvasZoom();
      final drawn = <Draft>[];
      await t.pumpWidget(MaterialApp(home: Scaffold(body: SizedBox(width: 700, height: 500,
          child: DesignCanvas(design: d, tool: tool, zoom: zoom,
              selected: const {}, onSelect: (i) {},
              onMove: (ids, dx, dy) {}, onMoveDone: () {}, onDraw: drawn.add, onTapAt: (_, _) {})))));
      final v = StockView(d.stock, const Size(700, 500));
      final a = v.toScreen(-500, -2500), b = v.toScreen(10800, 2800);
      final g = await t.startGesture(a);
      for (var i = 1; i <= 40; i++) { await g.moveTo(Offset.lerp(a, b, i / 40)!); await t.pump(); }
      await g.up();
      await t.pump();
      final s = shapeFromDraft(drawn.single, 's1')!;
      expect(s.r! <= 10000, isTrue);
    });
  }
}

