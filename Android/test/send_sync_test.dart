import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/net.dart';
import 'package:phobicccp/store.dart';
import 'package:phobicccp/sync.dart';

/// A minimal phobiCCCP that keeps designs and forks like Store::put, and
/// answers a send as MainWindow::padSend does ([sendAnswersPut]: with
/// `modified` and `forked`; without, as an older phobiCCCP).
class FakeDesk {
  final kept = <String, Map<String, Object?>>{};
  final forks = <String>[];
  late ServerSocket server;

  static Future<FakeDesk> start({bool sendAnswersPut = true}) async {
    final f = FakeDesk();
    f.server = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
    f.server.listen((s) {
      var hello = false;
      s.cast<List<int>>().transform(utf8.decoder).transform(const LineSplitter()).listen((line) {
        if (!hello) {
          hello = true;
          return;
        }
        s.write('${jsonEncode(f._answer(jsonDecode(line) as Map<String, Object?>, sendAnswersPut))}\n');
      });
    });
    return f;
  }

  Map<String, Object?> _answer(Map<String, Object?> req, bool sendAnswersPut) {
    switch (req['op']) {
      case 'info':
        return {'ok': true, 'host': 'shop'};
      case 'designs':
        return {
          'ok': true,
          'designs': [for (final d in kept.values) {'id': d['id'], 'modified': d['modified']}]
        };
      case 'get':
        return {'ok': true, 'design': kept[req['id']]};
      case 'put':
      case 'send':
        final d = Map<String, Object?>.from(req['design'] as Map);
        final base = (d.remove('base') as num?)?.toInt() ?? 0;
        final id = d['id'] as String;
        final have = kept[id];
        final hm = (have?['modified'] as num?)?.toInt() ?? -1;
        final m = (d['modified'] as num).toInt();
        final put = <String, Object?>{'ok': true};
        if (have != null && base > 0 && hm > base && hm != m) {
          final f = 'fork-$id-$m-$base';
          if (!kept.containsKey(f)) {
            forks.add(f);
            kept[f] = {...d, 'id': f, 'name': '${d['name']} (pad)'};
          }
          put.addAll({'stored': true, 'forked': f, 'modified': hm});
        } else if (have == null || hm < m) {
          kept[id] = d;
          put.addAll({'stored': true, 'modified': m});
        } else {
          put.addAll({'stored': false, 'modified': hm});
        }
        if (req['op'] == 'put') return put;
        return {
          'ok': true,
          'started': false,
          'message': 'opened',
          'notes': <String>[],
          if (sendAnswersPut) 'modified': put['modified'],
          if (sendAnswersPut && put.containsKey('forked')) 'forked': put['forked'],
        };
      default:
        return {'ok': false, 'error': '?'};
    }
  }

  Desktop get desktop => Desktop(id: 'x', host: 'shop', address: '127.0.0.1', port: server.port);
}

/// As AppState.send: the send, then what it did with the copy.
Future<SendResult> send(FakeDesk desk, DesignStore store, Design d) =>
    CccpConnection.use('127.0.0.1', desk.server.port, (c) async {
      final r = await c.send(d, machine: '', start: false, from: 'pad');
      await settleSend(c, store, d, r, 'shop');
      return r;
    });

void main() {
  for (final answersPut in [true, false]) {
    test('edit, send right away, edit again: no fork (send answers put: $answersPut)', () async {
      final desk = await FakeDesk.start(sendAnswersPut: answersPut);
      final d0 = Design(id: 'a1', name: 'Sign', modified: 1000);
      desk.kept['a1'] = d0.toJson();
      final dir = Directory.systemTemp.createTempSync('pad');
      final store = DesignStore(dir)..put(d0.copyWith(base: 1000));

      final edited = store.save(store.get('a1')!.copyWith(name: 'Sign v2'), now: 2000);
      await send(desk, store, edited);
      await syncWith(desk.desktop, store, withTools: false);
      expect(store.get('a1')!.base, 2000);
      store.save(store.get('a1')!.copyWith(name: 'Sign v3'), now: 3000);
      final r = await syncWith(desk.desktop, store, withTools: false);
      expect(desk.forks, isEmpty);
      expect(r.conflicts, isEmpty);
      expect(store.get('a1')!.name, 'Sign v3');
      expect(desk.kept['a1']!['name'], 'Sign v3');
      await desk.server.close();
      dir.deleteSync(recursive: true);
    });
  }

  test('a send that runs into an edit made there: one fork, and the pad takes the copy there', () async {
    final desk = await FakeDesk.start();
    final d0 = Design(id: 'a1', name: 'Sign', modified: 1000);
    final dir = Directory.systemTemp.createTempSync('pad');
    final store = DesignStore(dir)..put(d0.copyWith(base: 1000));
    desk.kept['a1'] = d0.copyWith(name: 'Desk edit', modified: 2000).toJson();

    final edited = store.save(store.get('a1')!.copyWith(name: 'Pad edit'), now: 3000);
    final r = await send(desk, store, edited);
    expect(r.forked, isNotEmpty);
    expect(store.get('a1')!.name, 'Desk edit');
    expect(store.get('a1')!.base, 2000);
    await syncWith(desk.desktop, store, withTools: false);
    await syncWith(desk.desktop, store, withTools: false);
    expect(desk.forks.length, 1);
    expect(store.get(r.forked)!.name, 'Pad edit (pad)');
    await desk.server.close();
    dir.deleteSync(recursive: true);
  });
}
