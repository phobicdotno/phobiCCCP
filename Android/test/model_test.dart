import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/model.dart';

void main() {
  // tests/data/pad-design.json, which the desktop's test_padlink builds into a document.
  final fixture = File('../tests/data/pad-design.json');

  test('the shared fixture round-trips unchanged', () {
    final text = fixture.readAsStringSync();
    final d = Design.fromJson(jsonDecode(text) as Map<String, Object?>);
    expect(d.name, 'Door sign');
    expect(d.shapes.length, 6);
    expect(d.toolpaths.map((t) => t.type), ['engrave', 'pocket', 'drill', 'profile']);
    expect(jsonDecode(jsonEncode(d.toJson())), jsonDecode(text));
  });

  test('writes only what a shape uses', () {
    final s = const Shape(id: 's1', type: Shape.circle, x: 1, y: 2, r: 3).toJson();
    expect(s.keys.toSet(), {'id', 'type', 'x', 'y', 'r'});
  });

  test('reads what the desktop sends back', () {
    // Qt writes whole numbers without a fraction and may add keys we do not know.
    final d = Design.fromJson(jsonDecode('{"format":"phobicccp-pad","version":1,"id":"a","name":"A","modified":1760850000000,'
        '"stock":{"width":300,"height":200,"thickness":19},"shapes":[{"id":"s1","type":"rect","x":0,"y":0,"w":10,"h":10,"extra":1}],'
        '"toolpaths":[],"future":true}') as Map<String, Object?>);
    expect(d.modified, 1760850000000);
    expect(d.shapes.single.w, 10.0);
    expect(d.deleted, false);
  });

  test('moving and bounds', () {
    final p = const Shape(id: 'p', type: Shape.path, points: [[0, 0], [10, 5]]).moved(1, 2);
    expect(p.points, [[1, 2], [11, 7]]);
    expect(p.bounds(), [1, 2, 11, 7]);
    expect(const Shape(id: 'r', type: Shape.rect, x: 1, y: 1, w: 4, h: 2).moved(1, 0).bounds(), [2, 1, 6, 3]);
  });

  test('ids', () {
    expect(Design(shapes: const [Shape(id: 's2', type: Shape.point)]).nextShapeId(), 's3');
    expect(RegExp(r'^[A-Za-z0-9-]{1,64}$').hasMatch(newId()), true);
  });

  test('tool library picks', () {
    final lib = ToolLibrary.fromJson({
      'materials': [{'id': 'softwood', 'name': 'Soft wood'}],
      'tools': [
        {'number': 501, 'name': 'Engraver', 'kind': 'engraver', 'materials': ['softwood']},
        {'number': 201, 'name': 'End mill', 'kind': 'endmill', 'materials': ['softwood']},
        {'number': 999, 'name': 'Other', 'kind': 'endmill', 'materials': ['acrylic']},
      ],
    });
    expect(lib.defaultTool('softwood', 'profile'), 201);
    expect(lib.defaultTool('softwood', 'engrave'), 501);
    expect(lib.toolsFor('acrylic').single.number, 999);
    expect(lib.materialName('softwood'), 'Soft wood');
  });
}
