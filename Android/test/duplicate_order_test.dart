// Toolpath Duplicate on the pad, as on the computer: the copy right after the original.
import 'dart:io';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/app_state.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/store.dart';
import 'package:phobicccp/ui/main_window.dart';
import 'package:phobicccp/ui/theme.dart';

void main() {
  testWidgets('duplicate lands right after the original', (t) async {
    t.view.physicalSize = const Size(1280, 800);
    t.view.devicePixelRatio = 1;
    addTearDown(t.view.reset);
    final dir = Directory.systemTemp.createTempSync('ui');
    final store = DesignStore(Directory('${dir.path}/d'));
    final settings = Settings(File('${dir.path}/s.json'));
    final a = store.save(
        Design(name: 'Sign', shapes: const [
          Shape(id: 's1', type: Shape.rect, x: 0, y: 0, w: 10, h: 10),
        ], toolpaths: const [
          ToolpathSpec(name: 'Pocket 1', type: 'pocket', shapes: ['s1'], tool: 201, depth: 2),
          ToolpathSpec(name: 'Profile 1', type: 'profile', shapes: ['s1'], tool: 201, depth: 12),
        ]),
        now: 1000);
    settings.lastDesign = a.id;
    final app = AppState(store, settings, deviceName: 'pad');
    await t.pumpWidget(MaterialApp(theme: phobiTheme(), home: MainWindow(app)));
    await t.pumpAndSettle();
    await t.tap(find.text('2 · Toolpaths'));
    await t.pumpAndSettle();
    await t.tap(find.text('Pocket 1'));
    await t.pumpAndSettle();
    await t.tap(find.text('Duplicate').last);
    await t.pumpAndSettle();
    final names = store.get(a.id)!.toolpaths.map((x) => x.name).toList();
    // ToolpathPanel::duplicateCurrent inserts at index + 1.
    expect(names, ['Pocket 1', 'Pocket 1 copy', 'Profile 1']);
    await t.pumpWidget(const SizedBox());
    app.dispose();
  });
}
