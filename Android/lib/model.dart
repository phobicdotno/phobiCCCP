// Designs and what phobiCCCP says about itself, in the JSON the desktop reads
// and writes (src/padlink.h and src/projectreceiver.h in phobiCCCP).
// Millimetres, X right, Y up, X0 Y0 at the stock's front-left corner.

import 'dart:convert';
import 'dart:math' as math;

import 'package:crypto/crypto.dart';

const padFormat = 'phobicccp-pad';

double _d(Object? v, [double or = 0]) => v is num ? v.toDouble() : or;
int _i(Object? v, [int or = 0]) => v is num ? v.toInt() : or;
String _s(Object? v, [String or = '']) => v is String ? v : or;
bool _b(Object? v, [bool or = false]) => v is bool ? v : or;
List<Object?> _l(Object? v) => v is List ? v : const [];
Map<String, Object?> _m(Object? v) => v is Map ? v.cast<String, Object?>() : const {};

/// The keys of [j] this pad does not read, kept as they are so that a pad
/// edit never strips what a newer phobiCCCP put into a design.
Map<String, Object?> _extra(Map<String, Object?> j, Set<String> known) =>
    {for (final e in j.entries) if (!known.contains(e.key)) e.key: e.value};

/// A design time further ahead of this pad's clock than this is a clock gone
/// wrong, not an edit (as phobiCCCP judges it, src/padlink.h kMaxAheadMs):
/// taken in, it would win every sync until then.
const maxAheadMs = 366 * 24 * 3600 * 1000;
bool plausibleStamp(int modified, int now) => modified <= now + maxAheadMs;

/// How one copy of a design stands to another (src/padlink.h, "Versions"):
/// the same copy, made from it ([after]), what it was made from ([before]),
/// edited apart from it ([concurrent]), or [unknown] when either has no
/// version vector that can be believed (then the newer time wins, as before).
enum Order { same, after, before, concurrent, unknown }

final _replicaId = RegExp(r'^[A-Za-z0-9-]{1,64}$');
const maxReplicas = 64;

Map<String, int> _ints(Object? v) =>
    {for (final e in _m(v).entries) if (e.value is num) e.key: (e.value as num).toInt()};

/// [vv] when it can be believed: a few entries, each a pad's or phobiCCCP's
/// id and time, the newest of them the copy's [modified] (a pad or phobiCCCP
/// from before version vectors stamps a new time and keeps the vv it got).
/// Else empty.
Map<String, int> believedVv(Map<String, int> vv, int modified) {
  if (vv.isEmpty || vv.length > maxReplicas) return const {};
  var newest = 0;
  for (final e in vv.entries) {
    if (!_replicaId.hasMatch(e.key) || e.value < 1) return const {};
    newest = math.max(newest, e.value);
  }
  return newest == modified ? vv : const {};
}

/// [vv] as it is, believed or not (empty for a copy with none); null when
/// malformed, as nothing can be told from it then.
Map<String, int>? _rawVv(Map<String, int> vv) =>
    vv.length <= maxReplicas && vv.entries.every((e) => _replicaId.hasMatch(e.key) && e.value >= 1) ? vv : null;

/// What an older pad's or phobiCCCP's copy tells of the copy it was edited
/// from: its vv, or for one with none (a design an older pad made), that
/// nothing of the newer ones went into it. A deletion from an older pad keeps
/// no vv, so it tells nothing (null).
Map<String, int>? _olderVv(Map<String, int> vv, bool deleted) {
  final raw = _rawVv(vv);
  return raw == null || (raw.isEmpty && deleted) ? null : raw;
}

/// The id in version vectors that stands for an older pad's or phobiCCCP's
/// edit (stamped [modified], keeping [vv]); as olderEditId() in
/// src/padlink.cpp, so every pad and computer names it the same.
String olderEditId(Map<String, int> vv, int modified) {
  final raw = _rawVv(vv) ?? const {};
  final key = '$modified\n${(raw.keys.toList()..sort()).map((k) => '$k=${raw[k]};').join()}';
  var h1 = 2166136261, h2 = 0x811c9dc5 ^ 0x5bd1e995;
  for (final c in utf8.encode(key)) {
    h1 = ((h1 ^ c) * 16777619) & 0xffffffff;
    h2 = ((h2 ^ c) * 16777619) & 0xffffffff;
  }
  return 'e-${h1.toRadixString(16).padLeft(8, '0')}${h2.toRadixString(16).padLeft(8, '0')}';
}

/// The version vector an edit of the copy with [vv] and [modified] builds on:
/// its own when believed, else (from an older pad or phobiCCCP) the one it
/// kept with that edit noted under [olderEditId], so the new edit counts as
/// made from it. As ancestry() in src/padlink.cpp.
Map<String, int> ancestryOf(Map<String, int> vv, int modified, {bool deleted = false}) {
  final b = believedVv(vv, modified);
  if (b.isNotEmpty) return b;
  final older = _olderVv(vv, deleted);
  return older == null || modified < 1 ? const {} : {...older, olderEditId(older, modified): modified};
}

/// How a copy with the believed [vx] stands to one with [vy] stamped [my]
/// that is not believed: an older pad's or phobiCCCP's edit of the copy [vy]
/// names.
Order _relate(Map<String, int> vx, Map<String, int> vy, int my, bool yDeleted) {
  final ry = _olderVv(vy, yDeleted);
  if (ry == null) return Order.unknown;
  if ((vx[olderEditId(ry, my)] ?? 0) >= my) return Order.after;
  if (vx.entries.every((e) => e.value <= (ry[e.key] ?? 0))) return Order.before;
  return Order.concurrent;
}

/// How the copy with [va] and [ma] stands to the one with [vb] and [mb]; as
/// phobiCCCP's order() in src/padlink.cpp. Whether either is a deletion
/// matters only for a copy from an older pad.
Order orderOf(Map<String, int> va, int ma, Map<String, int> vb, int mb, {bool aDeleted = false, bool bDeleted = false}) {
  final a = believedVv(va, ma), b = believedVv(vb, mb);
  if (a.isEmpty || b.isEmpty) {
    if (ma == mb) return Order.same;
    if (a.isNotEmpty) return _relate(a, vb, mb, bDeleted);
    if (b.isNotEmpty) {
      final o = _relate(b, va, ma, aDeleted);
      return o == Order.after ? Order.before : (o == Order.before ? Order.after : o);
    }
    return Order.unknown;
  }
  var aAhead = false, bAhead = false;
  for (final e in a.entries) {
    final o = b[e.key] ?? 0;
    aAhead = aAhead || e.value > o;
    bAhead = bAhead || e.value < o;
  }
  for (final e in b.entries) {
    bAhead = bAhead || (!a.containsKey(e.key) && e.value > 0);
  }
  return aAhead ? (bAhead ? Order.concurrent : Order.after) : (bAhead ? Order.before : Order.same);
}

/// The same text for the same vv on every pad and computer (src/padlink.cpp vvKey).
String vvKey(Map<String, int> vv) => (vv.keys.toList()..sort()).map((k) => '$k=${vv[k]};').join();

/// Of two copies edited apart, [a] is the one every phobiCCCP keeps under the
/// design's id (src/padlink.cpp wins): the later edit.
bool winsOver(Design a, Design b) => a.modified != b.modified
    ? a.modified > b.modified
    : vvKey(believedVv(a.vv, a.modified)).compareTo(vvKey(believedVv(b.vv, b.modified))) > 0;

/// The id every phobiCCCP gives [copy] when it keeps it beside the one that
/// won (src/padlink.cpp Store::forkIdFor): a name-based (version 5) UUID.
String forkIdFor(Design copy) {
  const ns = [0x3e, 0x0b, 0x7c, 0x52, 0x61, 0xa4, 0x4f, 0x0e, 0x9d, 0x27, 0x8b, 0x5c, 0x1a, 0x6e, 0x4f, 0x93];
  final name = utf8.encode('${copy.id}\n${copy.modified}\n${vvKey(believedVv(copy.vv, copy.modified))}');
  final h = sha1.convert([...ns, ...name]).bytes.sublist(0, 16);
  h[6] = (h[6] & 0x0f) | 0x50;
  h[8] = (h[8] & 0x3f) | 0x80;
  final hex = h.map((b) => b.toRadixString(16).padLeft(2, '0')).join();
  return '${hex.substring(0, 8)}-${hex.substring(8, 12)}-${hex.substring(12, 16)}-${hex.substring(16, 20)}-${hex.substring(20)}';
}

/// [points] without a point equal to the one before it (and, closed, without
/// a last point equal to the first): each would make a segment of no length.
List<List<double>> dropRepeats(List<List<double>> points, {bool closed = false}) {
  final out = <List<double>>[];
  for (final p in points) {
    if (out.isEmpty || out.last[0] != p[0] || out.last[1] != p[1]) out.add(p);
  }
  while (closed && out.length > 1 && out.last[0] == out.first[0] && out.last[1] == out.first[1]) {
    out.removeLast();
  }
  return out;
}

String newId() {
  final r = math.Random.secure();
  String hex(int n) => List.generate(n, (_) => r.nextInt(16).toRadixString(16)).join();
  return '${hex(8)}-${hex(4)}-4${hex(3)}-${(8 + r.nextInt(4)).toRadixString(16)}${hex(3)}-${hex(12)}';
}

class Stock {
  final double width, height, thickness;
  const Stock({this.width = 300, this.height = 200, this.thickness = 19});

  factory Stock.fromJson(Map<String, Object?> j) =>
      // Within what phobiCCCP takes (src/padlink.cpp check), so a bad copy
      // from the network cannot make the canvas draw millions of grid lines.
      Stock(
          width: _d(j['width'], 300).clamp(1, 10000).toDouble(),
          height: _d(j['height'], 200).clamp(1, 10000).toDouble(),
          thickness: _d(j['thickness'], 19).clamp(0.1, 1000).toDouble());
  Map<String, Object?> toJson() => {'width': width, 'height': height, 'thickness': thickness};
  Stock copyWith({double? width, double? height, double? thickness}) =>
      Stock(width: width ?? this.width, height: height ?? this.height, thickness: thickness ?? this.thickness);
}

/// One drawn shape. Which fields mean something depends on [type]:
/// rect x/y (lower-left) w/h; circle x/y (centre) r; polygon x/y r sides;
/// text x/y (baseline-left) text size; path points closed; point x/y.
class Shape {
  static const rect = 'rect', circle = 'circle', polygon = 'polygon', text = 'text', path = 'path', point = 'point';

  /// The longest text phobiCCCP takes (src/padlink.cpp checkShape).
  static const maxText = 500;

  final String id, type;
  final double x, y;
  final double? w, h, r, size;
  final int? sides;
  final String? label;
  final List<List<double>>? points;
  final bool? closed;
  final Map<String, Object?> extra;

  static const _known = {'id', 'type', 'x', 'y', 'w', 'h', 'r', 'sides', 'text', 'size', 'points', 'closed'};

  const Shape({
    required this.id,
    required this.type,
    this.x = 0,
    this.y = 0,
    this.w,
    this.h,
    this.r,
    this.sides,
    this.label,
    this.size,
    this.points,
    this.closed,
    this.extra = const {},
  });

  factory Shape.fromJson(Map<String, Object?> j) => Shape(
        id: _s(j['id']),
        type: _s(j['type']),
        x: _d(j['x']),
        y: _d(j['y']),
        w: j['w'] is num ? _d(j['w']) : null,
        h: j['h'] is num ? _d(j['h']) : null,
        r: j['r'] is num ? _d(j['r']) : null,
        sides: j['sides'] is num ? _i(j['sides']) : null,
        label: j['text'] is String ? j['text'] as String : null,
        size: j['size'] is num ? _d(j['size']) : null,
        points: j['points'] is List
            ? [
                for (final p in _l(j['points']))
                  if (_l(p).length >= 2) [_d(_l(p)[0]), _d(_l(p)[1])]
              ]
            : null,
        closed: j['closed'] is bool ? j['closed'] as bool : null,
        extra: _extra(j, _known),
      );

  Map<String, Object?> toJson() => {
        ...extra,
        'id': id,
        'type': type,
        'x': x,
        'y': y,
        if (w != null) 'w': w,
        if (h != null) 'h': h,
        if (r != null) 'r': r,
        if (sides != null) 'sides': sides,
        if (label != null) 'text': label,
        if (size != null) 'size': size,
        if (points != null) 'points': points,
        if (closed != null) 'closed': closed,
      };

  Shape copyWith({double? x, double? y, double? w, double? h, double? r, int? sides, String? label, double? size,
          List<List<double>>? points, bool? closed}) =>
      Shape(
        id: id,
        type: type,
        x: x ?? this.x,
        y: y ?? this.y,
        w: w ?? this.w,
        h: h ?? this.h,
        r: r ?? this.r,
        sides: sides ?? this.sides,
        label: label ?? this.label,
        size: size ?? this.size,
        points: points ?? this.points,
        closed: closed ?? this.closed,
        extra: extra,
      );

  /// Moved by (dx, dy) mm.
  Shape moved(double dx, double dy) => type == path
      ? copyWith(points: points?.map((p) => [p[0] + dx, p[1] + dy]).toList())
      : copyWith(x: x + dx, y: y + dy);

  /// Lower-left and upper-right corner, roughly (text is guessed from its size).
  List<double> bounds() {
    switch (type) {
      case rect:
        return [x, y, x + (w ?? 0), y + (h ?? 0)];
      case circle:
      case polygon:
        final rr = r ?? 0;
        return [x - rr, y - rr, x + rr, y + rr];
      case text:
        final s = size ?? 10;
        return [x, y, x + s * 0.62 * math.max(1, label?.length ?? 1), y + s];
      case path:
        final p = points ?? const [];
        if (p.isEmpty) return [0, 0, 0, 0];
        return [p.map((q) => q[0]).reduce(math.min), p.map((q) => q[1]).reduce(math.min),
            p.map((q) => q[0]).reduce(math.max), p.map((q) => q[1]).reduce(math.max)];
      default:
        return [x - 1, y - 1, x + 1, y + 1];
    }
  }

  String describe() {
    String f(double? v) => fmt(v ?? 0);
    switch (type) {
      case rect:
        return 'Rectangle ${f(w)} × ${f(h)}';
      case circle:
        return 'Circle Ø${f((r ?? 0) * 2)}';
      case polygon:
        return '${sides ?? 6}-sided polygon';
      case text:
        return 'Text “${label ?? ''}”';
      case path:
        return '${closed == true ? 'Closed path' : 'Path'}, ${points?.length ?? 0} points';
      case point:
        return 'Drill point';
      default:
        return type;
    }
  }
}

class ToolpathSpec {
  static const types = ['profile', 'pocket', 'drill', 'engrave'];
  static const sideNames = ['outside', 'inside', 'on'];

  final String name;

  /// profile | pocket | drill | engrave
  final String type;
  final List<String> shapes;

  /// Tool library number on the desktop (#201 and so on).
  final int tool;

  /// Below the top of the stock, mm.
  final double depth;

  /// profile only: outside | inside | on
  final String side;

  /// Off: kept in the design but not cut (the desktop's tick box).
  final bool enabled;
  final Map<String, Object?> extra;

  static const _known = {'name', 'type', 'shapes', 'tool', 'depth', 'side', 'enabled'};

  const ToolpathSpec(
      {required this.name,
      required this.type,
      required this.shapes,
      required this.tool,
      required this.depth,
      this.side = 'outside',
      this.enabled = true,
      this.extra = const {}});

  factory ToolpathSpec.fromJson(Map<String, Object?> j) => ToolpathSpec(
        name: _s(j['name']),
        type: _s(j['type'], 'profile'),
        shapes: _l(j['shapes']).whereType<String>().toList(),
        tool: _i(j['tool'], 201),
        depth: _d(j['depth']),
        side: _s(j['side'], 'outside'),
        enabled: _b(j['enabled'], true),
        extra: _extra(j, _known),
      );
  Map<String, Object?> toJson() =>
      {...extra, 'name': name, 'type': type, 'shapes': shapes, 'tool': tool, 'depth': depth, 'side': side, 'enabled': enabled};

  ToolpathSpec copyWith(
          {String? name, String? type, List<String>? shapes, int? tool, double? depth, String? side, bool? enabled}) =>
      ToolpathSpec(
        name: name ?? this.name,
        type: type ?? this.type,
        shapes: shapes ?? this.shapes,
        tool: tool ?? this.tool,
        depth: depth ?? this.depth,
        side: side ?? this.side,
        enabled: enabled ?? this.enabled,
        extra: extra,
      );

  static String typeLabel(String t) =>
      const {'profile': 'Profile', 'pocket': 'Pocket', 'drill': 'Drill', 'engrave': 'Engrave'}[t] ?? t;
}

class Design {
  final String id, name, material;

  /// Milliseconds since 1970, set by whoever edited it last. The newer copy wins a sync.
  final int modified;

  /// The [modified] of the copy this pad last got from or gave to a
  /// phobiCCCP, from before [bases]: used only where [bases] says nothing.
  /// 0: not known.
  final int base;

  /// Per phobiCCCP (by its id), the [modified] of the copy this pad last got
  /// from or gave to it. Edits here keep them, so a computer can tell an edit
  /// of its own copy from one of an older copy (and then keeps both). Kept on
  /// the pad only: a put carries just the one of the computer it goes to, as
  /// "base" ([toWire]). One base for all computers would let a copy given to
  /// one count as the base at another that never had it.
  final Map<String, int> bases;

  /// Version vector: the [modified] of the latest edit each pad or phobiCCCP
  /// (by its own id) made to this design. It travels with the design, so any
  /// computer can tell a copy made from another from one edited apart from it,
  /// however the copies went round (src/padlink.h, "Versions").
  final Map<String, int> vv;

  /// A removal, kept so it reaches every phobiCCCP too.
  final bool deleted;
  final Stock stock;
  final List<Shape> shapes;
  final List<ToolpathSpec> toolpaths;
  final Map<String, Object?> extra;

  static const _known = {
    'format', 'version', 'id', 'name', 'modified', 'base', 'bases', 'vv', 'deleted', 'stock', 'material', 'shapes', 'toolpaths'
  };

  Design({
    String? id,
    this.name = '',
    this.modified = 0,
    this.base = 0,
    this.bases = const {},
    this.vv = const {},
    this.deleted = false,
    this.stock = const Stock(),
    this.material = 'softwood',
    this.shapes = const [],
    this.toolpaths = const [],
    this.extra = const {},
  }) : id = id ?? newId();

  factory Design.fromJson(Map<String, Object?> j) => Design(
        id: _s(j['id']),
        name: _s(j['name']),
        modified: _i(j['modified']),
        base: _i(j['base']),
        bases: _ints(j['bases']),
        vv: _ints(j['vv']),
        deleted: _b(j['deleted']),
        stock: Stock.fromJson(_m(j['stock'])),
        material: _s(j['material'], 'softwood'),
        shapes: _l(j['shapes']).map((s) => Shape.fromJson(_m(s))).toList(),
        toolpaths: _l(j['toolpaths']).map((t) => ToolpathSpec.fromJson(_m(t))).toList(),
        extra: _extra(j, _known),
      );

  Map<String, Object?> toJson() => {
        ...extra,
        'format': padFormat,
        'version': 1,
        'id': id,
        'name': name,
        'modified': modified,
        if (base > 0) 'base': base,
        if (bases.isNotEmpty) 'bases': bases,
        if (vv.isNotEmpty) 'vv': vv,
        'deleted': deleted,
        'stock': stock.toJson(),
        'material': material,
        'shapes': shapes.map((s) => s.toJson()).toList(),
        'toolpaths': toolpaths.map((t) => t.toJson()).toList(),
      };

  Design copyWith(
          {String? id,
          String? name,
          int? modified,
          int? base,
          Map<String, int>? bases,
          Map<String, int>? vv,
          bool? deleted,
          Stock? stock,
          String? material,
          List<Shape>? shapes,
          List<ToolpathSpec>? toolpaths}) =>
      Design(
        id: id ?? this.id,
        name: name ?? this.name,
        modified: modified ?? this.modified,
        base: base ?? this.base,
        bases: bases ?? this.bases,
        vv: vv ?? this.vv,
        deleted: deleted ?? this.deleted,
        stock: stock ?? this.stock,
        material: material ?? this.material,
        shapes: shapes ?? this.shapes,
        toolpaths: toolpaths ?? this.toolpaths,
        extra: extra,
      );

  /// The base for the phobiCCCP [desktop]: what this pad last exchanged with
  /// it, else (never exchanged there) the oldest it exchanged anywhere, so a
  /// computer that changed it since still keeps both.
  int baseFor(String desktop) => bases[desktop] ?? (bases.isEmpty ? base : bases.values.reduce(math.min));

  /// With [modified] noted as exchanged with [desktop].
  Design withBase(String desktop, int modified) => copyWith(bases: {...bases, desktop: modified});

  /// As sent to [desktop]: its own base, and none of the others.
  Map<String, Object?> toWire(String desktop) {
    final j = toJson()..remove('bases')..remove('base');
    final b = baseFor(desktop);
    if (b > 0) j['base'] = b;
    return j;
  }

  Order orderTo(Map<String, int> otherVv, int otherModified, {bool otherDeleted = false}) =>
      orderOf(vv, modified, otherVv, otherModified, aDeleted: deleted, bDeleted: otherDeleted);

  /// [other] is this very copy (its bases aside). The time alone is not
  /// enough: two copies edited apart can carry the same time, and a computer
  /// that settled them may have kept the other one.
  bool isSameCopy(Design other) =>
      modified == other.modified &&
      deleted == other.deleted &&
      vv.length == other.vv.length &&
      vv.entries.every((e) => other.vv[e.key] == e.value);

  /// The computer that will not rebuild its linked file from a pad edit (it
  /// holds curves, tabs, parameters, ... the pad cannot), as that computer
  /// said when it gave this copy; empty when none. A pad edit then reaches it
  /// as a copy beside the file.
  String get desktopOnly {
    final v = extra['desktopOnly'];
    return v is String ? v : (v == true ? 'the computer it came from' : '');
  }

  Shape? shape(String id) {
    for (final s in shapes) {
      if (s.id == id) return s;
    }
    return null;
  }

  String nextShapeId() {
    var n = shapes.length + 1;
    while (shapes.any((s) => s.id == 's$n')) {
      n++;
    }
    return 's$n';
  }
}

/// A phobiCCCP found on the network (or typed in), as its discovery answer says.
class Desktop {
  /// This install of phobiCCCP; two computers with the same name still differ.
  final String id, host, address, version, machine;
  final int port;
  final int lastSeen;

  /// The name or address it was added by, when added by hand (Tailscale, another subnet).
  final String typed;

  const Desktop({
    required this.id,
    required this.host,
    required this.address,
    this.port = 47652,
    this.version = '',
    this.machine = '',
    this.lastSeen = 0,
    this.typed = '',
  });

  bool get manual => typed.isNotEmpty;
  bool online(int now) => now - lastSeen < onlineMs;
  static const onlineMs = 15000;

  Desktop seenAt(int t) =>
      Desktop(id: id, host: host, address: address, port: port, version: version, machine: machine, lastSeen: t, typed: typed);
  Desktop withTyped(String h) =>
      Desktop(id: id, host: host, address: address, port: port, version: version, machine: machine, lastSeen: lastSeen, typed: h);
}

class MachineInfo {
  final String id, name, size;
  final bool active;
  const MachineInfo({required this.id, required this.name, this.active = false, this.size = ''});
  factory MachineInfo.fromJson(Map<String, Object?> j) =>
      MachineInfo(id: _s(j['id']), name: _s(j['name']), active: _b(j['active']), size: _s(j['size']));
}

class MachineStatus {
  final bool connected, homed, busy, streaming;
  final String port, state, job;
  const MachineStatus(
      {this.connected = false, this.homed = false, this.busy = false, this.streaming = false, this.port = '', this.state = '', this.job = ''});
  factory MachineStatus.fromJson(Map<String, Object?> j) => MachineStatus(
        connected: _b(j['connected']),
        homed: _b(j['homed']),
        busy: _b(j['busy']),
        streaming: _b(j['streaming']),
        port: _s(j['port']),
        state: _s(j['state']),
        job: _s(j['job']),
      );

  String words() {
    if (!connected) return 'Not connected';
    if (streaming) return job.isEmpty ? 'Cutting' : 'Cutting $job';
    if (state.startsWith('Alarm')) return 'Alarm: needs Initialize';
    if (!homed) return 'Connected, not homed';
    if (busy) return 'Busy ($state)';
    if (state == 'Idle') return 'Idle, ready';
    return state.isEmpty ? 'Connected' : state;
  }

  /// What the desktop checks before it starts a job from the pad.
  bool get readyToCut => connected && homed && !busy && state == 'Idle';
}

/// The answer to "info".
class DesktopInfo {
  final String host, version, id, activeMachine, document;
  final List<MachineInfo> machines;
  final MachineStatus status;
  final bool allowStart;

  /// It keeps two copies edited apart itself, by their version vectors
  /// (phobiCCCP 1.2.9 and later). An older one lists no versions and keeps
  /// the later copy or forks by the base alone; the pad then settles such
  /// pairs for it (sync.dart).
  bool get keepsBoth {
    final v = [for (final p in version.split('.')) int.tryParse(p.split(RegExp(r'[^0-9]')).first) ?? 0];
    while (v.length < 3) {
      v.add(0);
    }
    return v[0] != 1 ? v[0] > 1 : (v[1] != 2 ? v[1] > 2 : v[2] >= 9);
  }

  const DesktopInfo({
    this.host = '',
    this.version = '',
    this.id = '',
    this.activeMachine = '',
    this.document = '',
    this.machines = const [],
    this.status = const MachineStatus(),
    this.allowStart = false,
  });
  factory DesktopInfo.fromJson(Map<String, Object?> j) => DesktopInfo(
        host: _s(j['host']),
        version: _s(j['version']),
        id: _s(j['id']),
        activeMachine: _s(j['activeMachine']),
        document: _s(j['document']),
        machines: _l(j['machines']).map((m) => MachineInfo.fromJson(_m(m))).toList(),
        status: MachineStatus.fromJson(_m(j['status'])),
        allowStart: _b(j['allowStart']),
      );
}

class Tool {
  final int number, flutes;
  final String name, kind;
  final double diameter, angle;

  /// Materials the library has feeds and speeds for.
  final List<String> materials;
  const Tool(
      {required this.number, required this.name, this.kind = '', this.diameter = 0, this.angle = 0, this.flutes = 0, this.materials = const []});
  factory Tool.fromJson(Map<String, Object?> j) => Tool(
        number: _i(j['number']),
        name: _s(j['name']),
        kind: _s(j['kind']),
        diameter: _d(j['diameter']),
        angle: _d(j['angle']),
        flutes: _i(j['flutes']),
        materials: _l(j['materials']).whereType<String>().toList(),
      );
  Map<String, Object?> toJson() => {
        'number': number,
        'name': name,
        'kind': kind,
        'diameter': diameter,
        'angle': angle,
        'flutes': flutes,
        'materials': materials,
      };
}

class MaterialInfo {
  final String id, name;
  const MaterialInfo(this.id, this.name);
}

/// The desktop's tool library, as "tools" answers it.
class ToolLibrary {
  final List<MaterialInfo> materials;
  final List<Tool> tools;
  const ToolLibrary({this.materials = const [], this.tools = const []});

  factory ToolLibrary.fromJson(Map<String, Object?> j) => ToolLibrary(
        materials: _l(j['materials']).map((m) => MaterialInfo(_s(_m(m)['id']), _s(_m(m)['name']))).toList(),
        tools: _l(j['tools']).map((t) => Tool.fromJson(_m(t))).toList(),
      );
  Map<String, Object?> toJson() => {
        'materials': materials.map((m) => {'id': m.id, 'name': m.name}).toList(),
        'tools': tools.map((t) => t.toJson()).toList(),
      };

  bool get isEmpty => tools.isEmpty;
  List<Tool> toolsFor(String material) => tools.where((t) => t.materials.contains(material)).toList();
  Tool? tool(int number) {
    for (final t in tools) {
      if (t.number == number) return t;
    }
    return null;
  }

  String materialName(String id) {
    for (final m in materials) {
      if (m.id == id) return m.name;
    }
    return id;
  }

  /// A sensible first tool for a kind of toolpath, from what there is for the material.
  int? defaultTool(String material, String type) {
    final have = toolsFor(material);
    final wanted = type == 'engrave' ? ['engraver', 'vbit', 'endmill'] : ['endmill', 'ball'];
    for (final k in wanted) {
      for (final t in have) {
        if (t.kind == k) return t.number;
      }
    }
    return have.isEmpty ? null : have.first.number;
  }
}

/// One row of "designs".
class DesignEntry {
  final String id, name;
  final int modified;
  final bool deleted;

  /// Its version vector ([Design.vv]); empty from an older phobiCCCP.
  final Map<String, int> vv;
  const DesignEntry(this.id, {this.name = '', this.modified = 0, this.deleted = false, this.vv = const {}});
  factory DesignEntry.fromJson(Map<String, Object?> j) => DesignEntry(_s(j['id']),
      name: _s(j['name']), modified: _i(j['modified']), deleted: _b(j['deleted']), vv: _ints(j['vv']));
}

/// The answer to "send".
class SendResult {
  final bool started;
  final String message;
  final List<String> notes;

  /// As for a put: the id the design was kept under there, as it had changed
  /// there since this pad's copy, and the time of the copy kept there (null
  /// when it was not kept, or from an older phobiCCCP).
  final String forked;

  /// With [forked]: the pad's was the later edit, so it is kept under its own
  /// id there and the computer's copy went to [forked] (src/padlink.h).
  final bool moved;
  final int? modified;
  const SendResult({this.started = false, this.message = '', this.notes = const [], this.forked = '', this.moved = false, this.modified});
  factory SendResult.fromJson(Map<String, Object?> j) => SendResult(
      started: _b(j['started']),
      message: _s(j['message']),
      notes: _l(j['notes']).whereType<String>().toList(),
      forked: _s(j['forked']),
      moved: j['moved'] == true,
      modified: j['modified'] is num ? (j['modified'] as num).toInt() : null);
}

String fmt(double v) {
  if (v == v.roundToDouble() && v.abs() < 1e9) return v.toInt().toString();
  var s = v.toStringAsFixed(3);
  while (s.endsWith('0')) {
    s = s.substring(0, s.length - 1);
  }
  return s.endsWith('.') ? s.substring(0, s.length - 1) : s;
}
