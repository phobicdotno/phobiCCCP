import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/store.dart';
import 'package:phobicccp/sync.dart';

/// A phobiCCCP whose put is refused as MainWindow::onPadRequest refuses it
/// while another send waits on a question there (m_linkBusy).
void main() {
  test('pad offline edit survives a sync while the desktop is busy', () async {
    final kept = <String, Map<String, Object?>>{};
    final server = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
    server.listen((s) {
      var hello = false;
      s.cast<List<int>>().transform(utf8.decoder).transform(const LineSplitter()).listen((line) {
        if (!hello) { hello = true; return; }
        final req = jsonDecode(line) as Map<String, Object?>;
        Map<String, Object?> a;
        switch (req['op']) {
          case 'info': a = {'ok': true, 'host': 'shop'}; break;
          case 'designs': a = {'ok': true, 'designs': [for (final d in kept.values) {'id': d['id'], 'modified': d['modified']}]}; break;
          case 'get': a = {'ok': true, 'design': kept[req['id']]}; break;
          case 'put': a = {'ok': false, 'error': 'shop is busy with another design; try again shortly'}; break;
          default: a = {'ok': false, 'error': '?'};
        }
        s.write('${jsonEncode(a)}\n');
      });
    });
    final dir = Directory.systemTemp.createTempSync('pad');
    final store = DesignStore(dir);
    final d0 = Design(id: 'a1', name: 'Sign', modified: 1000);
    store.put(d0.copyWith(base: 1000));
    // Edited on the pad while away...
    store.save(store.get('a1')!.copyWith(name: 'Pad edit'), now: 2000);
    // ...and on the desktop.
    kept['a1'] = d0.copyWith(name: 'Desk edit', modified: 3000).toJson();
    final desk = Desktop(id: 'x', host: 'shop', address: '127.0.0.1', port: server.port);
    final r = await syncWith(desk, store, withTools: false);
    // ignore: avoid_print
    print('refused=${r.refused} pulled=${r.pulled} pad copy now: ${store.get('a1')!.name} base ${store.get('a1')!.base}');
    expect(store.get('a1')!.name, 'Pad edit', reason: 'the pad edit was thrown away');
    await server.close();
  });
}
