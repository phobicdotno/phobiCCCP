// Area tool: a 600 x 400 table top with a 20 x 20 square overlapping its
// corner (10 x 10 of it inside). A tap in the middle picks the table minus
// that corner, not the whole table (the desktop Area tool agrees).
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/fill.dart';
import 'package:phobicccp/model.dart';

void main() {
  test('notched face is not the whole rectangle', () {
    const table = Shape(id: 't', type: Shape.rect, x: 0, y: 0, w: 600, h: 400);
    const notch = Shape(id: 'n', type: Shape.rect, x: 590, y: 390, w: 20, h: 20);
    final rings = [shapeRing(table)!, shapeRing(notch)!];
    final face = fillFace(rings, (300, 200));
    final same = sameRing(shapeRing(table)!, face.first);
    expect(same, isFalse);
  });
}
