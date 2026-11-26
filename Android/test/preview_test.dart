// The Toolpaths panel's Preview: the computer's answer read, and asked for.

import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/net.dart';
import 'package:phobicccp/ui/preview_page.dart';

const _png = [0x89, 0x50, 0x4E, 0x47, 1, 2, 3];

void main() {
  test('a preview answer is read whole', () {
    final r = PreviewResult.fromJson({
      'ok': true,
      'png': base64Encode(_png),
      'seconds': 245.6,
      'perToolpath': [
        {'name': 'Pocket', 'seconds': 200},
        {'name': 'Outline', 'seconds': 45.4},
        'junk',
      ],
      'notes': ['left out: Hole (no shapes)', 3],
    });
    expect(r.png, _png);
    expect(r.seconds, 246);
    expect(r.perToolpath, [('Pocket', 200), ('Outline', 45)]);
    expect(r.notes, ['left out: Hole (no shapes)']);
  });

  test('a broken answer reads as empty, not as a crash', () {
    final r = PreviewResult.fromJson({'png': 'not base64 !!', 'perToolpath': 5, 'notes': 'x'});
    expect(r.png, isEmpty);
    expect(r.seconds, 0);
    expect(r.perToolpath, isEmpty);
    expect(r.notes, isEmpty);
  });

  test('times in plain words', () {
    expect(previewTime(12), '12 s');
    expect(previewTime(245), '4 min 05 s');
    expect(previewTime(4320), '1 h 12 min');
  });

  test('preview sends the design and reads the picture', () async {
    final server = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
    final seen = <Map<String, Object?>>[];
    server.listen((s) {
      var hello = false;
      s.cast<List<int>>().transform(utf8.decoder).transform(const LineSplitter()).listen((line) {
        if (!hello) {
          hello = line == 'PHOBICCCP-API 1';
          return;
        }
        final req = jsonDecode(line) as Map<String, Object?>;
        seen.add(req);
        s.write('${jsonEncode(req['op'] == 'preview' ? {'ok': true, 'png': base64Encode(_png), 'seconds': 60} : {'ok': false, 'error': 'too big'})}\n');
      });
    });
    final r = await CccpConnection.use('127.0.0.1', server.port, (c) => c.preview(Design(id: 'a', name: 'A', modified: 6)));
    expect(r.seconds, 60);
    expect(r.png, _png);
    expect(seen.single['op'], 'preview');
    expect((seen.single['design'] as Map)['id'], 'a');
    await server.close();
  });
}
