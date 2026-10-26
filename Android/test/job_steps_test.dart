// The computer's running job reaches the pad's Machine page as the same list:
// cut preview, bit, name, time and a 2 px bar per toolpath.
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/ui/icons.dart';
import 'package:phobicccp/ui/machine_page.dart';
import 'package:phobicccp/ui/theme.dart';

// A 1 x 1 transparent PNG.
const _png = 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNkYAAAAAYAAjCB0C8AAAAASUVORK5CYII=';

void main() {
  test('status carries the job steps', () {
    final s = MachineStatus.fromJson({
      'connected': true,
      'streaming': true,
      'steps': [
        {'name': 'Tool change', 'kind': 'change', 'bit': 'endmill', 'state': 'done', 'time': '✓ 0:00'},
        {'name': 'Pocket', 'detail': 'Pocket · Ø3.2 mm', 'kind': 'toolpath', 'bit': 'endmill', 'state': 'run', 'done': 0.4, 'preview': _png},
        {'name': 'Outline', 'kind': 'toolpath', 'bit': 'vbit', 'state': 'ahead', 'done': 7},
      ],
    });
    expect(s.steps.length, 3);
    expect(s.steps[0].isChange, isTrue);
    expect(s.steps[1].done, closeTo(0.4, 1e-9));
    expect(s.steps[2].done, 1.0, reason: 'clamped');
    expect(MachineStatus.fromJson({'connected': true}).steps, isEmpty, reason: 'an older phobiCCCP sends none');
  });

  test('a finished job says so over the list', () {
    expect(MachineStatus.fromJson({'streaming': true}).jobHeading('cnc'), 'RUNNING ON CNC');
    expect(MachineStatus.fromJson({'jobEnd': 'finished'}).jobHeading('cnc'), 'FINISHED ON CNC');
    expect(MachineStatus.fromJson({'jobEnd': 'stopped'}).jobHeading('cnc'), 'STOPPED ON CNC');
    expect(MachineStatus.fromJson({}).jobHeading('cnc'), 'LAST JOB ON CNC', reason: 'an older phobiCCCP');
  });

  testWidgets('rows show preview, bit and a bar per toolpath', (t) async {
    final steps = [
      const JobStep(name: 'Tool change', kind: 'change', bit: 'endmill', state: 'done', time: '✓ 0:00'),
      const JobStep(name: 'Pocket', detail: 'Pocket · Ø3.2 mm', bit: 'endmill', state: 'run', done: 0.5, time: '1:00 left', preview: _png),
      const JobStep(name: 'Outline', bit: 'vbit', time: '1:07'),
    ];
    await t.pumpWidget(MaterialApp(home: Scaffold(body: SizedBox(width: 600, height: 400, child: JobSteps(steps)))));
    expect(find.text('Pocket  —  Pocket · Ø3.2 mm'), findsOneWidget);
    expect(find.text('1:00 left'), findsOneWidget);
    expect(find.byType(ToolGlyph), findsNWidgets(3));
    expect(find.byType(Image), findsOneWidget, reason: 'only the toolpath with a preview draws one');
    // Two toolpaths, two bars; the running one is half full.
    final bars = find.byWidgetPredicate((w) => w is SizedBox && w.height == 2);
    expect(bars, findsNWidgets(2));
    final fill = t.getSize(find.descendant(of: bars.first, matching: find.byType(Container)).last);
    final track = t.getSize(bars.first);
    expect(fill.width, closeTo(track.width / 2, 1));
  });

  testWidgets('a stopped job shows the step it ended in as stopped, not running', (t) async {
    final steps = [
      const JobStep(name: 'Pocket', bit: 'endmill', state: 'done', done: 1, time: '✓ 1:00'),
      const JobStep(name: 'Outline', bit: 'vbit', state: 'stopped', done: 0.3, time: 'stopped'),
      const JobStep(name: 'Drill', bit: 'drill', time: '0:20'),
    ];
    await t.pumpWidget(MaterialApp(home: Scaffold(body: SizedBox(width: 600, height: 400, child: JobSteps(steps)))));
    final text = t.widget<Text>(find.text('Outline'));
    expect(text.style?.color, const Color(0xFFE06C6C));
    expect(text.style?.fontWeight, isNot(FontWeight.w600), reason: 'bold is for the running step');
    final bars = find.byWidgetPredicate((w) => w is SizedBox && w.height == 2);
    final fill = t.widget<Container>(find.descendant(of: bars.at(1), matching: find.byType(Container)).last);
    expect(fill.color, C.disabled, reason: 'its bar is not the running amber');
  });
}
