// A shape's own near-touching tips are joined, and a tap stays fast on big designs.
import 'dart:math' as math;
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/fill.dart';
import 'package:phobicccp/model.dart';

double net(List<List<Pt>> rs) => rs.isEmpty ? 0 : rs.skip(1).fold(ringArea(rs.first).abs(), (a, r) => a - ringArea(r).abs());
List<Pt> rect(double x, double y, double w, double h) => [(x, y), (x + w, y), (x + w, y + h), (x, y + h)];

// A horseshoe (closed path) whose tips are `gap` apart: a 6x6 room inside a 10x10 block.
List<Pt> horseshoe(double gap) {
  final g = gap / 2;
  return [for (final p in <Pt>[(0, 0), (10, 0), (10, 10), (5 + g, 10), (5 + g, 8), (8, 8), (8, 2), (2, 2), (2, 8), (5 - g, 8), (5 - g, 10), (0, 10)]) (p.$1 + 20, p.$2 + 20)];
}

// An "O" as one keyhole ring (outer circle, slit `gap` wide, inner circle back).
List<Pt> keyhole(double gap) {
  const R = 10.0, r = 6.0, n = 200;
  final ao = math.asin(gap / 2 / R), ai = math.asin(gap / 2 / r);
  return [
    for (var i = 0; i <= n; i++) (50 + R * math.cos(ao + (2 * math.pi - 2 * ao) * i / n), 50 + R * math.sin(ao + (2 * math.pi - 2 * ao) * i / n)),
    for (var i = 0; i <= n; i++) (50 + r * math.cos(2 * math.pi - ai - (2 * math.pi - 2 * ai) * i / n), 50 + r * math.sin(2 * math.pi - ai - (2 * math.pi - 2 * ai) * i / n)),
  ];
}

void main() {
  test('tap inside a horseshoe whose tips are 0.01 mm apart: the room (desktop 36, r21 pad 36)', () {
    expect(net(fillFace([rect(0, 0, 100, 100), horseshoe(0.01)], (25, 25))), closeTo(36, 0.1));
  });
  test('tap in the counter of a keyhole O with a 0.005 mm slit: the counter (desktop 113.0, r21 pad 113.1)', () {
    expect(net(fillFace([rect(0, 0, 100, 100), keyhole(0.005)], (50, 50))), closeTo(113.1, 0.2));
  });
  test('tap in the panel around that O: the O and its counter are a hole (desktop 10000-314)', () {
    expect(net(fillFace([rect(0, 0, 100, 100), keyhole(0.005)], (10, 10))), closeTo(10000 - 314.1, 0.5));
  });
  test('60 circles r=110 (720 points each): tap answers in under 2 s (r21 pad 1.5 s)', () {
    final rings = [for (var i = 0; i < 60; i++) shapeRing(Shape(id: 'c$i', type: Shape.circle, x: (i % 10) * 250.0, y: (i ~/ 10) * 250.0, r: 110))!];
    final sw = Stopwatch()..start();
    fillFace(rings, (0, 0));
    expect(sw.elapsedMilliseconds, lessThan(2000));
  });
}
