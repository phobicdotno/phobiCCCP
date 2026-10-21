// Area fill on the pad against the computer's: shapes overlapping by microns
// (closing slits must not bend edges), and a framed window inside no shape.
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/fill.dart';
import 'package:phobicccp/model.dart';

double area(List<List<Pt>> rings) => rings.fold(0.0, (a, r) => a + ringArea(r)).abs();
List<Pt> rect(double x, double y, double w, double h) =>
    shapeRing(Shape(id: 'r', type: Shape.rect, x: x, y: y, w: w, h: h))!;

void main() {
  test('tap inside a 20 mm square with two rects overlapping its top by microns', () {
    final f = fillFace([rect(0, 0, 20, 20), rect(-2, 19.995, 20, 10), rect(-3, 19.993, 6, 7)], (10, 10));
    expect(area(f), closeTo(399.9, 0.1));
  });
  test('window of a frame of four overlapping bars: inside none, so nothing', () {
    final f = fillFace([rect(0, 0, 30, 5), rect(25, 0, 5, 30), rect(0, 25, 30, 5), rect(0, 0, 5, 30)], (15, 15));
    expect(f, isEmpty);
  });
  test('a square 0.012 mm above a panel edge joins it, as on the computer', () {
    // A diamond whose bottom corner stands 0.012 mm above the panel's bottom
    // edge: the slit under it is narrower than 0.02 mm, so the panel's area
    // around it is one face with the diamond cut out, not leaking under it.
    final panel = rect(0, 0, 50, 50);
    final diamond = <Pt>[(25, 0.012), (30, 5.012), (25, 10.012), (20, 5.012)];
    final f = fillFace([panel, diamond], (40, 40));
    expect(area(f), closeTo(2500 - 50, 0.5));
  });
}
