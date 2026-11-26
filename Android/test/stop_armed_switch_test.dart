// Stop armed on computer A, then computer B picked: the next Stop tap only
// arms Stop for B, it does not stop B.
import 'dart:io';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/app_state.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/store.dart';
import 'package:phobicccp/ui/machine_page.dart';
import 'package:phobicccp/ui/theme.dart';

class FakeApp extends AppState {
  FakeApp(super.store, super.settings, {required super.deviceName});
  final sent = <String>[];
  @override
  Future<void> control(Computer c, String op) async => sent.add('${c.desktop.id}:$op');
}

void main() {
  testWidgets('Stop armed for one computer does not stop another', (t) async {
    t.view.physicalSize = const Size(1280, 800);
    t.view.devicePixelRatio = 1;
    addTearDown(t.view.reset);
    final dir = Directory.systemTemp.createTempSync('stoparm');
    final app = FakeApp(DesignStore(Directory('${dir.path}/d')), Settings(File('${dir.path}/s.json')), deviceName: 'pad');
    app.lastTarget = 'iA/X';
    const run = MachineStatus(connected: true, homed: true, state: 'Run', streaming: true);
    final soon = DateTime.now().millisecondsSinceEpoch + 600000;
    app.computers = [
      Computer(Desktop(id: 'iA', host: 'shopA', address: '192.168.1.5', lastSeen: soon),
          info: DesktopInfo(host: 'shopA', id: 'iA', activeMachine: 'X', allowStart: true,
              machines: const [MachineInfo(id: 'X', name: 'Router A', active: true)], status: run)),
      Computer(Desktop(id: 'iB', host: 'shopB', address: '192.168.1.6', lastSeen: soon),
          info: DesktopInfo(host: 'shopB', id: 'iB', activeMachine: 'Y', allowStart: true,
              machines: const [MachineInfo(id: 'Y', name: 'Router B', active: true)], status: run)),
    ];
    final d = Design(name: 'Job', toolpaths: const [ToolpathSpec(name: 'p', type: 'profile', shapes: ['s1'], tool: 201, depth: 1)],
        shapes: const [Shape(id: 's1', type: Shape.rect, w: 10, h: 10)]);
    await t.pumpWidget(MaterialApp(theme: phobiTheme(),
        home: Scaffold(body: MachinePage(app: app, design: d, onStatus: (_) {}, onAddComputer: () {}))));
    await t.pump();
    await t.tap(find.text('Stop'));
    await t.pump();
    expect(find.text('Tap again to stop'), findsOneWidget);
    await t.tap(find.text('shopB'));
    await t.pump();
    expect(find.text('Tap again to stop'), findsNothing);
    await t.tap(find.text('Stop'));
    await t.pump();
    expect(app.sent, isEmpty);
    expect(find.text('Tap again to stop'), findsOneWidget);
    await t.tap(find.text('Tap again to stop'));
    await t.pump();
    expect(app.sent, ['iB:stop']);
    await t.pumpWidget(const SizedBox());
    app.dispose();
  });
}
