// Area on a shape with lettering inside: lettering the pad is sure about is
// selected with it, as on the computer, so a pocket leaves it standing; text
// near the edge or with closed letters (A, B) is left out with a note.
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
  testWidgets('Area selects the text standing inside the area', (t) async {
    t.view.physicalSize = const Size(1280, 800);
    t.view.devicePixelRatio = 1;
    addTearDown(t.view.reset);
    final dir = Directory.systemTemp.createTempSync('ui');
    final store = DesignStore(Directory('${dir.path}/d'));
    final settings = Settings(File('${dir.path}/s.json'));
    final a = store.save(Design(name: 'Sign', shapes: const [
      Shape(id: 'r', type: Shape.rect, x: -2000, y: -2000, w: 4000, h: 4000),
      Shape(id: 'in', type: Shape.text, x: 1500, y: 1500, label: 'TLW', size: 10),
      Shape(id: 'closed', type: Shape.text, x: 1500, y: 1000, label: 'AB', size: 10),
      Shape(id: 'edge', type: Shape.text, x: 1995, y: 0, label: 'CD', size: 10),
      Shape(id: 'out', type: Shape.text, x: 2500, y: 0, label: 'EF', size: 10),
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
    expect(texts.any((s) => s.contains('2 text item(s) not selected')), isTrue);
    await t.tap(find.text('Delete').first);
    await t.pumpAndSettle();
    expect(app.design(a.id)!.shapes.map((s) => s.id).toList(), ['closed', 'edge', 'out']);
    await t.pumpWidget(const SizedBox());
    app.dispose();
  });
}
