// The Area tool's fill, as phobiCCCP's vec::fillFace (tests/test_vectorops.cpp
// checks the same cases on the desktop).

import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/fill.dart';
import 'package:phobicccp/model.dart';

double area(List<List<Pt>> rings) => rings.fold(0.0, (a, r) => a + ringArea(r)).abs();

void main() {
  List<Pt> ring(Shape s) => shapeRing(s)!;
  final a = ring(const Shape(id: 'a', type: Shape.rect, x: 0, y: 0, w: 20, h: 20)); // 0..20
  final b = ring(const Shape(id: 'b', type: Shape.rect, x: 10, y: 0, w: 20, h: 20)); // 10..30

  test('the overlap of two squares', () {
    final f = fillFace([a, b], (15, 10));
    expect(f.length, 1);
    expect(area(f), closeTo(200, 0.01));
  });

  test('one square without the overlap', () {
    final f = fillFace([a, b], (5, 10));
    expect(f.length, 1);
    expect(area(f), closeTo(200, 0.01));
    expect(f.first.every((p) => p.$1 <= 10 + 1e-9), isTrue);
  });

  test('outside every shape is nothing', () {
    expect(fillFace([a, b], (50, 50)), isEmpty);
  });

  test('a ring between a square and its island, the island’s own inside left out', () {
    final big = ring(const Shape(id: 'big', type: Shape.rect, x: 30, y: 30, w: 40, h: 40));
    final isl = ring(const Shape(id: 'isl', type: Shape.rect, x: 40, y: 40, w: 20, h: 20));
    final dot = ring(const Shape(id: 'dot', type: Shape.circle, x: 50, y: 50, r: 3));
    var f = fillFace([big, isl, dot], (35, 50));
    expect(f.length, 2);
    expect(area(f), closeTo(1600 - 400, 0.01));
    expect(sameRing(f[0], big) && sameRing(f[1], isl), isTrue);
    f = fillFace([big, isl, dot], (45, 50));
    expect(f.length, 2);
    expect(area(f), closeTo(400 - 3.14159 * 9, 0.2));
  });

  test('shapes sharing an edge stay apart', () {
    final c = ring(const Shape(id: 'c', type: Shape.rect, x: 20, y: 0, w: 10, h: 20)); // touches a at x = 20
    final f = fillFace([a, c], (25, 10));
    expect(f.length, 1);
    expect(sameRing(f.first, c), isTrue);
  });

  test('a closed path and a circle overlapping', () {
    final tri = ring(
      const Shape(
        id: 't',
        type: Shape.path,
        points: [
          [0, 0],
          [40, 0],
          [20, 30],
          [0, 0],
        ],
        closed: true,
      ),
    );
    final circ = ring(const Shape(id: 'c', type: Shape.circle, x: 20, y: 0, r: 10));
    final inBoth = fillFace([tri, circ], (20, 5));
    final onlyTri = fillFace([tri, circ], (20, 20));
    expect(inBoth.length, 1);
    expect(area(inBoth), closeTo(3.14159 * 100 / 2, 0.5)); // the upper half of the circle
    expect(area(onlyTri), closeTo(600 - 3.14159 * 100 / 2, 0.5));
  });
}
