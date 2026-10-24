// Area tool on large or busy designs: a triangle across a full sheet took
// 12 s and 1.6 GB (every edge filed in every cell of its box), and a board
// with 400 small circles 14 s (every edge tried against every other).
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/fill.dart';
import 'package:phobicccp/model.dart';

void main() {
  test('a triangle across a sheet fills quickly', () {
    final sw = Stopwatch()..start();
    final face = fillFace([
      [(0, 0), (2440, 0), (0, 1220)]
    ], (300, 300));
    expect(face.length, 1);
    expect(sw.elapsedMilliseconds, lessThan(3000));
  });

  test('a board with 400 circles fills quickly, with every hole', () {
    final rings = <List<Pt>>[
      [(0, 0), (1000.0, 0), (1000.0, 1000.0), (0, 1000.0)]
    ];
    for (var i = 0; i < 20; i++) {
      for (var j = 0; j < 20; j++) {
        rings.add(shapeRing(Shape(id: 's$i-$j', type: Shape.circle, x: 25.0 + 50 * i, y: 25.0 + 50 * j, r: 10))!);
      }
    }
    final sw = Stopwatch()..start();
    final face = fillFace(rings, (2, 2));
    expect(face.length, 401);
    expect(sw.elapsedMilliseconds, lessThan(8000));
  });
}
