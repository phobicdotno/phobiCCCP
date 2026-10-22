// A circle touching a square's sides: the Area tool fills the piece tapped, as
// the desktop's vec::fillFace does, not every piece joined through the
// slits a flattened circle leaves at the touching points.
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/fill.dart';
import 'package:phobicccp/model.dart';

double area(List<List<Pt>> rings) => rings.fold(0.0, (a, r) => a + ringArea(r)).abs();

void main() {
  test('circle inscribed in a square: one corner', () {
    final sq = shapeRing(const Shape(id: 'a', type: Shape.rect, x: 0, y: 0, w: 10, h: 10))!;
    final c = shapeRing(const Shape(id: 'b', type: Shape.circle, x: 5, y: 5, r: 5))!;
    final f = fillFace([sq, c], (0.5, 0.5));

    expect(area(f), closeTo(5.365, 0.05));
  });
  test('circle in the corner of a panel: the nook', () {
    final sq = shapeRing(const Shape(id: 'a', type: Shape.rect, x: 0, y: 0, w: 50, h: 30))!;
    final c = shapeRing(const Shape(id: 'b', type: Shape.circle, x: 5, y: 5, r: 5))!;
    final f = fillFace([sq, c], (0.4, 0.4));
    expect(area(f), closeTo(5.365, 0.05));
  });
}
