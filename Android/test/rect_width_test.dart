// Rectangle W typed larger keeps the centre, and is limited so the corners
// stay inside ±10000 mm (padlink checkShape refuses "bad position").
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/ui/panels.dart';

void main() {
  testWidgets('rect W keeps corner inside ±10000', (t) async {
    var s = const Shape(id: 's1', type: Shape.rect, x: -9005, y: 0, w: 10, h: 10);
    final d = Design(name: 'J', shapes: [s]);
    await t.pumpWidget(MaterialApp(home: Scaffold(body: SizedBox(width: 400, height: 600,
        child: PropertiesPanel(design: d, selected: const {'s1'}, onChange: (x) => s = x)))));
    final box = find.byType(TextField).at(2);
    await t.tap(box);
    await t.pump();
    await t.enterText(box, '10000');
    await t.testTextInput.receiveAction(TextInputAction.done);
    await t.pumpAndSettle();
    expect(s.x >= -10000, isTrue);
  });
}
