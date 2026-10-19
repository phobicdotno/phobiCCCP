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

  testWidgets('Area picks just the overlap of two rectangles, like a fill', (t) async {
    await open(t);
    final canvas = find.byWidgetPredicate((w) => w.runtimeType.toString() == 'DesignCanvas');
    final c = t.getRect(canvas).center + const Offset(0, 150);
    Future<void> rect(Offset from, Offset to) async {
      await t.tap(find.byTooltip('Rectangle'));
      await t.pumpAndSettle();
      final g = await t.startGesture(from);
      for (var i = 1; i <= 10; i++) {
        await g.moveBy((to - from) / 10);
        await t.pump();
      }
      await g.up();
      await t.pumpAndSettle();
    }

    await rect(c + const Offset(-90, -30), c + const Offset(20, 30));
    await rect(c + const Offset(-20, -30), c + const Offset(90, 30));
    expect(app.design(d.id)!.shapes.length, 8);
    await t.tap(find.byTooltip('Area: tap inside an area to select just it, like a fill, for a pocket'));
    await t.pumpAndSettle();
    await t.tapAt(c);
    await t.pumpAndSettle();
    final shapes = app.design(d.id)!.shapes;
    expect(shapes.length, 9);
    expect(shapes.last.type, Shape.path);
    expect(shapes.last.closed, true);
    final b = shapes.last.bounds();
    final ra = shapes[6].bounds(), rb = shapes[7].bounds();
    expect(b[0], closeTo(rb[0], 0.01));
    expect(b[2], closeTo(ra[2], 0.01));
    // Left of the overlap: the rest of the left rectangle, a new path too.
    await t.tapAt(c + const Offset(-70, 0));
    await t.pumpAndSettle();
    expect(app.design(d.id)!.shapes.length, 10);   // the part of the left one outside the overlap
    await t.tap(find.text('Undo'));
    await t.pumpAndSettle();
    await t.tap(find.text('Undo'));
    await t.pumpAndSettle();
    expect(app.design(d.id)!.shapes.length, 8);
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

  testWidgets('a toolpath card moves only after a half-second hold', (t) async {
    await open(t);
    await t.tap(find.text('2 · Toolpaths'));
    await t.pumpAndSettle();
    List<String> order() => [for (final p in app.design(d.id)!.toolpaths) p.name];
    final before = order();
    expect(before.first, 'Lettering');

    // Dragging straight away scrolls; nothing moves.
    var g = await t.startGesture(t.getCenter(find.text('Lettering')));
    for (var k = 0; k < 6; k++) {
      await g.moveBy(const Offset(0, 25));
      await t.pump(const Duration(milliseconds: 100));
    }
    await t.pump(const Duration(milliseconds: 600));
    await g.up();
    await t.pumpAndSettle();
    expect(order(), before);

    // Held still for half a second, the card comes loose and moves down.
    g = await t.startGesture(t.getCenter(find.text('Lettering')));
    await t.pump(const Duration(milliseconds: 550));
    for (var k = 0; k < 6; k++) {
      await g.moveBy(const Offset(0, 25));
      await t.pump(const Duration(milliseconds: 16));
    }
    await g.up();
    await t.pumpAndSettle();
    expect(order().first, isNot('Lettering'));
    expect(order().toSet(), before.toSet());
    await close(t);
  });
}
