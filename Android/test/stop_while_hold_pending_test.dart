// A Hold sent from the pad and not answered yet: Stop must still go out.
import 'dart:async';
import 'dart:io';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/app_state.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/store.dart';
import 'package:phobicccp/ui/machine_page.dart';
import 'package:phobicccp/ui/theme.dart';

class SlowApp extends AppState {
  SlowApp(super.store, super.settings, {required super.deviceName});
  final sent = <String>[];
  final hold = Completer<void>();
  @override
  Future<void> control(Computer c, String op) {
    sent.add('${c.desktop.id}:$op');
    return op == 'hold' ? hold.future : Future.value();
  }
}

void main() {
  testWidgets('Stop stays usable while a Hold waits for its answer', (t) async {
    t.view.physicalSize = const Size(1280, 800);
    t.view.devicePixelRatio = 1;
    addTearDown(t.view.reset);
    final dir = Directory.systemTemp.createTempSync('stophold');
    final app = SlowApp(DesignStore(Directory('${dir.path}/d')), Settings(File('${dir.path}/s.json')), deviceName: 'pad');
    app.lastTarget = 'iA/X';
    const run = MachineStatus(connected: true, homed: true, state: 'Run', streaming: true);
    final soon = DateTime.now().millisecondsSinceEpoch + 600000;
    app.computers = [
      Computer(Desktop(id: 'iA', host: 'shopA', address: '192.168.1.5', lastSeen: soon),
          info: DesktopInfo(host: 'shopA', id: 'iA', activeMachine: 'X', allowStart: true,
              machines: const [MachineInfo(id: 'X', name: 'Router A', active: true)], status: run)),
    ];
    await t.pumpWidget(MaterialApp(theme: phobiTheme(),
        home: Scaffold(body: MachinePage(app: app, design: null, onStatus: (_) {}, onAddComputer: () {}))));
    await t.pump();
    await t.tap(find.text('Hold'));
    await t.pump();
    expect(app.sent, ['iA:hold']);
    await t.tap(find.text('Stop'));
    await t.pump();
    await t.tap(find.text('Tap again to stop'));
    await t.pump();
    expect(app.sent, ['iA:hold', 'iA:stop']);
    app.hold.complete();
    await t.pump();
    await t.pumpWidget(const SizedBox());
    app.dispose();
  });
}
