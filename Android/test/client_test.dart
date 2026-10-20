// The pad's side of the link against a stand-in for phobiCCCP on loopback.

import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/net.dart';
import 'package:phobicccp/store.dart';
import 'package:phobicccp/sync.dart';

Future<(ServerSocket, List<Map<String, Object?>>)> fakeDesktop(String Function(Map<String, Object?> req) answer) async {
  final server = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
  final seen = <Map<String, Object?>>[];
  server.listen((s) {
    var hello = false;
    s.cast<List<int>>().transform(utf8.decoder).transform(const LineSplitter()).listen((line) {
      if (!hello) {
        hello = line == 'PHOBICCCP-API 1';
        if (!hello) s.destroy();
        return;
      }
      final req = jsonDecode(line) as Map<String, Object?>;
      seen.add(req);
      s.write('${answer(req)}\n');
    });
  });
  return (server, seen);
}

void main() {
  test('requests and answers', () async {
    final (server, seen) = await fakeDesktop((req) => switch (req['op']) {
          'info' => '{"ok":true,"host":"shop","id":"i1","activeMachine":"m1","allowStart":true,'
              '"machines":[{"id":"m1","name":"Shapeoko","active":true},{"id":"m2","name":"Router"}],'
              '"status":{"connected":true,"state":"Idle","homed":true,"busy":false}}',
          'designs' => '{"ok":true,"designs":[{"id":"a","name":"A","modified":5,"deleted":false}]}',
          'put' => '{"ok":true,"stored":true}',
          'send' => '{"ok":true,"started":true,"message":"opened A.c2d on shop, cutting started","notes":[]}',
          _ => '{"ok":false,"error":"unknown request"}',
        });
    final c = await CccpConnection.open('127.0.0.1', port: server.port);
    final info = await c.info();
    expect(info.host, 'shop');
    expect(info.machines.length, 2);
    expect(info.status.readyToCut, true);
    // Several requests at once still go one at a time, in order.
    final both = await Future.wait([c.designs(), c.put(Design(id: 'a', name: 'A', modified: 6))]);
    expect((both[0] as List<DesignEntry>).single.modified, 5);
    expect(both[1], true);
    final r = await c.send(Design(id: 'a', name: 'A', modified: 6), machine: 'm2', start: true, from: 'iPad');
    expect(r.started, true);
    await expectLater(c.tools(), throwsA(isA<CccpException>().having((e) => e.message, 'message', 'unknown request')));
    c.close();
    final send = seen.firstWhere((s) => s['op'] == 'send');
    expect(send['machine'], 'm2');
    expect(send['start'], true);
    expect((send['design'] as Map)['id'], 'a');
    await server.close();
  });

  test('a sync round pulls the newer and pushes the newer', () async {
    final remote = Design(id: 'r1', name: 'From the PC', modified: 50);
    final (server, seen) = await fakeDesktop((req) => switch (req['op']) {
          'info' => '{"ok":true,"host":"shop"}',
          'tools' => '{"ok":true,"materials":[],"tools":[{"number":201,"name":"End mill","materials":["softwood"]}]}',
          'designs' => '{"ok":true,"designs":[{"id":"r1","modified":50},{"id":"l1","modified":1}]}',
          'get' => jsonEncode({'ok': true, 'design': remote.toJson()}),
          'put' => '{"ok":true,"stored":true}',
          _ => '{"ok":false,"error":"?"}',
        });
    final dir = Directory.systemTemp.createTempSync('pad');
    final store = DesignStore(dir)..put(Design(id: 'l1', name: 'Mine', modified: 9));
    final r = await syncWith(Desktop(id: 'x', host: 'shop', address: '127.0.0.1', port: server.port), store);
    expect(r.pulled, 1);
    expect(r.pushed, 1);
    expect(r.tools!.tool(201)!.name, 'End mill');
    expect(store.get('r1')!.name, 'From the PC');
    expect(store.get('r1')!.base, 50, reason: 'a pulled copy is the base of edits made on it');
    expect(store.get('l1')!.baseFor('x'), 9, reason: 'so is a copy the computer kept');
    expect(seen.where((s) => s['op'] == 'put').single['design'], isA<Map>());
    await server.close();
    dir.deleteSync(recursive: true);
  });

  test('a design sent from the computer comes with the sync, and the computer hears it opened', () async {
    final sent = Design(id: 'd1', name: 'Sign', modified: 40);
    final (server, seen) = await fakeDesktop((req) => switch (req['op']) {
          'info' => '{"ok":true,"host":"shop","open":{"seq":7,"design":"d1","name":"Sign","from":"shop"}}',
          'designs' => '{"ok":true,"designs":[{"id":"d1","modified":40}]}',
          'get' => jsonEncode({'ok': true, 'design': sent.toJson()}),
          'opened' => '{"ok":true}',
          _ => '{"ok":false,"error":"?"}',
        });
    final dir = Directory.systemTemp.createTempSync('pad');
    final store = DesignStore(dir);
    final desk = Desktop(id: 'x', host: 'shop', address: '127.0.0.1', port: server.port);
    final r = await syncWith(desk, store, withTools: false, pad: {'id': 'p1', 'name': 'Tab'});
    expect((seen.first['pad'] as Map)['id'], 'p1', reason: 'the pad says who it is');
    expect(r.open?.design, 'd1');
    expect(store.get('d1')!.name, 'Sign', reason: 'pulled before it is opened');
    final ack = seen.lastWhere((s) => s['op'] == 'opened');
    expect(ack['seq'], 7);
    expect(ack['pad'], 'p1');
    // Its answer lost: asked again, said again, but not opened again.
    final again = await syncWith(desk, store, withTools: false, pad: {'id': 'p1', 'name': 'Tab'}, handled: 7);
    expect(again.open, isNull);
    expect(seen.where((s) => s['op'] == 'opened').length, 2);
    // Without a pad id (an older call), nothing is opened or said.
    final none = await syncWith(desk, store, withTools: false);
    expect(none.open, isNull);
    expect(seen.where((s) => s['op'] == 'opened').length, 2);
    await server.close();
    dir.deleteSync(recursive: true);
  });

  test('a design sent that could not be pulled is not opened, nor said to be', () async {
    final (server, seen) = await fakeDesktop((req) => switch (req['op']) {
          'info' => '{"ok":true,"host":"shop","open":{"seq":3,"design":"gone"}}',
          'designs' => '{"ok":true,"designs":[]}',
          _ => '{"ok":false,"error":"?"}',
        });
    final dir = Directory.systemTemp.createTempSync('pad');
    final r = await syncWith(Desktop(id: 'x', host: 'shop', address: '127.0.0.1', port: server.port), DesignStore(dir),
        withTools: false, pad: {'id': 'p1', 'name': 'Tab'});
    expect(r.open, isNull);
    expect(seen.where((s) => s['op'] == 'opened'), isEmpty);
    await server.close();
    dir.deleteSync(recursive: true);
  });

  test('one design the computer turns down does not hold back the rest', () async {
    final (server, seen) = await fakeDesktop((req) => switch (req['op']) {
          'info' => '{"ok":true,"host":"shop"}',
          'designs' => '{"ok":true,"designs":[]}',
          'put' => (req['design'] as Map)['id'] == 'a1'
              ? '{"ok":false,"error":"shape s1: text too long"}'
              : '{"ok":true,"stored":true}',
          _ => '{"ok":false,"error":"?"}',
        });
    final dir = Directory.systemTemp.createTempSync('pad');
    final store = DesignStore(dir)
      ..put(Design(id: 'a1', name: 'Long', modified: 9))
      ..put(Design(id: 'b1', name: 'Fine', modified: 9));
    final r = await syncWith(Desktop(id: 'x', host: 'shop', address: '127.0.0.1', port: server.port), store,
        withTools: false);
    expect(r.pushed, 1);
    expect(r.refused.single, contains('text too long'));
    expect(seen.where((s) => s['op'] == 'put').length, 2);
    await server.close();
    dir.deleteSync(recursive: true);
  });

  test('an edit of a copy that changed on the computer is kept there as a new design', () async {
    final theirs = Design(id: 'a1', name: 'Desk edit', modified: 15);
    final (server, seen) = await fakeDesktop((req) => switch (req['op']) {
          'info' => '{"ok":true,"host":"shop"}',
          'designs' => '{"ok":true,"designs":[{"id":"a1","modified":15}]}',
          'get' => jsonEncode({'ok': true, 'design': theirs.toJson()}),
          'put' => '{"ok":true,"stored":true,"forked":"f1","modified":15}',
          _ => '{"ok":false,"error":"?"}',
        });
    final dir = Directory.systemTemp.createTempSync('pad');
    // Pulled at 10, edited here at 20; the computer changed it at 15.
    final store = DesignStore(dir)..put(Design(id: 'a1', name: 'Pad edit', modified: 20, base: 10));
    final r = await syncWith(Desktop(id: 'x', host: 'shop', address: '127.0.0.1', port: server.port), store, withTools: false);
    final put = seen.where((s) => s['op'] == 'put').single['design'] as Map;
    expect(put['base'], 10, reason: 'the pad says which copy it edited');
    final now = store.get('a1')!;
    expect(now.name, 'Desk edit', reason: "the computer's copy replaces ours, though older");
    expect(now.modified, 15);
    expect(now.base, 15);
    expect(r.conflicts.single, contains('Pad edit (pad)'));
    // The next round has nothing to send: the fork comes back by pull.
    expect(SyncPlan.of({'a1': now.modified}, const [DesignEntry('a1', modified: 15), DesignEntry('f1', modified: 20)]).push, isEmpty);
    await server.close();
    dir.deleteSync(recursive: true);
  });

  test('an edit made here is offered before a newer copy is pulled over it', () async {
    final theirs = Design(id: 'a1', name: 'Desk edit', modified: 30);
    final (server, seen) = await fakeDesktop((req) => switch (req['op']) {
          'info' => '{"ok":true,"host":"shop"}',
          'designs' => '{"ok":true,"designs":[{"id":"a1","modified":30}]}',
          'get' => jsonEncode({'ok': true, 'design': theirs.toJson()}),
          'put' => '{"ok":true,"stored":true,"forked":"f1","modified":30}',
          _ => '{"ok":false,"error":"?"}',
        });
    final dir = Directory.systemTemp.createTempSync('pad');
    final store = DesignStore(dir)..put(Design(id: 'a1', name: 'Pad edit', modified: 12, base: 10));
    final r = await syncWith(Desktop(id: 'x', host: 'shop', address: '127.0.0.1', port: server.port), store, withTools: false);
    expect(seen.map((s) => s['op']), containsAllInOrder(['put', 'get']));
    expect(store.get('a1')!.name, 'Desk edit');
    expect(r.conflicts, hasLength(1));
    await server.close();
    dir.deleteSync(recursive: true);
  });

  test('a deletion the computer refuses brings the design back', () async {
    final theirs = Design(id: 'a1', name: 'Kept', modified: 25);
    final (server, _) = await fakeDesktop((req) => switch (req['op']) {
          'info' => '{"ok":true,"host":"shop"}',
          'designs' => '{"ok":true,"designs":[{"id":"a1","modified":25}]}',
          'get' => jsonEncode({'ok': true, 'design': theirs.toJson()}),
          'put' => '{"ok":true,"stored":false,"conflict":true,"modified":25}',
          _ => '{"ok":false,"error":"?"}',
        });
    final dir = Directory.systemTemp.createTempSync('pad');
    final store = DesignStore(dir)..put(Design(id: 'a1', name: 'Kept', modified: 40, base: 10, deleted: true));
    final r = await syncWith(Desktop(id: 'x', host: 'shop', address: '127.0.0.1', port: server.port), store, withTools: false);
    expect(store.get('a1')!.deleted, false);
    expect(r.conflicts.single, contains('not deleted'));
    await server.close();
    dir.deleteSync(recursive: true);
  });

  test('the base moves on only to a copy the computer has', () async {
    var answer = '{"ok":true,"stored":false,"modified":9}';
    final (server, _) = await fakeDesktop((req) => switch (req['op']) {
          'info' => '{"ok":true,"host":"shop"}',
          'designs' => '{"ok":true,"designs":[]}',
          'put' => answer,
          _ => '{"ok":false,"error":"?"}',
        });
    final desk = Desktop(id: 'x', host: 'shop', address: '127.0.0.1', port: server.port);
    final dir = Directory.systemTemp.createTempSync('pad');
    final store = DesignStore(dir)..put(Design(id: 'a1', modified: 9, base: 5));
    await syncWith(desk, store, withTools: false);
    expect(store.get('a1')!.baseFor('x'), 9, reason: 'it already had this copy');
    // A phobiCCCP that does not say which copy it has, and did not keep ours.
    store.put(Design(id: 'a1', modified: 12, base: 9));
    answer = '{"ok":true,"stored":false}';
    await syncWith(desk, store, withTools: false);
    expect(store.get('a1')!.baseFor('x'), 9);
    await server.close();
    dir.deleteSync(recursive: true);
  });

  test('a busy computer says so', () async {
    final server = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
    server.listen((s) {
      s.write('ERR busy\n');
      s.close();
    });
    final c = await CccpConnection.open('127.0.0.1', port: server.port);
    await expectLater(c.info(), throwsA(isA<CccpException>().having((e) => e.message, 'message', contains('busy'))));
    c.close();
    await server.close();
  });

  test('a computer that is not there fails cleanly', () async {
    final s = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
    final port = s.port;
    await s.close();
    await expectLater(CccpConnection.open('127.0.0.1', port: port), throwsA(isA<SocketException>()));
  });

  test('discovery answers', () {
    final d = Discovery.parseAnswer('{"app":"phobiCCCP","host":"shop","port":47652,"id":"i1","machine":"Shapeoko","version":"1.1.21"}',
        '192.168.1.5', now: 1000)!;
    expect(d.id, 'i1');
    expect(d.machine, 'Shapeoko');
    expect(d.online(2000), true);
    expect(d.online(1000 + 60000), false);
    // An older phobiCCCP without an id is still found, keyed by name and address.
    expect(Discovery.parseAnswer('{"app":"phobiCCCP","host":"pc","port":47652}', '10.0.0.2')!.id, 'pc@10.0.0.2');
    expect(Discovery.parseAnswer('{"app":"other"}', '10.0.0.2'), isNull);
    expect(Discovery.parseAnswer('not json', '10.0.0.2'), isNull);
  });
}
