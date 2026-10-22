import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/ui/main_window.dart';

void main() {
  test('the version shown is the one built', () {
    final m = RegExp(r'^version:\s*([0-9.]+)', multiLine: true).firstMatch(File('pubspec.yaml').readAsStringSync());
    expect(m, isNotNull);
    expect(appVersion, m!.group(1));
  });
}
