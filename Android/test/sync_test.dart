import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
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
}
