// Keeping the pad and one phobiCCCP in step: the newest copy of every design
// wins, both ways. A deletion is just a newer copy, so it travels the same way.
//
// Every copy the pad sends carries its "base", the time of the copy it last
// got from or gave to a phobiCCCP. A computer whose own copy changed after
// that does not let the pad's edit replace it: it keeps the pad's version as a
// new design (a deletion it refuses), and the pad takes the computer's copy
// in place of its own, so both edits survive and the pad stops sending its
// version again.

import 'model.dart';
import 'net.dart';
import 'store.dart';

class SyncPlan {
  final List<String> pull, push;
  const SyncPlan(this.pull, this.push);
  bool get isEmpty => pull.isEmpty && push.isEmpty;

  /// Equal times mean the same copy: nothing moves. A time from a clock gone
  /// absurdly far ahead counts as the oldest, so it never wins.
  factory SyncPlan.of(Map<String, int> local, List<DesignEntry> remote, {int? now}) {
    final t = now ?? DateTime.now().millisecondsSinceEpoch;
    int sane(int m) => plausibleStamp(m, t) ? m : 0;
    final mine = {for (final e in local.entries) e.key: sane(e.value)};
    final theirs = {for (final e in remote) e.id: sane(e.modified)};
    final pull = theirs.entries.where((e) => !mine.containsKey(e.key) || e.value > mine[e.key]!).map((e) => e.key).toList()..sort();
    final push = mine.entries.where((e) => !theirs.containsKey(e.key) || e.value > theirs[e.key]!).map((e) => e.key).toList()..sort();
    return SyncPlan(pull, push);
  }
}

class SyncResult {
  final DesktopInfo info;
  final ToolLibrary? tools;
  final int pulled, pushed;

  /// Designs this phobiCCCP turned down, by name, with its reason.
  final List<String> refused;

  /// Designs changed both here and there: what became of them, to tell the user.
  final List<String> conflicts;

  /// A design sent from the computer for this pad to open, now in the store.
  final OpenRequest? open;
  const SyncResult(this.info, this.tools, this.pulled, this.pushed,
      [this.refused = const [], this.conflicts = const [], this.open]);
}

/// One round with one phobiCCCP: its status, its tool library when [withTools]
/// (it seldom changes, and it is the big answer), and the designs both ways.
/// [onPulled] runs after each design taken in, so an open copy is replaced
/// before more edits are made on top of the old one.
/// [pad] ({id, name}) says who asks; a design the computer sent for this pad
/// to open comes back in [SyncResult.open] once it is in the store, and the
/// computer is told it arrived. [handled] is the request already opened from
/// this computer, which is not opened again when its answer was lost.
Future<SyncResult> syncWith(Desktop desk, DesignStore store,
        {bool withTools = true, void Function()? onPulled, Map<String, String>? pad, int handled = 0}) =>
    CccpConnection.use(desk.address, desk.port, (c) async {
      final info = await c.info(pad: pad);
      final tools = withTools ? await c.tools() : null;
      final now = DateTime.now().millisecondsSinceEpoch;
      final remote = await c.designs();
      final plan = SyncPlan.of({for (final d in store.all()) d.id: d.modified}, remote, now: now);
      final host = info.host.isEmpty ? desk.host : info.host;
      // The same time there means the same copy (sent by "send", say, whose
      // answer this pad did not hear): the next edit here is an edit of it.
      for (final e in remote) {
        final mine = store.get(e.id);
        if (mine != null && mine.modified == e.modified && mine.base != e.modified && plausibleStamp(e.modified, now)) {
          store.put(mine.copyWith(base: e.modified));
        }
      }
      var pulled = 0, pushed = 0;
      final refused = <String>[], conflicts = <String>[];

      // The computer's copy, as this pad's from now on (and the base of edits
      // made on it). Older than ours only after a conflict, when it must
      // still replace ours.
      Future<bool> take(String id, {bool evenIfOlder = false}) async {
        final d = await c.get(id);
        // Not the one asked for, a name this pad cannot store, or a time from
        // a clock gone wrong: skipped, not counted as pulled.
        if (d.id != id || !DesignStore.validId.hasMatch(id) || !plausibleStamp(d.modified, now)) return false;
        // It may have changed here meanwhile; the newer one still wins.
        if (!evenIfOlder && d.modified <= (store.get(id)?.modified ?? -1)) return false;
        store.put(d.copyWith(base: d.modified));
        onPulled?.call();
        return true;
      }

      // Send ours, and sort out a conflict it runs into.
      Future<void> give(Design d) async {
        final PutResult r;
        // One design it will not take must not hold back the rest.
        try {
          r = await c.push(d);
        } on CccpException catch (e) {
          if (!e.refused) rethrow;
          refused.add('“${d.name}”: ${e.message}');
          return;
        }
        if (r.forked.isNotEmpty || r.conflict) {
          if (await take(d.id, evenIfOlder: true)) pulled++;
          conflicts.add(r.conflict
              ? '“${d.name}” changed on $host since, so it was not deleted there and is back here'
              : '“${d.name}” changed on $host too: theirs is here now, and yours comes as “${d.name} (pad)”');
          return;
        }
        if (r.stored) pushed++;
        // Kept there, or it already had this very copy: the next edit here is
        // an edit of the copy that computer has.
        if (r.stored || r.modified == d.modified) {
          final cur = store.get(d.id);
          if (cur != null && cur.modified == d.modified) store.put(cur.copyWith(base: d.modified));
        }
      }

      for (final id in plan.pull) {
        final mine = store.get(id);
        // Edited here since the copy it came from, and newer there: offered
        // first, so a computer that changed it too keeps both rather than
        // this pull throwing the edit made here away.
        if (mine != null && mine.base > 0 && mine.modified != mine.base) {
          final before = conflicts.length;
          await give(mine);
          if (conflicts.length > before) continue;
        }
        if (await take(id)) pulled++;
      }
      for (final id in plan.push) {
        final d = store.get(id);
        if (d != null) await give(d);
      }
      // Asked for by the computer: the design came with this round (or was
      // here already). Not there (it could not be pulled): asked again next round.
      OpenRequest? open;
      final req = info.open;
      final want = req == null ? null : store.get(req.design);
      if (req != null && pad != null && want != null && !want.deleted) {
        if (req.seq != handled) open = req;
        try {
          await c.opened(req.seq, pad['id'] ?? '');
        } on CccpException catch (e) {
          if (!e.refused) rethrow;
        }
      }
      return SyncResult(info, tools, pulled, pushed, refused, conflicts, open);
    });

/// After a send to [host] over [c]: the copy kept there is this pad's base
/// from now on, or, when it had changed there since ([SendResult.forked]: the
/// pad's version opened as a new design), its copy replaces this pad's, as
/// after a conflict in a sync. Then what to tell the user, else null.
Future<String?> settleSend(CccpConnection c, DesignStore store, Design d, SendResult r, String host) async {
  final cur = store.get(d.id);
  // Edited again here meanwhile: that edit is left for the next sync.
  if (cur == null || cur.modified != d.modified) return null;
  if (r.forked.isNotEmpty) {
    final theirs = await c.get(d.id);
    if (theirs.id != d.id || !plausibleStamp(theirs.modified, DateTime.now().millisecondsSinceEpoch)) return null;
    store.put(theirs.copyWith(base: theirs.modified));
    return '“${d.name}” changed on $host too: theirs is here now, and yours opened there as “${d.name} (pad)”';
  }
  if (r.modified == d.modified) store.put(cur.copyWith(base: d.modified));
  return null;
}
