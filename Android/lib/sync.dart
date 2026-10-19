// Keeping the pad and one phobiCCCP in step: the newest copy of every design
// wins, both ways. A deletion is just a newer copy, so it travels the same way.

import 'model.dart';
import 'net.dart';
import 'store.dart';

class SyncPlan {
  final List<String> pull, push;
  const SyncPlan(this.pull, this.push);
  bool get isEmpty => pull.isEmpty && push.isEmpty;

  /// Equal times mean the same copy: nothing moves.
  factory SyncPlan.of(Map<String, int> local, List<DesignEntry> remote) {
    final theirs = {for (final e in remote) e.id: e.modified};
    final pull = theirs.entries.where((e) => !local.containsKey(e.key) || e.value > local[e.key]!).map((e) => e.key).toList()..sort();
    final push = local.entries.where((e) => !theirs.containsKey(e.key) || e.value > theirs[e.key]!).map((e) => e.key).toList()..sort();
    return SyncPlan(pull, push);
  }
}

class SyncResult {
  final DesktopInfo info;
  final ToolLibrary? tools;
  final int pulled, pushed;
  const SyncResult(this.info, this.tools, this.pulled, this.pushed);
}

/// One round with one phobiCCCP: its status, its tool library when [withTools]
/// (it seldom changes, and it is the big answer), and the designs both ways.
Future<SyncResult> syncWith(Desktop desk, DesignStore store, {bool withTools = true}) =>
    CccpConnection.use(desk.address, desk.port, (c) async {
      final info = await c.info();
      final tools = withTools ? await c.tools() : null;
      final plan = SyncPlan.of({for (final d in store.all()) d.id: d.modified}, await c.designs());
      var pulled = 0, pushed = 0;
      for (final id in plan.pull) {
        final d = await c.get(id);
        // It may have changed here meanwhile; the newer one still wins.
        if (d.modified > (store.get(id)?.modified ?? -1)) {
          store.put(d);
          pulled++;
        }
      }
      for (final id in plan.push) {
        final d = store.get(id);
        if (d != null && await c.put(d)) pushed++;
      }
      return SyncResult(info, tools, pulled, pushed);
    });
