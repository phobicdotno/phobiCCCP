// Path Center typed past the range keeps every point at most 10000 (it left
// one at 10000.000000000002 by rounding; padlink checkShape needs d <= 10000).
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/ui/panels.dart';

void main() {
  testWidgets('path centre clamp keeps points within 10000 exactly', (t) async {
    var s = const Shape(id: 's1', type: Shape.path, points: [[-60.741, 0], [22.555, 0], [22.555, 50]], closed: true);
    final d = Design(name: 'J', shapes: [s]);
    await t.pumpWidget(MaterialApp(home: Scaffold(body: SizedBox(width: 400, height: 600,
        child: PropertiesPanel(design: d, selected: const {'s1'}, onChange: (x) => s = x)))));
    final box = find.byType(TextField).first;
    await t.tap(box);
    await t.pump();
    await t.enterText(box, '15000');
    await t.testTextInput.receiveAction(TextInputAction.done);
    await t.pumpAndSettle();
    expect(s.points!.every((p) => p[0] <= 10000 && p[0] >= -10000), isTrue);
  });
}
