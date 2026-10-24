// a design deleted elsewhere while the Open dialog is up, then picked.
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
  testWidgets('pick a design deleted while the Open dialog was up', (t) async {
    t.view.physicalSize = const Size(1280, 800);
    t.view.devicePixelRatio = 1;
    addTearDown(t.view.reset);
    final dir = Directory.systemTemp.createTempSync('ui');
    final store = DesignStore(Directory('${dir.path}/d'));
    final settings = Settings(File('${dir.path}/s.json'));
    final a = store.save(Design(name: 'Alpha', shapes: const [Shape(id: 's1', type: Shape.rect, x: 0, y: 0, w: 10, h: 10)]), now: 1000);
    final b = store.save(Design(name: 'Bravo', shapes: const [Shape(id: 's1', type: Shape.circle, x: 50, y: 50, r: 5)]), now: 2000);
    settings.lastDesign = a.id;
    final app = AppState(store, settings, deviceName: 'pad');
    await t.pumpWidget(MaterialApp(theme: phobiTheme(), home: MainWindow(app)));
    await t.pumpAndSettle();
    await t.tap(find.text('File'));
    await t.pumpAndSettle();
    await t.tap(find.text('Open…'));
    await t.pumpAndSettle();
    // Bravo deleted on a computer, pulled by a sync.
    store.put(Design(id: b.id, name: b.name, deleted: true, modified: 5000, vv: {'desk': 5000, ...b.vv}));
    app.designs = store.live();
    app.notifyListeners();
    await t.pumpAndSettle();
    await t.tap(find.text('Bravo'));
    await t.pumpAndSettle();
    final texts = t.widgetList<Text>(find.byType(Text)).map((x) => x.data ?? '').toList();

    // Draw a point on it.
    await t.tap(find.byTooltip('Point (drill here)'));
    await t.pumpAndSettle();
    await t.tapAt(t.getCenter(find.byType(DesignCanvas)));
    await t.pumpAndSettle();

    final now = store.get(b.id)!;

    expect(now.deleted && now.shapes.isNotEmpty, isFalse, reason: 'edit saved into a deletion record');
    expect(texts.any((s) => s.contains('was deleted on another pad')), isTrue);
    await t.pumpWidget(const SizedBox());
    app.dispose();
  });
}
