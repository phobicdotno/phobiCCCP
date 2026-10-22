// Path Center field: a value past the range keeps every point of the path
// inside ±10000 mm (phobiCCCP refuses the whole design otherwise).
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/ui/panels.dart';

void main() {
  testWidgets('path centre typed past the range keeps its points in range', (t) async {
    var s = const Shape(id: 's1', type: Shape.path, points: [[0, 0], [300, 0], [300, 200]], closed: true);
    final d = Design(name: 'J', shapes: [s]);
    await t.pumpWidget(MaterialApp(home: Scaffold(body: SizedBox(width: 400, height: 600,
        child: PropertiesPanel(design: d, selected: const {'s1'}, onChange: (x) => s = x)))));
    final box = find.byType(TextField).first;
    await t.tap(box);
    await t.pump();
    await t.enterText(box, '15000');
    await t.testTextInput.receiveAction(TextInputAction.done);
    await t.pumpAndSettle();
    final xs = s.points!.map((p) => p[0]).toList();
    expect(xs.every((x) => x.abs() <= 10000) && xs.contains(10000.0), isTrue, reason: 'padlink check() refuses any path point past ±10000');
  });
}
