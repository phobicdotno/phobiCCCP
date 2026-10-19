// The link to phobiCCCP, version 1 (src/projectreceiver.h on the desktop):
// UDP discovery and one JSON request per line over TCP, both on port 47652.

import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'model.dart';

class Protocol {
  static const port = 47652;
  static const discover = 'PHOBICCCP-DISCOVER 1';
  static const apiHello = 'PHOBICCCP-API 1\n';
}

class CccpException implements Exception {
  final String message;
  CccpException(this.message);
  @override
  String toString() => message;
}

/// One connection to a phobiCCCP: "PHOBICCCP-API 1", then one JSON request per
/// line, each answered by one JSON line before the next is sent.
class CccpConnection {
  static const maxReply = 32 * 1024 * 1024;

  /// Someone at the computer may have to say whether to save their own work first.
  static const sendTimeout = Duration(seconds: 120);
  static const readTimeout = Duration(seconds: 15);

  final Socket _socket;
  final _buf = BytesBuilder(copy: false);
  final _lines = <String>[];
  Completer<String?>? _waiting;
  bool _closed = false;
  Future<void> _queue = Future.value();

  CccpConnection._(this._socket) {
    _socket.listen(_onData, onDone: _onDone, onError: (_) => _onDone(), cancelOnError: true);
  }

  static Future<CccpConnection> open(String host, {int port = Protocol.port, Duration timeout = const Duration(seconds: 4)}) async {
    final s = await Socket.connect(host, port, timeout: timeout);
    s.setOption(SocketOption.tcpNoDelay, true);
    s.add(utf8.encode(Protocol.apiHello));
    return CccpConnection._(s);
  }

  /// Open, run [body], close.
  static Future<T> use<T>(String host, int port, Future<T> Function(CccpConnection c) body) async {
    final c = await open(host, port: port);
    try {
      return await body(c);
    } finally {
      c.close();
    }
  }

  void _onData(Uint8List data) {
    var start = 0;
    for (var i = 0; i < data.length; i++) {
      if (data[i] == 10) {
        _buf.add(data.sublist(start, i));
        _push(utf8.decode(_buf.takeBytes(), allowMalformed: true));
        start = i + 1;
      }
    }
    if (start < data.length) _buf.add(data.sublist(start));
    if (_buf.length > maxReply) {
      _buf.clear();
      _onDone();
    }
  }

  void _push(String line) {
    final w = _waiting;
    if (w != null && !w.isCompleted) {
      _waiting = null;
      w.complete(line);
    } else {
      _lines.add(line);
    }
  }

  void _onDone() {
    _closed = true;
    final w = _waiting;
    _waiting = null;
    if (w != null && !w.isCompleted) w.complete(null);
  }

  /// Send one request and wait for its answer; requests on one connection go one at a time.
  Future<Map<String, Object?>> request(Map<String, Object?> req, {Duration timeout = readTimeout}) {
    final f = _queue.then((_) => _request(req, timeout));
    _queue = f.then((_) {}, onError: (_) {});
    return f;
  }

  Future<Map<String, Object?>> _request(Map<String, Object?> req, Duration timeout) async {
    if (_closed) throw CccpException('phobiCCCP closed the connection');
    _socket.add(utf8.encode('${jsonEncode(req)}\n'));
    String? line;
    if (_lines.isNotEmpty) {
      line = _lines.removeAt(0);
    } else {
      final w = Completer<String?>();
      _waiting = w;
      line = await w.future.timeout(timeout, onTimeout: () {
        _waiting = null;
        throw CccpException('phobiCCCP did not answer');
      });
    }
    if (line == null) throw CccpException('phobiCCCP closed the connection');
    final Object? reply;
    try {
      reply = jsonDecode(line);
    } on FormatException {
      throw CccpException('phobiCCCP sent something unreadable');
    }
    if (reply is! Map) throw CccpException('phobiCCCP sent something unreadable');
    final m = reply.cast<String, Object?>();
    if (m['ok'] != true) throw CccpException(m['error'] is String ? m['error'] as String : 'phobiCCCP said no');
    return m;
  }

  Future<DesktopInfo> info() async => DesktopInfo.fromJson(await request({'op': 'info'}));

  Future<ToolLibrary> tools() async => ToolLibrary.fromJson(await request({'op': 'tools'}));

  Future<List<DesignEntry>> designs() async {
    final r = await request({'op': 'designs'});
    final list = r['designs'];
    return list is List ? list.whereType<Map>().map((e) => DesignEntry.fromJson(e.cast<String, Object?>())).toList() : [];
  }

  Future<Design> get(String id) async {
    final d = (await request({'op': 'get', 'id': id}))['design'];
    if (d is! Map) throw CccpException('no design in the answer');
    return Design.fromJson(d.cast<String, Object?>());
  }

  /// True when phobiCCCP kept it (it had no copy, or an older one).
  Future<bool> put(Design d) async => (await request({'op': 'put', 'design': d.toJson()}))['stored'] == true;

  Future<DesktopInfo> selectMachine(String id) async => DesktopInfo.fromJson(await request({'op': 'machine', 'id': id}));

  /// Open [d] on that computer, on [machine] (its id; empty for the one in use),
  /// and start cutting it when [start] and the computer allows it.
  Future<SendResult> send(Design d, {required String machine, required bool start, required String from}) async =>
      SendResult.fromJson(await request(
          {'op': 'send', 'design': d.toJson(), 'machine': machine, 'start': start, 'from': from},
          timeout: sendTimeout));

  void close() {
    _closed = true;
    _socket.destroy();
  }
}

/// Finding phobiCCCP: "PHOBICCCP-DISCOVER 1" to port 47652 on every broadcast
/// address, to every address of this pad's own /24 one by one (iOS only lets
/// apps broadcast with a special entitlement, but a datagram to one address is
/// fine), and straight to any host typed in; then every answer for a moment.
class Discovery {
  static Future<List<Desktop>> probe({Iterable<String> manualHosts = const [], Duration wait = const Duration(milliseconds: 1500)}) async {
    final found = <String, Desktop>{};
    final typedByAddress = <String, String>{};
    for (final h in manualHosts) {
      try {
        final a = (await InternetAddress.lookup(h.trim(), type: InternetAddressType.IPv4)).first;
        typedByAddress[a.address] = h;
      } catch (_) {}
    }
    final sock = await RawDatagramSocket.bind(InternetAddress.anyIPv4, 0);
    try {
      sock.broadcastEnabled = true;
      sock.listen((e) {
        if (e != RawSocketEvent.read) return;
        Datagram? d;
        while ((d = sock.receive()) != null) {
          final addr = d!.address.address;
          final desk = parseAnswer(utf8.decode(d.data, allowMalformed: true), addr, typed: typedByAddress[addr] ?? '');
          if (desk != null) found[desk.id] = desk;
        }
      });
      final hello = utf8.encode(Protocol.discover);
      final targets = <String>{'255.255.255.255', ...typedByAddress.keys};
      for (final nif in await _interfaces()) {
        for (final a in nif.addresses) {
          if (a.type != InternetAddressType.IPv4 || a.isLoopback || a.isLinkLocal) continue;
          final parts = a.address.split('.');
          if (parts.length != 4) continue;
          final net = parts.take(3).join('.');
          targets.add('$net.255');
          for (var i = 1; i < 255; i++) {
            targets.add('$net.$i');
          }
        }
      }
      for (final t in targets) {
        try {
          sock.send(hello, InternetAddress(t), Protocol.port);
        } catch (_) {}
      }
      await Future<void>.delayed(wait);
    } finally {
      sock.close();
    }
    return found.values.toList();
  }

  static Future<List<NetworkInterface>> _interfaces() async {
    try {
      return await NetworkInterface.list(type: InternetAddressType.IPv4);
    } catch (_) {
      return [];
    }
  }

  /// One discovery answer, or null when it is not phobiCCCP's.
  static Desktop? parseAnswer(String text, String address, {String typed = '', int? now}) {
    Object? o;
    try {
      o = jsonDecode(text);
    } on FormatException {
      return null;
    }
    if (o is! Map || o['app'] != 'phobiCCCP') return null;
    String s(String k) => o is Map && o[k] is String ? o[k] as String : '';
    final host = s('host').isEmpty ? address : s('host');
    return Desktop(
      id: s('id').isEmpty ? '$host@$address' : s('id'),
      host: host,
      address: address,
      port: o['port'] is num ? (o['port'] as num).toInt() : Protocol.port,
      version: s('version'),
      machine: s('machine'),
      lastSeen: now ?? DateTime.now().millisecondsSinceEpoch,
      typed: typed,
    );
  }
}
