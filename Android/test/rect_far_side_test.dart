// A rectangle from the computer centred past 10000 (x 9990, w 30): typing a
// smaller W threw (the field's max fell under its min) and changed nothing.
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/ui/panels.dart';

void main() {
  testWidgets('rect W typed on a rect centred past 10000', (t) async {
    var s = const Shape(id: 's1', type: Shape.rect, x: 9990, y: 50, w: 30, h: 100);
    final d = Design(name: 'J', stock: const Stock(width: 10000, height: 2000), shapes: [s]);
    await t.pumpWidget(MaterialApp(home: Scaffold(body: SizedBox(width: 400, height: 600,
        child: PropertiesPanel(design: d, selected: const {'s1'}, onChange: (x) => s = x)))));
    final box = find.byType(TextField).at(2);
    await t.tap(box);
    await t.pump();
    await t.enterText(box, '20');
    await t.testTextInput.receiveAction(TextInputAction.done);
    await t.pumpAndSettle();
    final e = t.takeException();
    expect(e, isNull);
    expect(s.w, 20);
  });
}
