import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/app_state.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/store.dart';
import 'package:phobicccp/sync.dart';

void main() {
  test('the newer copy wins, both ways', () {
    final plan = SyncPlan.of({'a': 5, 'b': 10, 'c': 7, 'only-here': 1},
        const [DesignEntry('a', modified: 9), DesignEntry('b', modified: 3), DesignEntry('c', modified: 7), DesignEntry('only-there', modified: 2)]);
    expect(plan.pull, ['a', 'only-there']);
    expect(plan.push, ['b', 'only-here']);
    expect(SyncPlan.of({'a': 1}, const [DesignEntry('a', modified: 1)]).isEmpty, true);
  });

  test('a time from a clock gone wrong never wins', () {
    const now = 1760850000000;
    final plan = SyncPlan.of({'a': now, 'b': 1e15.toInt()}, const [DesignEntry('a', modified: 1000000000000000), DesignEntry('b', modified: now)],
        now: now);
    expect(plan.pull, ['b']);
    expect(plan.push, ['a']);
    // Edits of such a copy start again from the clock instead of freezing at the limit.
    expect(DesignStore.stamp(1e15.toInt(), now), now);
    final soon = DesignStore.stamp(now + 10, now);
    expect(soon > now + 10 && soon <= now + 1011, true);
  });

  test('edits keep the base of the stored copy, deletions too', () {
    final dir = Directory.systemTemp.createTempSync('pad');
    final store = DesignStore(dir)..put(Design(id: 'd1', name: 'One', modified: 50, base: 50));
    final edited = store.save(store.get('d1')!.copyWith(name: 'Two'), now: 60);
    expect(edited.base, 50);
    // Undo saves an older copy, made before the last exchange: still the stored base.
    final undone = store.save(Design(id: 'd1', name: 'One', modified: 40, base: 30), now: 70);
    expect(undone.base, 50);
    store.delete('d1', now: 80);
    expect(store.get('d1')!.base, 50);
    dir.deleteSync(recursive: true);
  });

  test('the store stamps edits and keeps deletions', () {
    final dir = Directory.systemTemp.createTempSync('pad');
    final store = DesignStore(dir);
    final saved = store.save(Design(id: 'd1', name: 'One'), now: 100);
    expect(saved.modified, 100);
    // An edit within the same millisecond still counts as newer.
    final again = store.save(saved, now: 100).modified;
    expect(again > 100 && again <= 1100, true);
    store.delete('d1', now: 200);
    expect(store.get('d1')!.deleted, true);
    expect(store.live(), isEmpty);
    expect(store.all().length, 1);
    // Synced copies keep their own time.
    store.put(Design(id: 'd2', name: 'Two', modified: 42));
    expect(store.get('d2')!.modified, 42);
    // No paths through an id.
    store.put(Design(id: '../evil', modified: 1));
    expect(store.get('../evil'), isNull);
    expect(File('${dir.parent.path}/evil.json').existsSync(), false);
    dir.deleteSync(recursive: true);
  });

  test('edits note themselves in the versions, made from the stored copy', () {
    final dir = Directory.systemTemp.createTempSync('pad');
    final store = DesignStore(dir);
    final me = store.replicaId;
    expect(DesignStore(dir).replicaId, me, reason: 'a pad keeps its own id');
    final made = store.save(Design(id: 'd1', name: 'One', bases: {'x': 5}, vv: {'other': 7}), now: 100);
    expect(made.vv, {me: 100}, reason: 'a new design starts its own versions');
    expect(made.bases, isEmpty, reason: 'and was never exchanged with a computer');
    store.put(made.copyWith(vv: {'desk': 50, me: 100}, bases: {'desk': 100}));
    final edited = store.save(store.get('d1')!.copyWith(name: 'Two'), now: 200);
    expect(edited.vv, {'desk': 50, me: 200});
    expect(edited.bases, {'desk': 100});
    expect(orderOf(edited.vv, edited.modified, {'desk': 50, me: 100}, 100), Order.after);
    store.delete('d1', now: 300);
    expect(store.get('d1')!.vv, {'desk': 50, me: 300});
    // Stamped again by an older pad that kept the vv: not believed, but an
    // edit of it keeps it and notes the older pad's edit, so it counts as
    // made from that copy (and not from one edited apart from it).
    expect(believedVv({'a': 10}, 20), isEmpty);
    store.put(Design(id: 'd2', modified: 20, vv: {'a': 10}));
    final older = olderEditId({'a': 10}, 20);
    final onOlder = store.save(store.get('d2')!, now: 400);
    expect(onOlder.vv, {'a': 10, older: 20, me: 400});
    expect(orderOf(onOlder.vv, 400, {'a': 10}, 20), Order.after);
    expect(orderOf(onOlder.vv, 400, {'a': 10}, 25), Order.concurrent, reason: 'another edit of the same copy');
    // An older pad's design, with no vv at all, is noted the same way.
    store.put(Design(id: 'd3', modified: 30));
    expect(store.save(store.get('d3')!, now: 500).vv, {olderEditId(const {}, 30): 30, me: 500});
    dir.deleteSync(recursive: true);
  });

  test('how two copies stand to each other', () {
    expect(orderOf({'a': 1}, 1, {'a': 1}, 1), Order.same);
    expect(orderOf({'a': 1, 'b': 2}, 2, {'a': 1}, 1), Order.after);
    expect(orderOf({'a': 1}, 1, {'a': 1, 'b': 2}, 2), Order.before);
    expect(orderOf({'a': 3}, 3, {'a': 1, 'b': 2}, 2), Order.concurrent);
    // An older pad's copy: by the vv it kept (none: an older pad's design,
    // with nothing of the newer ones in it).
    expect(orderOf(const {}, 3, {'a': 1}, 1), Order.concurrent);
    expect(orderOf({'a': 1}, 3, {'a': 1}, 1), Order.after);
    expect(orderOf({'a': 1}, 3, {'a': 2}, 2), Order.concurrent, reason: 'an entry above the vv it kept');
    expect(orderOf({'a': 2}, 3, {'a': 1}, 1), Order.after);
    // Two older copies, or an older pad's deletion (it keeps no vv), tell nothing.
    expect(orderOf({'a': 1}, 3, {'a': 1}, 4), Order.unknown);
    expect(orderOf(const {}, 3, {'a': 1}, 1, aDeleted: true), Order.unknown);
    expect(orderOf(const {}, 3, const {}, 3), Order.same);
    // As phobiCCCP names them (tests/test_padlink.cpp).
    expect(olderEditId({'pad1': 1000, 'pad2': 2000}, 2600), 'e-e625dcdd1333364a');
    expect(forkIdFor(Design(id: 'sign-1', modified: 3000, vv: {'pad1': 3000})), '2b4050ca-c995-58c3-b7c9-374462673dd3');
    // Made from theirs: pushed; theirs made from ours: pulled; apart: pushed, for the computer to keep both.
    final plan = SyncPlan.versions({
      'a': (2, {'p': 2, 'd': 1}),
      'b': (1, {'d': 1}),
      'c': (3, {'p': 3}),
    }, const [
      DesignEntry('a', modified: 1, vv: {'d': 1}),
      DesignEntry('b', modified: 2, vv: {'d': 2}),
      DesignEntry('c', modified: 4, vv: {'d': 4}),
    ]);
    expect(plan.pull, ['b']);
    expect(plan.push, ['a', 'c']);
  });

  test('a put carries the base of the computer it goes to', () {
    final d = Design(id: 'd1', modified: 30, base: 9, bases: {'shop': 20, 'garage': 10});
    expect(d.toWire('shop')['base'], 20);
    expect(d.toWire('garage')['base'], 10);
    expect(d.toWire('new')['base'], 10, reason: 'one it never exchanged it with: the oldest');
    expect(d.toWire('shop').containsKey('bases'), false);
    expect(Design(id: 'd1', modified: 30, base: 9).toWire('shop')['base'], 9, reason: 'from before bases');
  });

  test('settings remember hosts and the tool library', () {
    final f = File('${Directory.systemTemp.createTempSync('pad').path}/s.json');
    Settings(f)
      ..manualHosts = {'shop.tail'}
      ..tools = const ToolLibrary(tools: [Tool(number: 201, name: 'End mill')]);
    final again = Settings(f);
    expect(again.manualHosts, {'shop.tail'});
    expect(again.tools.tool(201)?.name, 'End mill');
  });

  test('each computer keeps its own tool library', () {
    final f = File('${Directory.systemTemp.createTempSync('pad').path}/s.json');
    Settings(f)
      ..setToolsOf('shop', const ToolLibrary(tools: [Tool(number: 201, name: 'End mill')]))
      ..setToolsOf('garage', const ToolLibrary(tools: [Tool(number: 501, name: 'Engraver')]));
    final again = Settings(f);
    expect(again.toolsOf('shop').tool(201)?.name, 'End mill');
    expect(again.toolsOf('shop').tool(501), isNull);
    expect(again.toolsOf('garage').tool(501)?.name, 'Engraver');
    expect(again.toolsOf('nobody').isEmpty, true);
  });

  test('a computer typed in and heard on the LAN shows once', () {
    const lan = Computer(Desktop(id: 'i1', host: 'shop', address: '192.168.1.5'));
    const typed = Computer(Desktop(id: 'manual:shop.tail', host: 'shop.tail', address: 'shop.tail', typed: 'shop.tail'),
        info: DesktopInfo(id: 'i1', host: 'shop'));
    for (final order in [[typed, lan], [lan, typed]]) {
      final list = AppState.dedupe(order);
      expect(list, hasLength(1));
      expect(list.single.desktop.id, 'i1');
      expect(list.single.desktop.typed, 'shop.tail');
    }
    const offline = Computer(Desktop(id: 'manual:other', host: 'other', address: 'other', typed: 'other'));
    expect(AppState.dedupe([offline, lan]), hasLength(2));
  });
}
