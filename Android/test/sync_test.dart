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
