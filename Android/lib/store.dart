// What the pad keeps: designs (one <id>.json each) and a few settings.

import 'dart:convert';
import 'dart:io';
import 'dart:math';

import 'model.dart';

/// Designs on the pad. A deleted design stays as a small "deleted" record so
/// the removal reaches every phobiCCCP too.
class DesignStore {
  final Directory dir;
  DesignStore(this.dir) {
    dir.createSync(recursive: true);
  }

  // As phobiCCCP accepts them (src/padlink.cpp).
  static final validId = RegExp(r'^[A-Za-z0-9-]{1,64}$');

  List<Design> all() {
    final out = <Design>[];
    for (final f in dir.listSync().whereType<File>()) {
      if (!f.path.endsWith('.json')) continue;
      try {
        out.add(Design.fromJson((jsonDecode(f.readAsStringSync()) as Map).cast<String, Object?>()));
      } catch (_) {}
    }
    out.sort((a, b) => b.modified.compareTo(a.modified));
    return out;
  }

  /// The designs to show: everything not deleted, newest first.
  List<Design> live() => all().where((d) => !d.deleted).toList();

  Design? get(String id) {
    final f = _file(id);
    if (f == null || !f.existsSync()) return null;
    try {
      return Design.fromJson((jsonDecode(f.readAsStringSync()) as Map).cast<String, Object?>());
    } catch (_) {
      return null;
    }
  }

  /// Write [d] exactly as given (sync keeps the sender's modified time).
  void put(Design d) {
    final f = _file(d.id);
    if (f == null) return;
    final tmp = File('${f.path}.tmp');
    tmp.writeAsStringSync(jsonEncode(d.toJson()), flush: true);
    tmp.renameSync(f.path);
  }

  static final _rand = Random();

  /// After [base], for an edit made now. With a clock behind the copy's, two
  /// devices editing the same copy must not both pick base+1: equal times
  /// mean "the same copy", and the two would never sync again. A copy from a
  /// clock gone absurdly far ahead is not followed there (every phobiCCCP
  /// would refuse the edit): the edit starts again from the clock.
  static int stamp(int base, int now) {
    if (now > base) return now;
    final next = base + 1 + _rand.nextInt(1000);
    return plausibleStamp(next, now) ? next : now;
  }

  /// An edit made here: stamped with the time, so it wins the next sync. Its
  /// base is the stored copy's (the last one exchanged with a phobiCCCP),
  /// whatever older copy the edit was made from (an undo, say).
  Design save(Design d, {int? now}) {
    final have = get(d.id);
    final was = have != null && have.modified > d.modified ? have.modified : d.modified;
    final s = d.copyWith(modified: stamp(was, now ?? DateTime.now().millisecondsSinceEpoch), base: have?.base ?? d.base);
    put(s);
    return s;
  }

  void delete(String id, {int? now}) {
    final d = get(id);
    if (d == null) return;
    // With its base, so a computer that changed it since can refuse.
    put(Design(
        id: d.id,
        name: d.name,
        deleted: true,
        base: d.base,
        modified: stamp(d.modified, now ?? DateTime.now().millisecondsSinceEpoch)));
  }

  File? _file(String id) => validId.hasMatch(id) ? File('${dir.path}/$id.json') : null;
}

/// Computers added by hand, the tool libraries as last received, the last target.
class Settings {
  final File file;
  Map<String, Object?> _v = {};
  Settings(this.file) {
    try {
      _v = (jsonDecode(file.readAsStringSync()) as Map).cast<String, Object?>();
    } catch (_) {}
  }

  void _save() {
    try {
      final tmp = File('${file.path}.tmp');
      tmp.writeAsStringSync(jsonEncode(_v), flush: true);
      tmp.renameSync(file.path);
    } catch (_) {}
  }

  Set<String> get manualHosts => ((_v['manualHosts'] as List?) ?? const []).whereType<String>().toSet();
  set manualHosts(Set<String> v) {
    _v['manualHosts'] = v.toList();
    _save();
  }

  ToolLibrary get tools {
    final t = _v['tools'];
    return t is Map ? ToolLibrary.fromJson(t.cast<String, Object?>()) : const ToolLibrary();
  }

  set tools(ToolLibrary t) {
    _v['tools'] = t.toJson();
    _save();
  }

  /// Each computer's own library (by its id): two computers may not have the
  /// same bits, and a send needs the tools to be in the receiving one's.
  ToolLibrary toolsOf(String desktopId) {
    final t = _m(_v['toolsByDesktop'])[desktopId];
    return t is Map ? ToolLibrary.fromJson(t.cast<String, Object?>()) : const ToolLibrary();
  }

  void setToolsOf(String desktopId, ToolLibrary t) {
    _v['toolsByDesktop'] = {..._m(_v['toolsByDesktop']), desktopId: t.toJson()};
    _save();
  }

  static Map<String, Object?> _m(Object? v) => v is Map ? v.cast<String, Object?>() : const {};

  String get lastDesign => _v['lastDesign'] is String ? _v['lastDesign'] as String : '';
  set lastDesign(String v) {
    _v['lastDesign'] = v;
    _save();
  }

  /// This pad's own id, made once, so a computer can send designs to it.
  String get deviceId {
    final v = _v['deviceId'];
    if (v is String && v.isNotEmpty) return v;
    final id = newId();
    _v['deviceId'] = id;
    _save();
    return id;
  }

  String get lastTarget => _v['lastTarget'] is String ? _v['lastTarget'] as String : '';
  set lastTarget(String v) {
    _v['lastTarget'] = v;
    _save();
  }
}
