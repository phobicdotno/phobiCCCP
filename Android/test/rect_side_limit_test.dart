// W typed past 10000 on a rectangle near the origin: the field stops at
// 10000 (the computer refuses a wider side), as before round 33.
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/ui/panels.dart';

void main() {
  testWidgets('rect W typed 12000 on a 300x200 stock rect', (t) async {
    var s = const Shape(id: 's1', type: Shape.rect, x: 100, y: 50, w: 100, h: 100);
    final d = Design(name: 'J', shapes: [s]);
    await t.pumpWidget(MaterialApp(home: Scaffold(body: SizedBox(width: 400, height: 600,
        child: PropertiesPanel(design: d, selected: const {'s1'}, onChange: (x) => s = x)))));
    final box = find.byType(TextField).at(2);
    await t.tap(box);
    await t.pump();
    await t.enterText(box, '12000');
    await t.testTextInput.receiveAction(TextInputAction.done);
    await t.pumpAndSettle();
    expect(s.w! <= 10000, isTrue);
  });
}
