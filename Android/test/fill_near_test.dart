// Outlines running within microns of each other (near-copies, a dense path
// crossed at a shallow angle): the Area fill still finds the face tapped.
import 'dart:math' as math;
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/fill.dart';
import 'package:phobicccp/model.dart';

double net(List<List<Pt>> rs) => rs.isEmpty ? 0 : rs.skip(1).fold(ringArea(rs.first).abs(), (a, r) => a - ringArea(r).abs());

void main() {
  test('two circles r=10, centres 0.001 mm apart: tap the middle', () {
    final a = shapeRing(const Shape(id: 'a', type: Shape.circle, x: 50, y: 50, r: 10))!;
    final b = shapeRing(const Shape(id: 'b', type: Shape.circle, x: 50.001, y: 50, r: 10))!;
    expect(net(fillFace([a, b], (50, 50))), closeTo(313.9, 0.5)); // desktop 313.9; r21 pad: nothing
  });
  test('circle r=40, copy 0.006 mm to the right', () {
    final a = shapeRing(const Shape(id: 'a', type: Shape.circle, x: 0, y: 0, r: 40))!;
    final b = shapeRing(const Shape(id: 'b', type: Shape.circle, x: 0.006, y: 0, r: 40))!;
    expect(net(fillFace([a, b], (0, 0))), closeTo(5025.2, 1));
  });
  test('closed path with 0.006 mm vertex spacing, crossed at a shallow angle', () {
    const n = 1000;
    final ring = [for (var i = 0; i < n; i++) (math.cos(2 * math.pi * i / n + 0.0001), math.sin(2 * math.pi * i / n + 0.0001))];
    final cap = <Pt>[(-3, 0.9), (3, 0.99), (3, 3), (-3, 3)];
    expect(net(fillFace([ring, cap], (-0.5, 0.1))), closeTo(3.108, 0.02)); // desktop 3.108; r21 pad 15.45
  });
}
