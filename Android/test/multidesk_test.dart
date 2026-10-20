// Several pads and several phobiCCCPs that never talk to each other directly
// (round 2): with one base for all computers, an edit carried to one computer
// could be written over there without a fork by a pad whose base came from
// another. Version vectors tell a copy made from another from two edited
// apart, wherever they went, and every computer keeps both the same way.

import 'dart:convert';
import 'dart:io';
import 'dart:math';

import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/store.dart';
import 'package:phobicccp/sync.dart';

/// A phobiCCCP that keeps designs as Store::put does (src/padlink.cpp): a copy
/// made from the kept one replaces it; two edited apart are both kept, the
/// later under the id and the other as a fork whose id depends only on it; a
/// deletion that loses is dropped. Without vvs, the base rules.
class VvDesk {
  final String name;
  final kept = <String, Map<String, Object?>>{};
  late ServerSocket server;
  VvDesk(this.name);

  static Future<VvDesk> start(String name) async {
    final f = VvDesk(name);
    f.server = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
    f.server.listen((s) {
      var hello = false;
      s.cast<List<int>>().transform(utf8.decoder).transform(const LineSplitter()).listen((line) {
        if (!hello) {
          hello = true;
          return;
        }
        s.write('${jsonEncode(f._answer(jsonDecode(line) as Map<String, Object?>))}\n');
      });
    });
    return f;
  }

  Desktop get desktop => Desktop(id: '$name-id', host: name, address: '127.0.0.1', port: server.port);

  static Map<String, int> _vv(Map<String, Object?> d) =>
      {for (final e in ((d['vv'] as Map?) ?? const {}).entries) e.key as String: (e.value as num).toInt()};
  static int _m(Map<String, Object?> d) => (d['modified'] as num).toInt();
  static String _key(Map<String, Object?> d) {
    final vv = _vv(d);
    return (vv.keys.toList()..sort()).map((k) => '$k=${vv[k]};').join();
  }

  Map<String, Object?> put(Map<String, Object?> design) {
    final d = Map<String, Object?>.from(design);
    final base = (d.remove('base') as num?)?.toInt() ?? 0;
    final id = d['id'] as String;
    final have = kept[id];
    if (have == null) {
      kept[id] = d;
      return {'stored': true};
    }
    switch (orderOf(_vv(d), _m(d), _vv(have), _m(have))) {
      case Order.same:
      case Order.before:
        return {'stored': false};
      case Order.after:
        kept[id] = d;
        return {'stored': true};
      case Order.concurrent:
        final moved = _m(d) != _m(have) ? _m(d) > _m(have) : _key(d).compareTo(_key(have)) > 0;
        final loser = moved ? have : d;
        if (loser['deleted'] == true) {
          if (moved) kept[id] = d;
          return moved ? {'stored': true} : {'stored': false, 'conflict': true};
        }
        final fork = 'f-${_m(loser)}-${_key(loser).hashCode.toUnsigned(32)}';
        kept.putIfAbsent(fork, () => {...loser, 'id': fork, 'name': '${loser['name']} (conflict)'});
        if (moved) kept[id] = d;
        return {'stored': true, 'forked': fork, if (moved) 'moved': true};
      case Order.unknown:
        if (base > 0 && _m(have) > base && _m(have) != _m(d)) {
          if (d['deleted'] == true) return {'stored': false, 'conflict': true};
          final fork = 'p-${_m(d)}-$base';
          kept.putIfAbsent(fork, () => {...d, 'id': fork, 'name': '${d['name']} (pad)'});
          return {'stored': true, 'forked': fork};
        }
        if (_m(have) >= _m(d)) return {'stored': false};
        kept[id] = d;
        return {'stored': true};
    }
  }

  Map<String, Object?> _answer(Map<String, Object?> req) {
    switch (req['op']) {
      case 'info':
        return {'ok': true, 'host': name, 'id': '$name-id'};
      case 'designs':
        return {
          'ok': true,
          'designs': [
            for (final d in kept.values) {'id': d['id'], 'modified': d['modified'], 'deleted': d['deleted'] == true, 'vv': d['vv']}
          ]
        };
      case 'get':
        final d = kept[req['id']];
        return d == null ? {'ok': false, 'error': 'no such design'} : {'ok': true, 'design': d};
      case 'put':
        final id = (req['design'] as Map)['id'];
        return {'ok': true, ...put((req['design'] as Map).cast<String, Object?>()), 'modified': kept[id]?['modified']};
      default:
        return {'ok': false, 'error': '?'};
    }
  }
}

Shape mark(int t) => Shape(id: 'm$t', type: 'point', x: t + 0.5, y: 7.25);
Set<int> marks(Design d) => {for (final s in d.shapes) if (s.type == 'point' && s.y == 7.25) (s.x - 0.5).round()};

void main() {
  late Directory root;
  setUp(() => root = Directory.systemTemp.createTempSync('md'));
  tearDown(() => root.deleteSync(recursive: true));

  test('an edit kept on one computer is not written over by a pad whose base came from another', () async {
    final a = await VvDesk.start('deskA'), b = await VvDesk.start('deskB');
    final pad1 = DesignStore(Directory('${root.path}/pad1')), pad2 = DesignStore(Directory('${root.path}/pad2'));
    final x = pad1.save(Design(name: 'Sign', shapes: [mark(1)]), now: 1000);
    for (final (desk, pad) in [(a, pad1), (b, pad1), (a, pad2)]) {
      await syncWith(desk.desktop, pad, withTools: false);
    }
    // Pad 2 edits and syncs with A only; pad 1, which never saw that, edits
    // too and reaches B first, then A.
    pad2.save(pad2.get(x.id)!.copyWith(shapes: [...pad2.get(x.id)!.shapes, mark(2)]), now: 2000);
    await syncWith(a.desktop, pad2, withTools: false);
    pad1.save(pad1.get(x.id)!.copyWith(shapes: [...pad1.get(x.id)!.shapes, mark(3)]), now: 3000);
    await syncWith(b.desktop, pad1, withTools: false);
    final r = await syncWith(a.desktop, pad1, withTools: false);
    expect(r.conflicts.single, contains('yours is kept'));
    for (var i = 0; i < 3; i++) {
      for (final p in [pad1, pad2]) {
        for (final d in [a, b]) {
          await syncWith(d.desktop, p, withTools: false);
        }
      }
    }
    for (final p in [pad1, pad2]) {
      final live = p.live();
      expect(live.length, 2, reason: 'the design and one fork');
      expect(p.get(x.id)!.shapes.map((s) => s.id), ['m1', 'm3'], reason: 'the later edit keeps the id');
      expect(live.firstWhere((d) => d.id != x.id).shapes.map((s) => s.id), ['m1', 'm2'], reason: "pad 2's edit survives");
    }
    expect(a.kept.keys.toSet(), b.kept.keys.toSet(), reason: 'both computers forked it the same way');
    await a.server.close();
    await b.server.close();
  });

  test('pads and computers, edited and synced at random, lose nothing and end up the same', () async {
    for (var seed = 1; seed <= 8; seed++) {
      final rnd = Random(seed);
      final dir = Directory('${root.path}/s$seed');
      final desks = [await VvDesk.start('deskA'), await VvDesk.start('deskB')];
      final pads = [DesignStore(Directory('${dir.path}/p0')), DesignStore(Directory('${dir.path}/p1'))];
      var clock = 1000, token = 0;
      final made = <int>{}, gone = <int>{};
      for (var step = 0; step < 150; step++) {
        clock += 1 + rnd.nextInt(5);
        final pad = pads[rnd.nextInt(2)];
        final live = pad.live();
        final op = rnd.nextInt(100);
        if (op < 35) {
          final t = ++token;
          made.add(t);
          if (live.isEmpty || rnd.nextInt(100) < 15) {
            pad.save(Design(name: 'D$t', shapes: [mark(t)]), now: clock);
          } else {
            final d = live[rnd.nextInt(live.length)];
            pad.save(d.copyWith(shapes: [...d.shapes, mark(t)]), now: clock);
          }
        } else if (op < 40) {
          if (live.isNotEmpty) {
            final d = live[rnd.nextInt(live.length)];
            gone.addAll(marks(d));
            pad.delete(d.id, now: clock);
          }
        } else {
          await syncWith(desks[rnd.nextInt(2)].desktop, pad, withTools: false);
        }
      }
      for (var i = 0; i < 6; i++) {
        for (final p in pads) {
          for (final d in desks) {
            await syncWith(d.desktop, p, withTools: false);
          }
        }
      }
      String sig(Map<String, Design> m) =>
          (m.keys.toList()..sort()).map((k) => '$k:${m[k]!.modified}:${m[k]!.deleted}').join(',');
      final views = [
        for (final p in pads) {for (final d in p.all()) d.id: d},
        for (final k in desks) {for (final e in k.kept.entries) e.key: Design.fromJson(e.value)},
      ];
      for (final v in views.skip(1)) {
        expect(sig(v), sig(views.first), reason: 'seed $seed: every copy the same');
      }
      final kept = {for (final d in views.first.values) if (!d.deleted) ...marks(d)};
      expect(made.difference(gone).difference(kept), isEmpty, reason: 'seed $seed: no edit lost');
      final liveSets = [for (final d in views.first.values) if (!d.deleted) (marks(d).toList()..sort()).join(',')];
      expect(liveSets.toSet().length, liveSets.length, reason: 'seed $seed: no fork made twice');
      for (final d in desks) {
        await d.server.close();
      }
    }
  });
}
