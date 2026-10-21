// Area tool: a 600 x 400 table top with a 20 x 20 square overlapping its
// corner (10 x 10 of it inside). A tap in the middle picks the table minus
// that corner, not the whole table (the desktop Area tool agrees).
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/fill.dart';
import 'package:phobicccp/model.dart';

void main() {
  moreNotches();
  test('notched face is not the whole rectangle', () {
    const table = Shape(id: 't', type: Shape.rect, x: 0, y: 0, w: 600, h: 400);
    const notch = Shape(id: 'n', type: Shape.rect, x: 590, y: 390, w: 20, h: 20);
    final rings = [shapeRing(table)!, shapeRing(notch)!];
    final face = fillFace(rings, (300, 200));
    final same = sameRing(shapeRing(table)!, face.first);
    expect(same, isFalse);
  });
}

void notchCase(String name, Shape outer, Shape notch, (double, double) at) {
  test(name, () {
    final face = fillFace([shapeRing(outer)!, shapeRing(notch)!], at);
    expect(sameRing(shapeRing(outer)!, face.first), isFalse);
  });
}

void moreNotches() {
  const table = Shape(id: 't', type: Shape.rect, x: 0, y: 0, w: 600, h: 400);
  notchCase('8 mm square straddling an edge', table,
      const Shape(id: 'n', type: Shape.rect, x: 296, y: -4, w: 8, h: 8), (300, 200));
  notchCase('12 mm square on a corner', table,
      const Shape(id: 'n', type: Shape.rect, x: 594, y: 394, w: 12, h: 12), (300, 200));
  notchCase('22 mm square on a full sheet', const Shape(id: 's', type: Shape.rect, x: 0, y: 0, w: 2400, h: 1200),
      const Shape(id: 'n', type: Shape.rect, x: 2389, y: 1189, w: 22, h: 22), (1200, 600));
  notchCase('10 mm circle on an edge', table,
      const Shape(id: 'n', type: Shape.circle, x: 300, y: 0, r: 5), (300, 200));
  test('a circle is still the whole circle', () {
    const c = Shape(id: 'c', type: Shape.circle, x: 50, y: 50, r: 40);
    final face = fillFace([shapeRing(c)!], (50, 50));
    expect(sameRing(shapeRing(c)!, face.first), isTrue);
  });
  test('the plain table is still the whole table', () {
    final face = fillFace([shapeRing(table)!], (300, 200));
    expect(sameRing(shapeRing(table)!, face.first), isTrue);
  });
}
