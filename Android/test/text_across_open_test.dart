// The Text box open on design A when the computer sends design B to open
// here (File > Send to pad): OK added the text to B; it is dropped now.
import 'dart:io';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/app_state.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/store.dart';
import 'package:phobicccp/ui/main_window.dart';
import 'package:phobicccp/ui/theme.dart';

void main() {
  testWidgets('text dialog across an open from the computer', (t) async {
    t.view.physicalSize = const Size(1280, 800);
    t.view.devicePixelRatio = 1;
    addTearDown(t.view.reset);
    final dir = Directory.systemTemp.createTempSync('ui');
    final store = DesignStore(Directory('${dir.path}/d'));
    final settings = Settings(File('${dir.path}/s.json'));
    final a = store.save(Design(name: 'A'), now: 1000);
    final b = store.save(Design(name: 'B', shapes: const [Shape(id: 's1', type: Shape.rect, x: 0, y: 0, w: 10, h: 10)]), now: 1001);
    settings.lastDesign = a.id;
    final app = AppState(store, settings, deviceName: 'pad');
    await t.pumpWidget(MaterialApp(theme: phobiTheme(), home: MainWindow(app)));
    await t.pumpAndSettle();
    await t.tap(find.byTooltip('Text'));
    await t.pumpAndSettle();
    // tap the middle of the canvas
    await t.tapAt(const Offset(400, 400));
    await t.pumpAndSettle();
    expect(find.text('Text'), findsWidgets);
    // Someone at the computer sends B to this pad.
    app.openSent(store.get(b.id)!, 'desk');
    await t.pumpAndSettle();
    await t.enterText(find.byType(TextField).first, 'HELLO');
    await t.tap(find.text('OK'));
    await t.pumpAndSettle();
    final nb = store.get(b.id)!;
    expect(store.get(a.id)!.shapes, isEmpty);
    expect(nb.shapes.length, 1, reason: 'text typed for A landed in B');
    await t.pumpWidget(const SizedBox());
    app.dispose();
  });
}
