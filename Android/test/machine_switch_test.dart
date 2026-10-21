// Machine page targets with machine ids shared between computers, and a
// machine switched back at the desktop.
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
  @override
  Future<void> selectMachine(Computer c, String machineId) async {
    final i = c.info!;
    computers = [
      for (final x in computers)
        x.desktop.id == c.desktop.id
            ? Computer(x.desktop, info: DesktopInfo(host: i.host, id: i.id, activeMachine: machineId, allowStart: true,
                machines: [for (final m in i.machines) MachineInfo(id: m.id, name: m.name, active: m.id == machineId)],
                status: i.status))
            : x
    ];
    notifyListeners();
  }
}

void main() {
  testWidgets('a machine picked on computer B leaves computer A\'s target alone', (t) async {
    t.view.physicalSize = const Size(1280, 800);
    t.view.devicePixelRatio = 1;
    addTearDown(t.view.reset);
    final dir = Directory.systemTemp.createTempSync('target');
    final app = FakeApp(DesignStore(Directory('${dir.path}/d')), Settings(File('${dir.path}/s.json')), deviceName: 'pad');
    app.lastTarget = 'iA/X';
    final ok = const MachineStatus(connected: true, homed: true, state: 'Idle');
    final soon = DateTime.now().millisecondsSinceEpoch + 600000;
    app.computers = [
      Computer(Desktop(id: 'iA', host: 'shopA', address: '192.168.1.5', lastSeen: soon),
          info: DesktopInfo(host: 'shopA', id: 'iA', activeMachine: 'X', allowStart: true,
              machines: const [MachineInfo(id: 'm1', name: 'Laser A'), MachineInfo(id: 'X', name: 'Router A', active: true)],
              status: ok)),
      Computer(Desktop(id: 'iB', host: 'shopB', address: '192.168.1.6', lastSeen: soon),
          info: DesktopInfo(host: 'shopB', id: 'iB', activeMachine: 'Y', allowStart: true,
              machines: const [MachineInfo(id: 'm1', name: 'Shapeoko B'), MachineInfo(id: 'Y', name: 'Other B', active: true)],
              status: ok)),
    ];
    final d = Design(name: 'Job', toolpaths: const [ToolpathSpec(name: 'p', type: 'profile', shapes: ['s1'], tool: 201, depth: 1)],
        shapes: const [Shape(id: 's1', type: Shape.rect, w: 10, h: 10)]);
    await t.pumpWidget(MaterialApp(theme: phobiTheme(),
        home: Scaffold(body: MachinePage(app: app, design: d, onStatus: (_) {}, onAddComputer: () {}))));
    await t.pump();
    expect(t.widget<Text>(find.textContaining('To: ')).data, 'To: shopA · Router A');
    await t.tap(find.text('Shapeoko B'));
    await t.pumpAndSettle();
    // A machine on computer B with the same id must not retarget computer A.
    expect(t.widget<Text>(find.textContaining('To: ')).data, 'To: shopA · Router A');
    await t.pumpWidget(const SizedBox());
    app.dispose();
  });

  testWidgets('the target follows the desktop switching machines back', (t) async {
    t.view.physicalSize = const Size(1280, 800);
    t.view.devicePixelRatio = 1;
    addTearDown(t.view.reset);
    final dir = Directory.systemTemp.createTempSync('target');
    final app = FakeApp(DesignStore(Directory('${dir.path}/d')), Settings(File('${dir.path}/s.json')), deviceName: 'pad');
    final ok = const MachineStatus(connected: true, homed: true, state: 'Idle');
    final soon = DateTime.now().millisecondsSinceEpoch + 600000;
    Computer a(String active) => Computer(Desktop(id: 'iA', host: 'shopA', address: '192.168.1.5', lastSeen: soon),
          info: DesktopInfo(host: 'shopA', id: 'iA', activeMachine: active, allowStart: true,
              machines: [MachineInfo(id: 'm1', name: 'Laser A', active: active == 'm1'), MachineInfo(id: 'X', name: 'Router A', active: active == 'X')],
              status: ok));
    app.computers = [a('m1')];
    final d = Design(name: 'Job', toolpaths: const [ToolpathSpec(name: 'p', type: 'profile', shapes: ['s1'], tool: 201, depth: 1)],
        shapes: const [Shape(id: 's1', type: Shape.rect, w: 10, h: 10)]);
    await t.pumpWidget(MaterialApp(theme: phobiTheme(),
        home: Scaffold(body: MachinePage(app: app, design: d, onStatus: (_) {}, onAddComputer: () {}))));
    await t.pump();
    await t.tap(find.text('Router A'));
    await t.pumpAndSettle();
    expect(t.widget<Text>(find.textContaining('To: ')).data, 'To: shopA · Router A');
    app.computers = [a('m1')]; // desktop user switched back to the laser; next sync
    app.notifyListeners();
    await t.pump();
    expect(t.widget<Text>(find.textContaining('To: ')).data, 'To: shopA · Laser A');
    await t.pumpWidget(const SizedBox());
    app.dispose();
  });
}
