// The Machine page's Start goes to the machine in use now, not the machine
// of an earlier send (lastTarget).
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/app_state.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/store.dart';
import 'package:phobicccp/ui/machine_page.dart';
import 'package:phobicccp/ui/theme.dart';

void main() {
  testWidgets('Start target after switching machines on the Machine page', (t) async {
    t.view.physicalSize = const Size(1280, 800);
    t.view.devicePixelRatio = 1;
    addTearDown(t.view.reset);
    final dir = Directory.systemTemp.createTempSync('target');
    final app = AppState(DesignStore(Directory('${dir.path}/d')), Settings(File('${dir.path}/s.json')), deviceName: 'pad');
    // An earlier send went to machine B.
    app.lastTarget = 'i1/B';
    // Then the user tapped machine A on this page: the computer now uses A
    // (what selectMachine leaves in app.computers).
    app.computers = [
      Computer(Desktop(id: 'i1', host: 'shop', address: '192.168.1.5', lastSeen: DateTime.now().millisecondsSinceEpoch + 600000),
          info: const DesktopInfo(host: 'shop', id: 'i1', activeMachine: 'A', allowStart: true,
              machines: [MachineInfo(id: 'A', name: 'Shapeoko A', active: true), MachineInfo(id: 'B', name: 'Router B')],
              status: MachineStatus(connected: true, homed: true, state: 'Idle'))),
    ];
    final d = Design(name: 'Job', toolpaths: const [ToolpathSpec(name: 'p', type: 'profile', shapes: ['s1'], tool: 201, depth: 1)],
        shapes: const [Shape(id: 's1', type: Shape.rect, w: 10, h: 10)]);
    await t.pumpWidget(MaterialApp(theme: phobiTheme(),
        home: Scaffold(body: MachinePage(app: app, design: d, onStatus: (_) {}, onAddComputer: () {}))));
    await t.pump();
    final to = t.widget<Text>(find.textContaining('To: ')).data;
    expect(to, 'To: shop · Shapeoko A');
    await t.pumpWidget(const SizedBox());
    app.dispose();
  });
}
