// What the pad keeps: designs (one <id>.json each) and a few settings.

import 'dart:convert';
import 'dart:io';

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

  /// An edit made here: stamped with the time, so it wins the next sync.
  Design save(Design d, {int? now}) {
    final t = now ?? DateTime.now().millisecondsSinceEpoch;
    final s = d.copyWith(modified: t > d.modified ? t : d.modified + 1);
    put(s);
    return s;
  }

  void delete(String id, {int? now}) {
    final d = get(id);
    if (d == null) return;
    final t = now ?? DateTime.now().millisecondsSinceEpoch;
    put(Design(id: d.id, name: d.name, deleted: true, modified: t > d.modified ? t : d.modified + 1));
  }

  File? _file(String id) => validId.hasMatch(id) ? File('${dir.path}/$id.json') : null;
}

/// Computers added by hand, the tool library as last received, the last target.
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
      file.writeAsStringSync(jsonEncode(_v));
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

  String get lastDesign => _v['lastDesign'] is String ? _v['lastDesign'] as String : '';
  set lastDesign(String v) {
    _v['lastDesign'] = v;
    _save();
  }

  String get lastTarget => _v['lastTarget'] is String ? _v['lastTarget'] as String : '';
  set lastTarget(String v) {
    _v['lastTarget'] = v;
    _save();
  }
}
