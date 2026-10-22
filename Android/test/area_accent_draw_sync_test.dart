// Area leaves accented closed letters out like O and e; a shape drawn in
// the frame a sync lands gets an id the sync did not already take.
import 'dart:io';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/app_state.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/store.dart';
import 'package:phobicccp/ui/canvas.dart';
import 'package:phobicccp/ui/main_window.dart';
import 'package:phobicccp/ui/theme.dart';

void main() {
  testWidgets('Area: accented closed letters', (t) async {
    t.view.physicalSize = const Size(1280, 800);
    t.view.devicePixelRatio = 1;
    addTearDown(t.view.reset);
    final dir = Directory.systemTemp.createTempSync('ui');
    final store = DesignStore(Directory('${dir.path}/d'));
    final settings = Settings(File('${dir.path}/s.json'));
    final a = store.save(Design(name: 'Sign', shapes: const [
      Shape(id: 'r', type: Shape.rect, x: -2000, y: -2000, w: 4000, h: 4000),
      Shape(id: 'ol', type: Shape.text, x: 1500, y: 1500, label: 'ØL', size: 10),
      Shape(id: 'ete', type: Shape.text, x: 1500, y: 1000, label: 'été', size: 10),
      Shape(id: 'oe', type: Shape.text, x: 1500, y: 500, label: 'ÖL', size: 10),
    ]), now: 1000);
    settings.lastDesign = a.id;
    final app = AppState(store, settings, deviceName: 'pad');
    await t.pumpWidget(MaterialApp(theme: phobiTheme(), home: MainWindow(app)));
    await t.pumpAndSettle();
    await t.tap(find.byTooltip('Area: tap inside an area to select just it, like a fill, for a pocket'));
    await t.pumpAndSettle();
    await t.tapAt(t.getCenter(find.byType(DesignCanvas)));
    await t.pumpAndSettle();
    final texts = t.widgetList<Text>(find.byType(Text)).map((x) => x.data ?? '').toList();
    expect(texts.any((s) => s.contains('text item(s) not selected')), isTrue);
    await t.tap(find.text('Delete').first);
    await t.pumpAndSettle();
    expect(app.design(a.id)!.shapes.map((s) => s.id).toList(), containsAll(['ol', 'ete', 'oe']));
    await t.pumpWidget(const SizedBox());
    app.dispose();
  });

  testWidgets('draw finished in the frame a sync lands', (t) async {
    t.view.physicalSize = const Size(1280, 800);
    t.view.devicePixelRatio = 1;
    addTearDown(t.view.reset);
    final dir = Directory.systemTemp.createTempSync('ui');
    final store = DesignStore(Directory('${dir.path}/d'));
    final settings = Settings(File('${dir.path}/s.json'));
    final a = store.save(Design(name: 'Sign', shapes: const [
      Shape(id: 's1', type: Shape.rect, x: 0, y: 0, w: 10, h: 10),
    ]), now: 1000);
    settings.lastDesign = a.id;
    final app = AppState(store, settings, deviceName: 'pad');
    await t.pumpWidget(MaterialApp(theme: phobiTheme(), home: MainWindow(app)));
    await t.pumpAndSettle();
    await t.tap(find.byTooltip('Rectangle'));
    await t.pumpAndSettle();
    final r = t.getRect(find.byType(DesignCanvas));
    final g = await t.startGesture(r.center - const Offset(60, 40));
    for (var i = 1; i <= 10; i++) {
      await g.moveBy(const Offset(12, 8));
      await t.pump();
    }
    final desk = a.copyWith(
        modified: 5000,
        vv: {...a.vv, 'desk': 5000},
        shapes: [...a.shapes, const Shape(id: 's2', type: Shape.circle, x: 50, y: 50, r: 5)],
        toolpaths: [const ToolpathSpec(name: 'Drill 1', type: 'drill', shapes: ['s2'], tool: 201, depth: 3)]);
    store.put(desk);
    app.designs = store.live();
    app.notifyListeners();
    await g.up();
    await t.pumpAndSettle();
    final now = store.get(a.id)!;
    final ids = now.shapes.map((s) => s.id).toList();
    expect(ids.toSet().length, ids.length, reason: 'duplicate shape ids');
    await t.pumpWidget(const SizedBox());
    app.dispose();
  });
}
