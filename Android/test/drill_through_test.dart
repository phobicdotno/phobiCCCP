// A new Drill toolpath goes straight through the stock and 0.5 mm on, as
// the computer's makeToolpath() starts it.

import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/ui/panels.dart';

void main() {
  test('a new drill goes through the stock + 0.5 mm', () {
    expect(newToolpathDepth('drill', 18), 18.5);
    expect(newToolpathDepth('drill', 12.7), closeTo(13.2, 1e-9));
  });
  test('the other types keep their depths', () {
    expect(newToolpathDepth('profile', 18), 2.54);
    expect(newToolpathDepth('pocket', 18), 2.54);
    expect(newToolpathDepth('engrave', 18), 0.5);
  });
}
