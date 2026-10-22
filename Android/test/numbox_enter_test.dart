// Typing a value and pressing Done (Enter) keeps it: focus loss after Done
// does not send the old value back.
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/ui/widgets.dart';
import 'package:phobicccp/ui/panels.dart';
import 'package:phobicccp/model.dart';

class Host extends StatefulWidget {
  final double v0; final int dec; final List<double> log;
  const Host(this.v0, this.dec, this.log, {super.key});
  @override State<Host> createState() => _H();
}
class _H extends State<Host> {
  late double v = widget.v0;
  @override Widget build(BuildContext c) => Column(children: [
    Text('v=$v', key: const Key('lbl')),
    NumBox(v, (x) { widget.log.add(x); setState(() => v = x); }, decimals: widget.dec),
    const TextField(key: Key('other')),
  ]);
}

void main() {
  panelMain();
  for (final (v0, dec) in [(12.3456, 3), (3.175, 2), (2.0, 3)]) {
    testWidgets('type 50 + Enter on $v0 (decimals $dec) keeps 50', (t) async {
      final log = <double>[];
      await t.pumpWidget(MaterialApp(home: Scaffold(body: Host(v0, dec, log))));
      await t.tap(find.byType(TextField).first);
      await t.pump();
      await t.enterText(find.byType(TextField).first, '50');
      await t.testTextInput.receiveAction(TextInputAction.done);
      await t.pumpAndSettle();
      final s = t.widget<Text>(find.byKey(const Key('lbl'))).data;
      expect(s, 'v=50.0', reason: 'onChanged calls: $log');
    });
  }
}

class PHost extends StatefulWidget {
  final Design d0;
  const PHost(this.d0, {super.key});
  @override State<PHost> createState() => _PH();
}
class _PH extends State<PHost> {
  late Design d = widget.d0;
  @override Widget build(BuildContext c) => SizedBox(width: 300, height: 400, child: PropertiesPanel(
      design: d, selected: const {'p'}, onChange: (s) => setState(() => d = d.copyWith(shapes: [for (final x in d.shapes) x.id == s.id ? s : x]))));
}

void panelMain() {
  testWidgets('path: type Center X 50 + Enter moves it there', (t) async {
    final k = GlobalKey<_PH>();
    final d = Design(id: 'a', name: 'A', modified: 1, shapes: [
      Shape(id: 'p', type: Shape.path, closed: true, points: [[10.1234, 5.0], [14.5678, 5.0], [14.5678, 9.0]]),
    ]);
    await t.pumpWidget(MaterialApp(home: Scaffold(body: PHost(d, key: k))));
    final x = find.byType(TextField).first;
    await t.tap(x);
    await t.pump();
    await t.enterText(x, '50');
    await t.testTextInput.receiveAction(TextInputAction.done);
    await t.pumpAndSettle();
    final b = k.currentState!.d.shapes.first.bounds();
    expect((b[0] + b[2]) / 2, closeTo(50, 1e-6), reason: 'centre x ${(b[0] + b[2]) / 2}');
  });
}
