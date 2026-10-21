// Designs deleted elsewhere while the Open dialog is up: scrolling to the end
// builds cards past the new end of the list.
import 'dart:io';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/app_state.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/store.dart';
import 'package:phobicccp/ui/main_window.dart';
import 'package:phobicccp/ui/theme.dart';

void main() {
  testWidgets('scroll the Open list after designs went away', (t) async {
    t.view.physicalSize = const Size(1280, 800);
    t.view.devicePixelRatio = 1;
    addTearDown(t.view.reset);
    final dir = Directory.systemTemp.createTempSync('ui');
    final store = DesignStore(Directory('${dir.path}/d'));
    final settings = Settings(File('${dir.path}/s.json'));
    final all = [for (var i = 0; i < 40; i++) store.save(Design(name: 'D$i'), now: 1000 + i)];
    settings.lastDesign = all.last.id;
    final app = AppState(store, settings, deviceName: 'pad');
    await t.pumpWidget(MaterialApp(theme: phobiTheme(), home: MainWindow(app)));
    await t.pumpAndSettle();
    await t.tap(find.text('File'));
    await t.pumpAndSettle();
    await t.tap(find.text('Open…'));
    await t.pumpAndSettle();
    for (var i = 0; i < 30; i++) {
      store.delete(all[i].id);
    }
    app.designs = store.live();
    app.notifyListeners();
    await t.pumpAndSettle();
    await t.drag(find.byType(GridView), const Offset(0, -3000));
    await t.pumpAndSettle();
    expect(t.takeException(), isNull);
    await t.pumpWidget(const SizedBox());
    app.dispose();
  });
}
