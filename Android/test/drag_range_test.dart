// A shape dragged on the canvas stays within ±10000 mm (padlink checkShape
// refuses anything past it): on a 10000 mm stock, a rectangle near the right
// edge dragged past it stops at the limit.
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/ui/canvas.dart';

void main() {
  testWidgets('drag past the edge of a 10 m stock', (t) async {
    var d = Design(name: 'J', stock: const Stock(width: 10000, height: 2000, thickness: 19),
        shapes: const [Shape(id: 's1', type: Shape.rect, x: 9500, y: 800, w: 450, h: 450)]);
    final zoom = CanvasZoom();
    late StateSetter set;
    await t.pumpWidget(MaterialApp(home: Scaffold(body: SizedBox(width: 700, height: 500,
        child: StatefulBuilder(builder: (c, s) { set = s; return DesignCanvas(design: d, tool: DrawTool.select, zoom: zoom,
            selected: const {}, onSelect: (i) {},
            onMove: (ids, dx, dy) => set(() => d = d.copyWith(shapes: [for (final x in d.shapes) ids.contains(x.id) ? x.moved(dx, dy) : x])),
            onMoveDone: () {}, onDraw: (_) {}, onTapAt: (_, _) {}); })))));
    final v = StockView(d.stock, const Size(700, 500));
    final at = v.toScreen(9520, 1000);
    final g = await t.startGesture(at);
    for (var i = 0; i < 30; i++) { await g.moveBy(const Offset(1, 0)); await t.pump(); }
    for (var i = 0; i < 6; i++) { await g.moveBy(const Offset(5, 0)); await t.pump(); }
    await g.up();
    await t.pump();
    final s = d.shapes.first;
    expect(s.x <= 10000, isTrue);
  });
}
