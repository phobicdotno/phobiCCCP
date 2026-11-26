// Round 3: a drawing tool does not follow onto the Toolpaths page; every pick
// of shapes lets go of the toolpath halo but keeps the toolpath current;
// depth-like boxes read "-3" as 3; a held job says so.
import 'dart:io';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/app_state.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/store.dart';
import 'package:phobicccp/ui/canvas.dart';
import 'package:phobicccp/ui/main_window.dart';
import 'package:phobicccp/ui/panels.dart';
import 'package:phobicccp/ui/theme.dart';
import 'package:phobicccp/ui/widgets.dart';

Future<(AppState, DesignStore)> _window(WidgetTester t) async {
  t.view.physicalSize = const Size(1280, 800);
  t.view.devicePixelRatio = 1;
  addTearDown(t.view.reset);
  final dir = Directory.systemTemp.createTempSync('r3');
  final store = DesignStore(Directory('${dir.path}/d'));
  final settings = Settings(File('${dir.path}/s.json'));
  final a = store.save(
      Design(name: 'Sign', shapes: const [
        Shape(id: 's1', type: Shape.rect, x: 0, y: 0, w: 10, h: 10),
        Shape(id: 's2', type: Shape.rect, x: 40, y: 40, w: 10, h: 10),
      ], toolpaths: const [
        ToolpathSpec(name: 'Profile 1', type: 'profile', shapes: ['s1', 's2'], tool: 201, depth: 3),
      ]),
      now: 1000);
  settings.lastDesign = a.id;
  final app = AppState(store, settings, deviceName: 'pad');
  await t.pumpWidget(MaterialApp(theme: phobiTheme(), home: MainWindow(app)));
  await t.pumpAndSettle();
  return (app, store);
}

DesignCanvas _canvas(WidgetTester t) => t.widget<DesignCanvas>(find.byType(DesignCanvas));
int? _current(WidgetTester t) => t.widget<ToolpathsPanel>(find.byType(ToolpathsPanel)).current;

Future<void> _close(WidgetTester t, AppState app) async {
  await t.pumpWidget(const SizedBox());
  app.dispose();
}

class _Host extends StatefulWidget {
  final double v0;
  final bool magnitude, stepper;
  const _Host(this.v0, {this.magnitude = false, this.stepper = false});
  @override
  State<_Host> createState() => _HostState();
}

class _HostState extends State<_Host> {
  late double v = widget.v0;
  @override
  Widget build(BuildContext c) => Column(children: [
        Text('v=$v', key: const Key('lbl')),
        widget.stepper
            ? StepperField(v, (x) => setState(() => v = x), min: 0.01, max: 1000, decimals: 3, magnitude: widget.magnitude)
            : NumBox(v, (x) => setState(() => v = x), min: 0.01, max: 1000, magnitude: widget.magnitude),
      ]);
}

Future<String?> _type(WidgetTester t, Widget host, String text) async {
  await t.pumpWidget(MaterialApp(home: Scaffold(body: host)));
  await t.tap(find.byType(TextField));
  await t.pump();
  await t.enterText(find.byType(TextField), text);
  await t.testTextInput.receiveAction(TextInputAction.done);
  await t.pumpAndSettle();
  return t.widget<Text>(find.byKey(const Key('lbl'))).data;
}

void main() {
  group('toolForMode', () {
    test('drawing tools fall back to Select off the Design page', () {
      for (final tool in [DrawTool.circle, DrawTool.rect, DrawTool.polygon, DrawTool.text, DrawTool.point, DrawTool.path]) {
        expect(toolForMode(1, tool), DrawTool.select, reason: '$tool');
        expect(toolForMode(2, tool), DrawTool.select, reason: '$tool');
      }
      for (final tool in [DrawTool.select, DrawTool.hand, DrawTool.area]) {
        expect(toolForMode(1, tool), tool);
        expect(toolForMode(0, tool), tool);
      }
      expect(toolForMode(0, DrawTool.circle), DrawTool.circle);
      expect(toolForMode(0, DrawTool.path), DrawTool.select, reason: 'an unfinished path is dropped');
    });
  });

  group('afterPick', () {
    test('shapes: halo off, toolpath kept', () => expect(afterPick({'s1'}, 2, true), (tp: 2, lit: false)));
    test('nothing: toolpath let go', () => expect(afterPick({}, 2, true), (tp: null, lit: true)));
  });

  testWidgets('a circle chosen in Design is Select on the Toolpaths page', (t) async {
    final (app, _) = await _window(t);
    await t.tap(find.byTooltip('Circle'));
    await t.pumpAndSettle();
    expect(_canvas(t).tool, DrawTool.circle);
    await t.tap(find.text('2 · Toolpaths'));
    await t.pumpAndSettle();
    expect(_canvas(t).tool, DrawTool.select);
    await _close(t, app);
  });

  testWidgets('picking shapes lets go of the halo, keeps the toolpath current', (t) async {
    final (app, _) = await _window(t);
    await t.tap(find.text('2 · Toolpaths'));
    await t.pumpAndSettle();
    await t.tap(find.text('Profile 1'));
    await t.pumpAndSettle();
    expect(_canvas(t).highlighted, {'s1', 's2'});
    expect(_current(t), 0);

    // On the board.
    _canvas(t).onSelect({'s1'});
    await t.pumpAndSettle();
    expect(_canvas(t).highlighted, isEmpty);
    expect(_current(t), 0);

    // With the Area tool.
    t.widget<ToolpathsPanel>(find.byType(ToolpathsPanel)).onPick(0);
    await t.pumpAndSettle();
    expect(_canvas(t).highlighted, {'s1', 's2'});
    await t.tap(find.byTooltip('Area: tap inside an area to select just it, like a fill, for a pocket'));
    await t.pumpAndSettle();
    _canvas(t).onTapAt(5, 5);
    await t.pumpAndSettle();
    expect(_canvas(t).selected, {'s1'});
    expect(_canvas(t).highlighted, isEmpty);
    expect(_current(t), 0);

    // Area on empty board: lets go of the toolpath, as a tap there does.
    _canvas(t).onTapAt(-500, -500);
    await t.pumpAndSettle();
    expect(_canvas(t).selected, isEmpty);
    expect(_current(t), isNull);
    await _close(t, app);
  });

  testWidgets('Duplicate lights the copy', (t) async {
    final (app, _) = await _window(t);
    await t.tap(find.text('2 · Toolpaths'));
    await t.pumpAndSettle();
    await t.tap(find.text('Profile 1'));
    await t.pumpAndSettle();
    _canvas(t).onSelect({'s1'});
    await t.pumpAndSettle();
    expect(_canvas(t).highlighted, isEmpty);
    await t.tap(find.text('Duplicate').last);
    await t.pumpAndSettle();
    expect(_current(t), 1);
    expect(_canvas(t).highlighted, {'s1', 's2'});
    await _close(t, app);
  });

  group('magnitude', () {
    testWidgets('NumBox: -3 is 3', (t) async {
      expect(await _type(t, const _Host(1, magnitude: true), '-3'), 'v=3.0');
    });
    testWidgets('NumBox without it still clamps -3 to the min', (t) async {
      expect(await _type(t, const _Host(1), '-3'), 'v=0.01');
    });
    testWidgets('StepperField: -2.5 mm is 2.5', (t) async {
      expect(await _type(t, const _Host(1, magnitude: true, stepper: true), '-2.5 mm'), 'v=2.5');
    });
  });

  group('MachineStatus held', () {
    const base = {'connected': true, 'homed': true, 'busy': true, 'streaming': true, 'state': 'Hold:0', 'job': 'Sign'};
    test('held says so', () {
      final s = MachineStatus.fromJson({...base, 'held': true});
      expect(s.held, isTrue);
      expect(s.words(), contains('Held'));
      expect(s.words(), contains('Resume'));
    });
    test('not held, or an older computer: cutting', () {
      expect(MachineStatus.fromJson({...base, 'held': false}).words(), 'Cutting Sign');
      final old = MachineStatus.fromJson(base);
      expect(old.held, isFalse);
      expect(old.words(), 'Cutting Sign');
    });
  });

  group('MachineStatus lifting and tool change', () {
    const base = {'connected': true, 'homed': true, 'busy': true, 'streaming': true, 'state': 'Run', 'job': 'Sign'};
    test('older computer: defaults, cutting', () {
      final s = MachineStatus.fromJson(base);
      expect(s.lifting, isFalse);
      expect(s.toolChange, -1);
      expect(s.words(), 'Cutting Sign');
    });
    test('lifting', () {
      expect(MachineStatus.fromJson({...base, 'lifting': true}).words(), 'Finishing the sent moves, then lifting — still cutting');
    });
    test('held wins over lifting', () {
      expect(MachineStatus.fromJson({...base, 'lifting': true, 'held': true}).words(), 'Held — press Resume at the computer');
    });
    test('tool change wins over all', () {
      final s = MachineStatus.fromJson({...base, 'toolChange': 3, 'held': true, 'lifting': true});
      expect(s.toolChange, 3);
      expect(s.words(), 'Tool change: T3 — answer at the computer');
    });
    test('toolChange -1 or not a number: none', () {
      expect(MachineStatus.fromJson({...base, 'toolChange': -1}).words(), 'Cutting Sign');
      expect(MachineStatus.fromJson({...base, 'toolChange': '3'}).toolChange, -1);
    });
  });
}
