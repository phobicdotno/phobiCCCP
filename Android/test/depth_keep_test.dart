// Tapping a toolpath's End depth field and leaving it must not change 3.175 mm,
// and the field shows the 3 decimals depths are stored with.
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/ui/widgets.dart';

void main() {
  testWidgets('focus then unfocus End = 3.175 keeps 3.175', (t) async {
    final got = <double>[];
    await t.pumpWidget(MaterialApp(home: Scaffold(body: Column(children: [
      StepperField(3.175, got.add, min: 0.01, max: 1000, decimals: 3),
      const TextField(key: Key('other')),
    ]))));
    expect(t.widget<TextField>(find.byType(TextField).first).controller!.text, '3.175 mm');
    await t.tap(find.byType(TextField).first);
    await t.pump();
    await t.tap(find.byKey(const Key('other')));
    await t.pump();
    expect(got, isEmpty, reason: 'depth changed to $got');
  });
  testWidgets('a 2-decimal field left untouched keeps its value', (t) async {
    final got = <double>[];
    await t.pumpWidget(MaterialApp(home: Scaffold(body: Column(children: [
      StepperField(3.175, got.add, min: 0.01, max: 1000),
      const TextField(key: Key('other')),
    ]))));
    await t.tap(find.byType(TextField).first);
    await t.pump();
    await t.tap(find.byKey(const Key('other')));
    await t.pump();
    expect(got, isEmpty, reason: 'value changed to $got');
  });
}
