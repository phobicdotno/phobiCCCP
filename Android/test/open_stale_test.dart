// Open dialog left open while a newer copy of a design comes in: picking it
// opens the old copy, and the first edit is then thrown away.
import 'dart:io';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/app_state.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/store.dart';
import 'package:phobicccp/ui/main_window.dart';
import 'package:phobicccp/ui/theme.dart';

void main() {
  testWidgets('open a design that changed while the Open dialog was up', (t) async {
    t.view.physicalSize = const Size(1280, 800);
    t.view.devicePixelRatio = 1;
    addTearDown(t.view.reset);
    final dir = Directory.systemTemp.createTempSync('ui');
    final store = DesignStore(Directory('${dir.path}/d'));
    final settings = Settings(File('${dir.path}/s.json'));
    final a = store.save(Design(name: 'Alpha', shapes: const [Shape(id: 's1', type: Shape.rect, x: 0, y: 0, w: 10, h: 10)]), now: 1000);
    var b = store.save(Design(name: 'Bravo', shapes: const [Shape(id: 's1', type: Shape.circle, x: 50, y: 50, r: 5)]), now: 2000);
    settings.lastDesign = a.id;
    final app = AppState(store, settings, deviceName: 'pad');
    await t.pumpWidget(MaterialApp(theme: phobiTheme(), home: MainWindow(app)));
    await t.pumpAndSettle();
    await t.tap(find.text('File'));
    await t.pumpAndSettle();
    await t.tap(find.text('Open…'));
    await t.pumpAndSettle();
    // A newer Bravo arrives (as a sync pull would put it): a second circle.
    final remote = b.copyWith(shapes: [...b.shapes, const Shape(id: 's2', type: Shape.circle, x: 80, y: 80, r: 5)],
        modified: 5000, vv: {'desk': 5000, ...b.vv});
    store.put(remote);
    app.designs = store.live();
    app.notifyListeners();
    await t.pumpAndSettle();
    await t.tap(find.text('Bravo'));
    await t.pumpAndSettle();
    // Edit: Edit > Select all, then Delete (toolbar).
    await t.tap(find.text('Edit'));
    await t.pumpAndSettle();
    await t.tap(find.text('Select all'));
    await t.pumpAndSettle();
    await t.tap(find.text('Delete').first);
    await t.pumpAndSettle();
    final now = store.get(b.id)!;
    final status = t.widgetList<Text>(find.byType(Text)).map((x) => x.data ?? '').where((s) => s.contains('kept') || s.contains('Deleted')).toList();
    expect(now.shapes, isEmpty, reason: 'the delete was dropped: $status');
    await t.pumpWidget(const SizedBox());
    app.dispose();
  });
}
