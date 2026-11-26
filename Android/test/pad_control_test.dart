// Hold, Resume and Stop from the pad: the request lines the computer gets,
// and the machine status read from its answer.

import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/net.dart';

import 'client_test.dart' show fakeDesktop;

void main() {
  test('hold, resume and stop requests', () async {
    final (server, seen) = await fakeDesktop((req) => switch (req['op']) {
          'hold' => '{"ok":true,"status":{"connected":true,"streaming":true,"held":true,"state":"Hold:0"}}',
          'stop' => '{"ok":true,"status":{"connected":true,"streaming":false,"state":"Idle","jobEnd":"stopped"}}',
          'resume' => '{"ok":false,"error":"resuming from a pad is off on shop (Machine > Pads may start cutting)"}',
          _ => '{"ok":false,"error":"unknown request"}',
        });
    final c = await CccpConnection.open('127.0.0.1', port: server.port);
    final held = await c.control('hold', pad: 'p1');
    expect(held.onHold, true);
    expect(held.jobRunning, true);
    await expectLater(c.control('resume', pad: 'p1'),
        throwsA(isA<CccpException>().having((e) => e.message, 'message', contains('Pads may start cutting'))));
    final stopped = await c.control('stop', pad: 'p1');
    expect(stopped.jobRunning, false);
    expect(stopped.jobEnd, 'stopped');
    c.close();
    expect(seen, [
      {'op': 'hold', 'pad': 'p1'},
      {'op': 'resume', 'pad': 'p1'},
      {'op': 'stop', 'pad': 'p1'},
    ]);
    await server.close();
  });

  test('status: held and running', () {
    expect(MachineStatus.fromJson({'connected': true, 'state': 'Hold:0'}).jobRunning, true);
    expect(MachineStatus.fromJson({'connected': true, 'state': 'Idle'}).jobRunning, false);
    expect(MachineStatus.fromJson({'connected': false, 'held': true}).jobRunning, false);
  });
}
