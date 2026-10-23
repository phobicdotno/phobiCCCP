// Area tool where outlines cross: a crossing rounded to the grid a unit off
// its edge bent the edge and reordered the edges round a corner, so a tap in
// one of two overlapping circles filled both (snap rounding now bends every
// edge through each corner and crossing it passes). Areas as the computer's
// Area tool gives them on the same outlines.
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/fill.dart';
import 'package:phobicccp/model.dart';

void main() {
  List<Pt> circle(double x, double y, double r) => shapeRing(Shape(id: 'c', type: Shape.circle, x: x, y: y, r: r))!;
  List<Pt> hex(double x, double y, double r) =>
      shapeRing(Shape(id: 'h', type: Shape.polygon, x: x, y: y, r: r, sides: 6))!;

  test('a tap in one of two overlapping circles fills its crescent, not both', () {
    final face = fillFace([circle(5, 5, 20), circle(35, 20, 15)], (42.4, 18.66));
    expect(face.length, 1);
    expect(ringArea(face.first).abs(), closeTo(696.95, 1));
  });

  test('the other circle and the lens of a larger pair', () {
    final a = circle(0, 0, 30), b = circle(35, 20, 20);
    final lens = fillFace([a, b], (22, 12));
    final crescent = fillFace([a, b], (-10, -5));
    expect(lens.length, 1);
    expect(crescent.length, 1);
    expect(ringArea(lens.first).abs() + ringArea(crescent.first).abs(), closeTo(ringArea(a).abs(), 0.01));
    expect(ringArea(lens.first).abs(), lessThan(ringArea(b).abs()));
  });

  test('a hexagon sharing a corner and an edge with another stays out of the face', () {
    final face = fillFace([hex(25, 25, 10), hex(5, 30, 15), hex(20, 25, 5)], (9.27, 40.74));
    expect(face.length, 1);
    expect(ringArea(face.first).abs(), closeTo(569.94, 1));
  });
}
