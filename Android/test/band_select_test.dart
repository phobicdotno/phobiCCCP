// The select band, as the desktop canvas (tests/test_canvas.cpp checks the
// same cases): left to right selects what lies wholly inside, right to left
// whatever line the band touches.

import 'dart:ui';

import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/ui/canvas.dart';

void main() {
  final d = Design(shapes: const [
    Shape(id: 'big', type: Shape.rect, x: 0, y: 0, w: 100, h: 100),
    Shape(id: 'small', type: Shape.circle, x: 20, y: 20, r: 5),
    Shape(id: 'line', type: Shape.path, points: [[60, 10], [90, 10]], closed: false),
    Shape(id: 'dot', type: Shape.point, x: 70, y: 70),
  ]);

  test('window: only what lies wholly inside', () {
    expect(bandSelect(d, const Rect.fromLTRB(10, 5, 95, 80), crossing: false), {'small', 'line', 'dot'});
    expect(bandSelect(d, const Rect.fromLTRB(10, 5, 80, 80), crossing: false), {'small', 'dot'});
    expect(bandSelect(d, const Rect.fromLTRB(-5, -5, 105, 105), crossing: false), {'big', 'small', 'line', 'dot'});
  });

  test('crossing: whatever line it touches, not the inside of a closed shape', () {
    // Inside the big square, crossing the line and the circle only.
    expect(bandSelect(d, const Rect.fromLTRB(18, 5, 80, 16), crossing: true), {'small', 'line'});
    // Over the big square's left edge.
    expect(bandSelect(d, const Rect.fromLTRB(-5, 40, 5, 50), crossing: true), {'big'});
    // Inside the big square touching nothing.
    expect(bandSelect(d, const Rect.fromLTRB(40, 40, 50, 50), crossing: true), isEmpty);
    // Over the point.
    expect(bandSelect(d, const Rect.fromLTRB(68, 68, 72, 72), crossing: true), {'dot'});
  });
}
