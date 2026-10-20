// Keeping the pad and phobiCCCPs in step, both ways, a copy made from
// another replacing it. A deletion is just a later copy, so it travels the
// same way.
//
// Every edit notes itself in the design's version vector ([Design.vv]): the
// time of the latest edit each pad or phobiCCCP made to it. From those, the
// pad and the computer can tell a copy made from the other (it replaces the
// other) from two copies edited apart, however they went round between pads
// and computers. Two copies edited apart are settled by the computer: both
// are kept, the later edit under the design's id and the other as a new
// design, the same way on every computer (src/padlink.h, "Versions"); the pad
// then takes both from it.
//
// A copy from an older pad or phobiCCCP (before 1.2.9) keeps the vv of the
// copy it edited, or has none (a design an older pad made): that still tells
// whether the edit was made from a copy with a vv, and an edit here of such
// a copy notes it in its vv ([ancestryOf]). Where neither copy can tell (two
// from older pads, or a deletion from one), the newer time wins as before.
// Then the pad's copy carries its
// "base": the time of the copy it last got from or gave to that computer
// ([Design.bases]). A computer whose own copy changed after that does not let
// the pad's edit replace it: it keeps the pad's version as a new design (a
// deletion it refuses), and the pad takes the computer's copy in place of its
// own, so both edits survive and the pad stops sending its version again.

import 'dart:math' as math;

import 'model.dart';
import 'net.dart';
import 'store.dart';

/// The id a pad keeps that computer's base under: the one it gives itself,
/// else the one it was found by.
String desktopKey(DesktopInfo? info, Desktop desk) => info != null && info.id.isNotEmpty ? info.id : desk.id;

class SyncPlan {
  final List<String> pull, push;
  const SyncPlan(this.pull, this.push);
  bool get isEmpty => pull.isEmpty && push.isEmpty;

  /// Equal times mean the same copy: nothing moves. A time from a clock gone
  /// absurdly far ahead counts as the oldest, so it never wins.
  factory SyncPlan.of(Map<String, int> local, List<DesignEntry> remote, {int? now}) =>
      SyncPlan.versions({for (final e in local.entries) e.key: (e.value, const <String, int>{})}, remote, now: now);

  /// With version vectors: a copy made from the other's is pulled or pushed;
  /// two edited apart are pushed, for the computer to keep both. Where they
  /// cannot tell ([Order.unknown]), the newer time wins.
  /// [deleted]: the local designs that are deletions.
  factory SyncPlan.versions(Map<String, (int, Map<String, int>)> local, List<DesignEntry> remote,
      {int? now, Set<String> deleted = const {}}) {
    final t = now ?? DateTime.now().millisecondsSinceEpoch;
    int sane(int m) => plausibleStamp(m, t) ? m : 0;
    final pull = <String>[], push = <String>[];
    final theirs = <String>{};
    for (final e in remote) {
      theirs.add(e.id);
      final mine = local[e.id];
      if (mine == null) {
        pull.add(e.id);
        continue;
      }
      final (modified, vv) = mine;
      final plausible = plausibleStamp(modified, t) && plausibleStamp(e.modified, t);
      final o = orderOf(vv, modified, e.vv, e.modified, aDeleted: deleted.contains(e.id), bDeleted: e.deleted);
      switch (plausible ? o : Order.unknown) {
        case Order.same:
          break;
        case Order.before:
          pull.add(e.id);
        case Order.after:
        case Order.concurrent:
          push.add(e.id);
        case Order.unknown:
          if (sane(e.modified) > sane(modified)) pull.add(e.id);
          if (sane(modified) > sane(e.modified)) push.add(e.id);
      }
    }
    push.addAll(local.keys.where((id) => !theirs.contains(id)));
    return SyncPlan(pull..sort(), push..sort());
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
  const SyncResult(this.info, this.tools, this.pulled, this.pushed, [this.refused = const [], this.conflicts = const []]);
}

/// One round with one phobiCCCP: its status, its tool library when [withTools]
/// (it seldom changes, and it is the big answer), and the designs both ways.
/// [onPulled] runs after each design taken in, so an open copy is replaced
/// before more edits are made on top of the old one.
Future<SyncResult> syncWith(Desktop desk, DesignStore store, {bool withTools = true, void Function()? onPulled}) =>
    CccpConnection.use(desk.address, desk.port, (c) async {
      final info = await c.info();
      final tools = withTools ? await c.tools() : null;
      final now = DateTime.now().millisecondsSinceEpoch;
      final key = desktopKey(info, desk);
      final all = store.all();
      final mineNow = {for (final d in all) d.id: d};
      var remote = await c.designs();
      if (!info.keepsBoth) {
        // An older phobiCCCP lists no versions, though it keeps them: each
        // copy there that is not this pad's is asked for whole.
        remote = [
          for (final e in remote)
            if (mineNow[e.id] != null && mineNow[e.id]!.modified != e.modified) await _whole(c, e) else e
        ];
      }
      final theirs = {for (final e in remote) e.id: e};
      final plan = SyncPlan.versions({for (final d in all) d.id: (d.modified, d.vv)}, remote,
          now: now, deleted: {for (final d in all) if (d.deleted) d.id});
      final host = info.host.isEmpty ? desk.host : info.host;
      // The same copy there (sent by "send", say, whose answer this pad did
      // not hear): the next edit here is an edit of it.
      for (final e in remote) {
        final mine = store.get(e.id);
        if (mine != null &&
            mine.orderTo(e.vv, e.modified, otherDeleted: e.deleted) == Order.same &&
            mine.bases[key] != e.modified &&
            plausibleStamp(e.modified, now)) {
          store.put(mine.withBase(key, e.modified));
        }
      }
      var pulled = 0, pushed = 0;
      final refused = <String>[], conflicts = <String>[];

      // The computer's copy, as this pad's from now on (and the base of edits
      // made on it): when it was made from this pad's, or, with [over] (the
      // copy this pad gave, after a conflict the computer settled), in place
      // of that one.
      Future<bool> take(String id, {Design? over}) async {
        final d = await c.get(id);
        // Not the one asked for, a name this pad cannot store, or a time from
        // a clock gone wrong: skipped, not counted as pulled.
        if (d.id != id || !DesignStore.validId.hasMatch(id) || !plausibleStamp(d.modified, now)) return false;
        final cur = store.get(id);
        if (over != null) {
          // Edited here again meanwhile: that edit goes with the next sync.
          if (cur != null && cur.modified != over.modified) return false;
        } else if (cur != null) {
          // It may have changed here meanwhile; the copy made from the other still wins.
          final o = orderOf(d.vv, d.modified, cur.vv, cur.modified, aDeleted: d.deleted, bDeleted: cur.deleted);
          if (o != Order.after && (o != Order.unknown || d.modified <= cur.modified)) return false;
        }
        store.put(d.copyWith(base: d.modified, bases: {...?cur?.bases, key: d.modified}));
        onPulled?.call();
        return true;
      }

      // A phobiCCCP from before version vectors would keep the later of two
      // copies edited apart, or fork the pad's by its base, unlike every
      // later one (Store::put): the pad settles the two for it the same way
      // they do. The copy that loses becomes a design of its own here, with
      // the id they give it, and goes everywhere with the next sync. Whether
      // this pad's copy won (it then goes there with that computer's copy as
      // its base, so it replaces it), else null.
      Future<bool?> settleForOlder(Design d) async {
        final w = await c.get(d.id);
        if (w.id != d.id || !plausibleStamp(w.modified, now)) return null;
        final mineWins = winsOver(d, w);
        final loser = mineWins ? w : d;
        String? fork;
        if (!loser.deleted) {
          final id = forkIdFor(loser);
          fork = '${loser.name.substring(0, math.min(60, loser.name.length))} (conflict)';
          if (store.get(id) == null) store.put(loser.copyWith(id: id, name: fork, base: 0, bases: const {}));
          fork = store.get(id)?.name ?? fork;
          pulled++;
        }
        if (mineWins) {
          conflicts.add(w.deleted
              ? '“${d.name}” was deleted on $host, but changed here since, so it is back there'
              : d.deleted
                  ? '“${d.name}” changed on $host before it was deleted here: that version is here as “$fork”'
                  : '“${d.name}” changed on $host too: yours is kept, and theirs is here as “$fork”');
          return true;
        }
        store.put(w.copyWith(base: w.modified, bases: {...d.bases, key: w.modified}));
        pulled++;
        onPulled?.call();
        conflicts.add(d.deleted
            ? '“${d.name}” changed on $host since, so it was not deleted there and is back here'
            : w.deleted
                ? '“${d.name}” was deleted on $host since: your version is here as “$fork”'
                : '“${d.name}” changed on $host too: theirs is here now, and yours is here as “$fork”');
        return false;
      }

      // Send ours, and sort out a conflict it runs into.
      Future<bool> give(Design d) async {
        // A phobiCCCP from before version vectors goes by the base alone.
        // The versions here say more: made from its copy, the base is that
        // copy (it is replaced, not forked again); edited apart from it, the
        // pad settles the two first (settleForOlder).
        var sent = d;
        final there = theirs[d.id];
        if (there != null) {
          switch (d.orderTo(there.vv, there.modified, otherDeleted: there.deleted)) {
            case Order.after:
              sent = d.withBase(key, there.modified);
            case Order.concurrent:
              if (!info.keepsBoth) {
                final mineWins = await settleForOlder(d);
                if (mineWins != true) return mineWins == false;
                sent = d.withBase(key, there.modified);
              }
            default:
          }
        }
        final PutResult r;
        // One design it will not take must not hold back the rest.
        try {
          r = await c.push(sent, desktop: key);
        } on CccpException catch (e) {
          if (!e.refused) rethrow;
          refused.add('“${d.name}”: ${e.message}');
          return false;
        }
        if (r.forked.isNotEmpty || r.conflict) {
          if (await take(d.id, over: d)) pulled++;
          // The other of the two, kept there as a design of its own.
          if (r.forked.isNotEmpty && await take(r.forked)) pulled++;
          final fork = r.forked.isEmpty ? null : store.get(r.forked)?.name;
          conflicts.add(r.conflict
              ? '“${d.name}” changed on $host since, so it was not deleted there and is back here'
              : r.moved
                  ? '“${d.name}” changed on $host too: yours is kept, and theirs is here as “${fork ?? '${d.name} (conflict)'}”'
                  : '“${d.name}” changed on $host too: theirs is here now, and yours comes as “${fork ?? '${d.name} (pad)'}”');
          return true;
        }
        if (r.stored) pushed++;
        // Kept there, or it already had this very copy: the next edit here is
        // an edit of the copy that computer has.
        if (r.stored || r.modified == d.modified) {
          final cur = store.get(d.id);
          if (cur != null && cur.modified == d.modified) store.put(cur.withBase(key, d.modified));
        }
        return true;
      }

      for (final id in plan.pull) {
        final mine = store.get(id);
        final there = theirs[id];
        // Without versions to tell: edited here since the copy it came from,
        // and newer there: offered first, so a computer that changed it too
        // keeps both rather than this pull throwing the edit made here away.
        if (mine != null && there != null && mine.orderTo(there.vv, there.modified, otherDeleted: there.deleted) == Order.unknown) {
          final base = mine.baseFor(key);
          if (base > 0 && mine.modified != base) {
            final before = conflicts.length;
            // Not taken there (busy, refused): the edit here must not be pulled over.
            if (!await give(mine)) continue;
            if (conflicts.length > before) continue;
          }
        }
        if (await take(id)) pulled++;
      }
      for (final id in plan.push) {
        final d = store.get(id);
        if (d != null) await give(d);
      }
      return SyncResult(info, tools, pulled, pushed, refused, conflicts);
    });

/// [e] as [c] keeps it, with its versions; [e] itself when that fails.
Future<DesignEntry> _whole(CccpConnection c, DesignEntry e) async {
  try {
    final d = await c.get(e.id);
    if (d.id == e.id && d.modified == e.modified) {
      return DesignEntry(e.id, name: e.name, modified: e.modified, deleted: e.deleted, vv: d.vv);
    }
  } on CccpException catch (_) {}
  return e;
}

/// After a send to [host] (whose id is [desktop]) over [c]: the copy kept
/// there is this pad's base from now on, or, when it had changed there since
/// and the computer's was the later edit ([SendResult.forked], not moved: the
/// pad's version opened as a new design), its copy replaces this pad's, as
/// after a conflict in a sync. Then what to tell the user, else null.
Future<String?> settleSend(CccpConnection c, DesignStore store, Design d, SendResult r, String host, {String desktop = ''}) async {
  final cur = store.get(d.id);
  // Edited again here meanwhile: that edit is left for the next sync.
  if (cur == null || cur.modified != d.modified) return null;
  if (r.forked.isNotEmpty && !r.moved) {
    final theirs = await c.get(d.id);
    if (theirs.id != d.id || !plausibleStamp(theirs.modified, DateTime.now().millisecondsSinceEpoch)) return null;
    store.put(theirs.copyWith(base: theirs.modified, bases: {...cur.bases, desktop: theirs.modified}));
    return '“${d.name}” changed on $host too: theirs is here now, and yours opened there as a copy';
  }
  if (r.modified == d.modified) store.put(cur.withBase(desktop, d.modified));
  if (r.moved) return '“${d.name}” changed on $host too: yours opened there, and theirs comes here as a copy';
  return null;
}
