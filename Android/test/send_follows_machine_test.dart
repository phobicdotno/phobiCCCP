// The Send dialog follows the machine the computer has in use when it is
// switched there while the dialog is open (sending switched it back).
import 'dart:io';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/app_state.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/store.dart';
import 'package:phobicccp/ui/send_dialog.dart';
import 'package:phobicccp/ui/theme.dart';

DesktopInfo info(String active) => DesktopInfo(host: 'shopA', id: 'iA', activeMachine: active, allowStart: true,
    machines: [MachineInfo(id: 'm1', name: 'Laser A', active: active == 'm1'), MachineInfo(id: 'X', name: 'Router A', active: active == 'X')],
    status: const MachineStatus(connected: true, state: 'Idle', homed: true));

void main() {
  testWidgets('switch at the computer while Send is open', (t) async {
    t.view.physicalSize = const Size(1280, 1000);
    t.view.devicePixelRatio = 1;
    addTearDown(t.view.reset);
    final dir = Directory.systemTemp.createTempSync('target');
    final app = AppState(DesignStore(Directory('${dir.path}/d')), Settings(File('${dir.path}/s.json')), deviceName: 'pad');
    final soon = DateTime.now().millisecondsSinceEpoch + 600000;
    final desk = Desktop(id: 'iA', host: 'shopA', address: '192.168.1.5', lastSeen: soon);
    app.computers = [Computer(desk, info: info('m1'))];
    final d = Design(name: 'Job', toolpaths: const [ToolpathSpec(name: 'p', type: 'profile', shapes: ['s1'], tool: 201, depth: 1)],
        shapes: const [Shape(id: 's1', type: Shape.rect, w: 10, h: 10)]);
    await t.pumpWidget(MaterialApp(theme: phobiTheme(), home: Scaffold(body: SendDialog(app, d))));
    await t.pump();
    // Someone at the computer switches to the router; the next sync says so.
    app.computers = [Computer(desk, info: info('X'))];
    // ignore: invalid_use_of_protected_member, invalid_use_of_visible_for_testing_member
    app.notifyListeners();
    await t.pump();
    final group = t.widget<RadioGroup<String>>(find.byType(RadioGroup<String>).last).groupValue;
    expect(group, 'X');
    await t.pumpWidget(const SizedBox());
    app.dispose();
  });
}
