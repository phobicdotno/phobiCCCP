// Toolpath Delete tapped after a sync swapped the design but before the next frame.
import 'dart:io';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/app_state.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/store.dart';
import 'package:phobicccp/ui/main_window.dart';
import 'package:phobicccp/ui/theme.dart';

void main() {
  for (final action in ['Delete', 'Duplicate', '▼']) {
    testWidgets('toolpath $action in the window after a sync', (t) async {
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
            ToolpathSpec(name: 'Profile 1', type: 'profile', shapes: ['s1'], tool: 201, depth: 3),
            ToolpathSpec(name: 'Pocket 1', type: 'pocket', shapes: ['s1'], tool: 201, depth: 2),
          ]),
          now: 1000);
      settings.lastDesign = a.id;
      final app = AppState(store, settings, deviceName: 'pad');
      await t.pumpWidget(MaterialApp(theme: phobiTheme(), home: MainWindow(app)));
      await t.pumpAndSettle();
      await t.tap(find.text('2 · Toolpaths'));
      await t.pumpAndSettle();
      await t.tap(find.text('Profile 1'));
      await t.pumpAndSettle();
      // A computer added a circle and a drill toolpath; the sync brings it in.
      final desk = a.copyWith(
          modified: 5000,
          vv: {...a.vv, 'desk': 5000},
          shapes: [...a.shapes, const Shape(id: 's2', type: Shape.circle, x: 50, y: 50, r: 5)],
          toolpaths: [...a.toolpaths, const ToolpathSpec(name: 'Drill 1', type: 'drill', shapes: ['s2'], tool: 201, depth: 3)]);
      store.put(desk);
      app.designs = store.live();
      app.notifyListeners();
      // The user's tap lands before the next frame is drawn.
      await t.tap(action == '▼' ? find.text(action).first : find.text(action).last);
      await t.pumpAndSettle();
      final now = store.get(a.id)!;
      expect(now.shapes.any((s) => s.id == 's2'), isTrue, reason: 'the computer\'s circle was lost');
      expect(now.toolpaths.any((x) => x.name == 'Drill 1'), isTrue, reason: 'the computer\'s toolpath was lost');
      await t.pumpWidget(const SizedBox());
      app.dispose();
    });
  }
}
