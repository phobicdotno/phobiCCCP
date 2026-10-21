// The Send dialog's default machine after the computer switched machines.
import 'dart:io';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/app_state.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/store.dart';
import 'package:phobicccp/ui/send_dialog.dart';
import 'package:phobicccp/ui/theme.dart';

void main() {
  testWidgets('Send dialog defaults to the machine in use, not the last send\'s', (t) async {
    t.view.physicalSize = const Size(1280, 800);
    t.view.devicePixelRatio = 1;
    addTearDown(t.view.reset);
    final dir = Directory.systemTemp.createTempSync('target');
    final app = AppState(DesignStore(Directory('${dir.path}/d')), Settings(File('${dir.path}/s.json')), deviceName: 'pad');
    app.lastTarget = 'iA/X';
    final soon = DateTime.now().millisecondsSinceEpoch + 600000;
    app.computers = [
      Computer(Desktop(id: 'iA', host: 'shopA', address: '192.168.1.5', lastSeen: soon),
          info: const DesktopInfo(host: 'shopA', id: 'iA', activeMachine: 'm1', allowStart: true,
              machines: [MachineInfo(id: 'm1', name: 'Laser A', active: true), MachineInfo(id: 'X', name: 'Router A')],
              status: MachineStatus(connected: false, state: ''))),
    ];
    final d = Design(name: 'Job', toolpaths: const [ToolpathSpec(name: 'p', type: 'profile', shapes: ['s1'], tool: 201, depth: 1)],
        shapes: const [Shape(id: 's1', type: Shape.rect, w: 10, h: 10)]);
    await t.pumpWidget(MaterialApp(theme: phobiTheme(), home: Scaffold(body: SendDialog(app, d))));
    await t.pump();
    final group = t.widget<RadioGroup<String>>(find.byType(RadioGroup<String>).last).groupValue;
    expect(group, 'm1');
    await t.pumpWidget(const SizedBox());
    app.dispose();
  });
}
