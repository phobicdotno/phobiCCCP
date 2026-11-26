// All settings: a pocket's fields come from the desktop form's catalog, and
// edits land in the toolpath's `set` (or its own field) within the form's ranges.
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/toolpath_fields.dart';
import 'package:phobicccp/ui/toolpath_settings.dart';

const pocket = ToolpathSpec(
  name: 'Pocket 1',
  type: 'pocket',
  shapes: ['r'],
  tool: 102,
  depth: 3,
  setTool: 102,
  set: {
    'start_depth': 0.0,
    'stepdown': 1.5,
    'stepover': 2.5,
    'stock_to_leave': 0.0,
    'speeds.feedrate': 1500.0,
    'helix_entry': true,
    'helix_diameter': 6.35,
    'enable_rest': false,
    'rest_diameter': 6.35,
    'angle': 45.0,
    'phobic_note': 'x',
  },
  extra: {'strategy': 'raster', 'rasterAngle': 30.0},
);

void main() {
  test('a pocket shows the desktop fields it has, in card order', () {
    final keys = [for (final f in tpVisibleFields(pocket)) f.key];
    expect(keys, containsAll(['start_depth', 'stepdown', 'stepover', 'pocket_strategy', 'raster_angle', 'corners',
      'speeds.feedrate', 'helix_entry', 'helix_diameter', 'enable_rest', 'rest_diameter']));
    expect(keys, isNot(contains('angle')), reason: 'the pocket export does not use angle');
    expect(keys, isNot(contains('end_depth')), reason: 'End is the form\'s own row');
    expect(keys.indexOf('stepdown') < keys.indexOf('stepover'), isTrue);
    expect(keys.indexOf('stepover') < keys.indexOf('helix_entry'), isTrue);
    expect(tpSetting(pocket, 'raster_angle'), 30.0);
    // Rings: no raster angle.
    final rings = tpWithSetting(pocket, tpFieldCatalog('pocket_toolpath').firstWhere((f) => f.key == 'pocket_strategy'), 'offset');
    expect(rings.extra.containsKey('strategy'), isFalse);
    expect([for (final f in tpVisibleFields(rings)) f.key], isNot(contains('raster_angle')));
    expect(tpOtherKeys(pocket), ['phobic_note']);
  });

  test('edits go into set, held to the range', () {
    final f = tpFieldCatalog('pocket_toolpath').firstWhere((f) => f.key == 'stepover');
    final e = tpWithSetting(pocket, f, 500.0);
    expect(e.set['stepover'], 100.0);
    expect(e.toJson()['set'], containsPair('stepover', 100.0));
    expect(ToolpathSpec.fromJson(e.toJson()).set['stepover'], 100.0);
    // Another bit: the old bit's feeds and steps go.
    final b = e.copyWith(tool: 201);
    expect(b.set.containsKey('speeds.feedrate'), isFalse);
    expect(b.set.containsKey('stepover'), isFalse);
    expect(b.set['helix_entry'], true);
    expect(b.setTool, 201);
  });

  testWidgets('the panel renders a pocket and writes a switch back into set', (t) async {
    ToolpathSpec? got;
    await t.pumpWidget(MaterialApp(
      home: Scaffold(
        body: SingleChildScrollView(child: ToolpathSettings(pocket, (n) => got = n, initiallyOpen: true)),
      ),
    ));
    expect(find.text('Stepover'), findsOneWidget);
    expect(find.text('Raster angle'), findsOneWidget);
    expect(find.text('Helix in'), findsOneWidget);
    expect(find.text('ENTRY'), findsOneWidget);
    expect(find.text('phobic_note: x'), findsOneWidget);
    final helixRow = find.ancestor(of: find.text('Helix in'), matching: find.byType(Row)).first;
    final sw = find.descendant(of: helixRow, matching: find.byType(Switch));
    await t.ensureVisible(sw);
    await t.tap(sw);
    await t.pump();
    expect(got?.set['helix_entry'], false);
    expect(got?.set['stepover'], 2.5);
  });
}
