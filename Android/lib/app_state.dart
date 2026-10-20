// Keeps the pad in step with every phobiCCCP on the network while the app is
// open: looks for them every few seconds, and syncs designs, tool library and
// machine status with each one it can reach, straight away after an edit.

import 'dart:async';
import 'dart:io';

import 'package:flutter/foundation.dart';

import 'model.dart';
import 'net.dart';
import 'store.dart';
import 'sync.dart';

/// A phobiCCCP the pad knows, with what it said last.
class Computer {
  final Desktop desktop;
  final DesktopInfo? info;
  final String? error;

  /// Designs it would not take; it still answers, and can still be sent to.
  final String? warning;
  final int lastSync, toolsAt;
  const Computer(this.desktop, {this.info, this.error, this.warning, this.lastSync = 0, this.toolsAt = 0});

  String get name => desktop.host.isEmpty ? desktop.address : desktop.host;
  bool reachable(int now) => info != null && error == null && desktop.online(now);
}

class AppState extends ChangeNotifier {
  static const round = Duration(seconds: 5);
  static const toolsEvery = 120000;

  final DesignStore store;
  final Settings settings;
  final String deviceName;

  List<Computer> computers = [];
  List<Design> designs = [];

  /// The library from the computer synced last, for picking tools; each
  /// computer's own is [toolsOf].
  ToolLibrary tools;
  bool syncing = false;

  /// Something a sync did that the user should hear about (a design changed
  /// both here and on a computer); [noticeCount] goes up with each new one.
  String notice = '';
  int noticeCount = 0;

  Timer? _timer, _soon;
  bool _busy = false;
  bool _disposed = false;

  AppState(this.store, this.settings, {String? deviceName})
      : tools = settings.tools,
        deviceName = deviceName ?? _hostName() {
    designs = store.live();
  }

  static String _hostName() {
    try {
      return Platform.localHostname;
    } catch (_) {
      return 'pad';
    }
  }

  int get now => DateTime.now().millisecondsSinceEpoch;
  Set<String> get manualHosts => settings.manualHosts;

  void start() {
    unawaited(_tick());
    _timer = Timer.periodic(round, (_) => unawaited(_tick()));
  }

  @override
  void dispose() {
    _disposed = true;
    _timer?.cancel();
    _soon?.cancel();
    super.dispose();
  }

  void _changed() {
    if (!_disposed) notifyListeners();
  }

  Future<void> _tick() async {
    if (_busy) return;
    _busy = true;
    try {
      await _discover();
      await _syncAll();
    } catch (_) {
      // Shown per computer; nothing else to do until the next round.
    } finally {
      _busy = false;
    }
  }

  Future<void> syncNow() => _tick();

  /// One row per phobiCCCP: a computer typed in is also heard on the LAN, or
  /// answers a sync under its own id, and must not then show twice.
  static List<Computer> dedupe(List<Computer> list) {
    final byId = <String, Computer>{};
    for (final c in list) {
      if (!c.desktop.id.startsWith('manual:')) byId[c.desktop.id] = c;
    }
    final out = <Computer>[];
    for (final c in list) {
      if (!c.desktop.id.startsWith('manual:')) continue;
      final same = byId[c.info?.id ?? ''];
      if (same == null) {
        out.add(c);
      } else if (same.desktop.typed.isEmpty) {
        byId[same.desktop.id] = Computer(same.desktop.withTyped(c.desktop.typed),
            info: same.info, error: same.error, warning: same.warning, lastSync: same.lastSync, toolsAt: same.toolsAt);
      }
    }
    return [...byId.values, ...out]..sort((a, b) => a.name.toLowerCase().compareTo(b.name.toLowerCase()));
  }

  ToolLibrary toolsOf(String desktopId) => settings.toolsOf(desktopId);

  Future<void> _discover() async {
    List<Desktop> found;
    try {
      found = await Discovery.probe(manualHosts: settings.manualHosts);
    } catch (_) {
      found = [];
    }
    final byId = {for (final c in computers) c.desktop.id: c};
    for (final d0 in found) {
      final c = byId[d0.id];
      // Heard on the LAN too: still the one typed in, not a second computer.
      final d = d0.typed.isEmpty && c != null && c.desktop.typed.isNotEmpty ? d0.withTyped(c.desktop.typed) : d0;
      byId[d.id] = c == null
          ? Computer(d)
          : Computer(d, info: c.info, error: c.error, warning: c.warning, lastSync: c.lastSync, toolsAt: c.toolsAt);
    }
    // A computer typed in but not answering still shows, as offline; one
    // already listed keeps what its last sync learned.
    final manual = settings.manualHosts;
    byId.removeWhere((k, c) =>
        k.startsWith('manual:') && (!manual.contains(c.desktop.typed) || byId.values.any((o) => !o.desktop.id.startsWith('manual:') && o.desktop.typed == c.desktop.typed)));
    for (final h in manual) {
      if (!byId.values.any((c) => c.desktop.typed == h)) {
        byId['manual:$h'] = Computer(Desktop(id: 'manual:$h', host: h, address: h, typed: h));
      }
    }
    computers = dedupe(byId.values.toList());
    _changed();
  }

  Future<void> _syncAll() async {
    final t = now;
    // Typed-in ones are tried too: discovery datagrams may not get through where TCP does.
    final targets = computers.where((c) => c.desktop.online(t) || c.desktop.manual).toList();
    if (targets.isEmpty) return;
    syncing = true;
    _changed();
    try {
      for (final c in targets) {
        final withTools = t - c.toolsAt > toolsEvery || tools.isEmpty || settings.toolsOf(c.desktop.id).isEmpty;
        Computer next;
        try {
          final r = await syncWith(c.desktop, store, withTools: withTools, onPulled: () {
            designs = store.live();
            _changed();
          });
          next = Computer(c.desktop.seenAt(now),
              info: r.info,
              warning: r.refused.isEmpty ? null : 'Not taken: ${r.refused.join('; ')}',
              lastSync: now,
              toolsAt: r.tools != null ? now : c.toolsAt);
          final lib = r.tools;
          if (lib != null && !lib.isEmpty) {
            tools = lib;
            settings.tools = lib;
            settings.setToolsOf(c.desktop.id, lib);
            // A typed-in computer answers under its own id too.
            final id = r.info.id;
            if (id.isNotEmpty && id != c.desktop.id) settings.setToolsOf(id, lib);
          }
          if (r.conflicts.isNotEmpty) {
            notice = r.conflicts.join('; ');
            noticeCount++;
          }
        } catch (e) {
          next = Computer(c.desktop, info: c.info, error: '$e', lastSync: c.lastSync, toolsAt: c.toolsAt);
        }
        computers = dedupe([for (final x in computers) x.desktop.id == c.desktop.id ? next : x]);
        _changed();
      }
    } finally {
      syncing = false;
      try {
        designs = store.live();
      } catch (_) {}
      _changed();
    }
  }

  /// Sync shortly, once the edits stop coming.
  void _syncSoon() {
    _soon?.cancel();
    _soon = Timer(const Duration(milliseconds: 1500), () {
      if (_busy) {
        _syncSoon();
      } else {
        _busy = true;
        unawaited(_syncAll().catchError((_) {}).whenComplete(() => _busy = false));
      }
    });
  }

  Design? design(String id) => store.get(id);

  Design newDesign(String name, Stock stock, String material) {
    final d = store.save(Design(name: name.trim().isEmpty ? 'Design' : name.trim(), stock: stock, material: material));
    designs = store.live();
    _changed();
    _syncSoon();
    return d;
  }

  Design saveDesign(Design d) {
    final s = store.save(d);
    designs = store.live();
    _changed();
    _syncSoon();
    return s;
  }

  void deleteDesign(String id) {
    store.delete(id);
    designs = store.live();
    _changed();
    _syncSoon();
  }

  Design? duplicateDesign(String id) {
    final d = store.get(id);
    if (d == null) return null;
    // A new design: it was never exchanged with any computer.
    final copy = store.save(d.copyWith(id: newId(), name: '${d.name} copy', base: 0));
    designs = store.live();
    _changed();
    _syncSoon();
    return copy;
  }

  void addHost(String host) {
    final h = host.trim();
    if (h.isEmpty) return;
    settings.manualHosts = {...settings.manualHosts, h};
    _changed();
    syncNow();
  }

  void removeHost(String host) {
    settings.manualHosts = settings.manualHosts.difference({host});
    computers = computers.where((c) => c.desktop.typed != host).toList();
    _changed();
  }

  String get lastDesign => settings.lastDesign;
  set lastDesign(String v) => settings.lastDesign = v;

  String get lastTarget => settings.lastTarget;
  set lastTarget(String v) => settings.lastTarget = v;

  /// Make [machineId] the machine in use on that computer.
  Future<void> selectMachine(Computer c, String machineId) async {
    final info = await CccpConnection.use(c.desktop.address, c.desktop.port, (k) => k.selectMachine(machineId));
    computers = [
      for (final x in computers)
        x.desktop.id == c.desktop.id ? Computer(x.desktop, info: info, warning: x.warning, lastSync: x.lastSync, toolsAt: x.toolsAt) : x
    ];
    _changed();
  }

  /// Send [d] to open on that computer (and machine), and start it when asked.
  Future<SendResult> send(Design d, Computer c, String machineId, bool start) async {
    try {
      return await CccpConnection.use(c.desktop.address, c.desktop.port, (k) async {
        final r = await k.send(d, machine: machineId, start: start, from: deviceName);
        // It got there; what it did with the copy is for the sync to sort out
        // if this part fails.
        try {
          final note = await settleSend(k, store, d, r, c.name);
          if (note != null) {
            notice = note;
            noticeCount++;
          }
          designs = store.live();
          _changed();
        } catch (_) {}
        return r;
      });
    } finally {
      unawaited(syncNow());
    }
  }
}
