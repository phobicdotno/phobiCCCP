// The Machine page shows a banner once when the computer's job ends
// (padlink status jobEnd), not for a job that had already ended when the
// page opened.
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/app_state.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/store.dart';
import 'package:phobicccp/ui/machine_page.dart';
import 'package:phobicccp/ui/theme.dart';

Computer _shop(MachineStatus st) => Computer(
    Desktop(id: 'i1', host: 'shop', address: '192.168.1.5', lastSeen: DateTime.now().millisecondsSinceEpoch + 600000),
    info: DesktopInfo(host: 'shop', id: 'i1', activeMachine: 'A', allowStart: true,
        machines: const [MachineInfo(id: 'A', name: 'Shapeoko A', active: true)], status: st));

void main() {
  testWidgets('Banner when the job ends', (t) async {
    t.view.physicalSize = const Size(1280, 800);
    t.view.devicePixelRatio = 1;
    addTearDown(t.view.reset);
    final dir = Directory.systemTemp.createTempSync('jobend');
    final app = AppState(DesignStore(Directory('${dir.path}/d')), Settings(File('${dir.path}/s.json')), deviceName: 'pad');
    app.computers = [_shop(const MachineStatus(connected: true, homed: true, state: 'Idle', jobEnd: 'stopped', job: 'old.cccp'))];
    await t.pumpWidget(MaterialApp(theme: phobiTheme(),
        home: Scaffold(body: MachinePage(app: app, design: null, onStatus: (_) {}, onAddComputer: () {}))));
    await t.pump();
    expect(find.byType(SnackBar), findsNothing);
    // A new job runs, then finishes.
    app.computers = [_shop(const MachineStatus(connected: true, homed: true, streaming: true, state: 'Run', job: 'sign.cccp'))];
    // ignore: invalid_use_of_protected_member, invalid_use_of_visible_for_testing_member
    app.notifyListeners();
    await t.pump();
    expect(find.byType(SnackBar), findsNothing);
    app.computers = [_shop(const MachineStatus(connected: true, homed: true, state: 'Idle', jobEnd: 'finished', job: 'sign.cccp'))];
    // ignore: invalid_use_of_protected_member, invalid_use_of_visible_for_testing_member
    app.notifyListeners();
    await t.pump();
    await t.pump(const Duration(milliseconds: 500));
    expect(find.text('✓ sign.cccp finished on shop'), findsOneWidget);
    await t.pumpWidget(const SizedBox());
    app.dispose();
  });
}
