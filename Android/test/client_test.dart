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
    expect(seen.where((s) => s['op'] == 'put').single['design'], isA<Map>());
    await server.close();
    dir.deleteSync(recursive: true);
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
