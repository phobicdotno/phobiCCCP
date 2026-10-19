// Designs and what phobiCCCP says about itself, in the JSON the desktop reads
// and writes (src/padlink.h and src/projectreceiver.h in phobiCCCP).
// Millimetres, X right, Y up, X0 Y0 at the stock's front-left corner.

import 'dart:math' as math;

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

  /// A removal, kept so it reaches every phobiCCCP too.
  final bool deleted;
  final Stock stock;
  final List<Shape> shapes;
  final List<ToolpathSpec> toolpaths;
  final Map<String, Object?> extra;

  static const _known = {'format', 'version', 'id', 'name', 'modified', 'deleted', 'stock', 'material', 'shapes', 'toolpaths'};

  Design({
    String? id,
    this.name = '',
    this.modified = 0,
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
        'deleted': deleted,
        'stock': stock.toJson(),
        'material': material,
        'shapes': shapes.map((s) => s.toJson()).toList(),
        'toolpaths': toolpaths.map((t) => t.toJson()).toList(),
      };

  Design copyWith({String? id, String? name, int? modified, bool? deleted, Stock? stock, String? material,
          List<Shape>? shapes, List<ToolpathSpec>? toolpaths}) =>
      Design(
        id: id ?? this.id,
        name: name ?? this.name,
        modified: modified ?? this.modified,
        deleted: deleted ?? this.deleted,
        stock: stock ?? this.stock,
        material: material ?? this.material,
        shapes: shapes ?? this.shapes,
        toolpaths: toolpaths ?? this.toolpaths,
        extra: extra,
      );

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
  const DesignEntry(this.id, {this.name = '', this.modified = 0, this.deleted = false});
  factory DesignEntry.fromJson(Map<String, Object?> j) =>
      DesignEntry(_s(j['id']), name: _s(j['name']), modified: _i(j['modified']), deleted: _b(j['deleted']));
}

/// The answer to "send".
class SendResult {
  final bool started;
  final String message;
  final List<String> notes;
  const SendResult({this.started = false, this.message = '', this.notes = const []});
  factory SendResult.fromJson(Map<String, Object?> j) => SendResult(
      started: _b(j['started']), message: _s(j['message']), notes: _l(j['notes']).whereType<String>().toList());
}

String fmt(double v) {
  if (v == v.roundToDouble() && v.abs() < 1e9) return v.toInt().toString();
  var s = v.toStringAsFixed(3);
  while (s.endsWith('0')) {
    s = s.substring(0, s.length - 1);
  }
  return s.endsWith('.') ? s.substring(0, s.length - 1) : s;
}
