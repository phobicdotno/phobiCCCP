// The window on a pad-sized screen: drawing with a tool, undo, and making a
// toolpath from selected vectors, as on the desktop.

import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/app_state.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/store.dart';
import 'package:phobicccp/ui/main_window.dart';
import 'package:phobicccp/ui/theme.dart';

void main() {
  late AppState app;
  late Design d;

  Future<void> open(WidgetTester t) async {
    t.view.physicalSize = const Size(1280, 800);
    t.view.devicePixelRatio = 1;
    addTearDown(t.view.reset);
    final dir = Directory.systemTemp.createTempSync('ui');
    final store = DesignStore(Directory('${dir.path}/d'));
    final settings = Settings(File('${dir.path}/s.json'));
    d = Design.fromJson((jsonDecode(File('../tests/data/pad-design.json').readAsStringSync()) as Map).cast<String, Object?>());
    store.put(d);
    settings.lastDesign = d.id;
    app = AppState(store, settings, deviceName: 'pad');
    await t.pumpWidget(MaterialApp(theme: phobiTheme(), home: MainWindow(app)));
    await t.pumpAndSettle();
  }

  // Saving schedules a sync; stop it before the test ends.
  Future<void> close(WidgetTester t) async {
    await t.pumpWidget(const SizedBox());
    app.dispose();
  }

  testWidgets('opens the last design with the desktop layout', (t) async {
    await open(t);
    expect(find.text('1 · Design'), findsOneWidget);
    expect(find.text('Properties'), findsOneWidget);
    expect(find.text('·  Door sign'), findsOneWidget);
    await close(t);
  });

  testWidgets('a rectangle is drawn by dragging, and Undo takes it away', (t) async {
    await open(t);
    await t.tap(find.byTooltip('Rectangle'));
    await t.pumpAndSettle();
    final canvas = find.byWidgetPredicate((w) => w.runtimeType.toString() == 'DesignCanvas');
    final r = t.getRect(canvas);
    final g = await t.startGesture(r.center - const Offset(60, 40));
    for (var i = 1; i <= 10; i++) {
      await g.moveBy(const Offset(12, 8));
      await t.pump();
    }
    await g.up();
    await t.pumpAndSettle();
    expect(app.design(d.id)!.shapes.length, 7);
    expect(app.design(d.id)!.shapes.last.type, Shape.rect);
    await t.tap(find.text('Undo'));
    await t.pumpAndSettle();
    expect(app.design(d.id)!.shapes.length, 6);
    await close(t);
  });

  testWidgets('a toolpath is made from the selected vectors', (t) async {
    await open(t);
    await t.tap(find.text('Shapes'));
    await t.pumpAndSettle();
    await t.tap(find.text('Circle Ø25'));
    await t.tap(find.text('2 · Toolpaths'));
    await t.pumpAndSettle();
    await t.tap(find.text('Pocket'));
    await t.pumpAndSettle();
    final tps = app.design(d.id)!.toolpaths;
    expect(tps.length, 5);
    expect(tps.last.type, 'pocket');
    expect(tps.last.shapes, ['s2']);
    await close(t);
  });
}
