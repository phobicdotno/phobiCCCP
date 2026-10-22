// The text box limit counts UTF-16 units as the computer does (padlink
// checkShape refuses text over 500 by QString::size), not characters.
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/ui/panels.dart';

void main() {
  testWidgets('emoji text stays within 500 UTF-16 units', (t) async {
    var s = const Shape(id: 's1', type: Shape.text, x: 10, y: 10, label: 'Hi', size: 10);
    final d = Design(name: 'J', shapes: [s]);
    await t.pumpWidget(MaterialApp(home: Scaffold(body: SizedBox(width: 400, height: 600,
        child: PropertiesPanel(design: d, selected: const {'s1'}, onChange: (x) => s = x)))));
    final box = find.byType(TextField).at(2);
    await t.tap(box);
    await t.pump();
    await t.enterText(box, 'Ø' * 10 + '\u{1F600}' * 400);
    await t.testTextInput.receiveAction(TextInputAction.done);
    await t.pumpAndSettle();
    expect(s.label!.length <= Shape.maxText, isTrue);
  });
}
