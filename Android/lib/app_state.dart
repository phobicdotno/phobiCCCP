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
  final int lastSync, toolsAt;
  const Computer(this.desktop, {this.info, this.error, this.lastSync = 0, this.toolsAt = 0});

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
  ToolLibrary tools;
  bool syncing = false;

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
    _tick();
    _timer = Timer.periodic(round, (_) => _tick());
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
    } finally {
      _busy = false;
    }
  }

  Future<void> syncNow() => _tick();

  Future<void> _discover() async {
    List<Desktop> found;
    try {
      found = await Discovery.probe(manualHosts: settings.manualHosts);
    } catch (_) {
      found = [];
    }
    final byId = {for (final c in computers) c.desktop.id: c};
    for (final d in found) {
      final c = byId[d.id];
      byId[d.id] = c == null ? Computer(d) : Computer(d, info: c.info, error: c.error, lastSync: c.lastSync, toolsAt: c.toolsAt);
    }
    // A computer typed in but not answering still shows, as offline.
    byId.removeWhere((k, _) => k.startsWith('manual:'));
    for (final h in settings.manualHosts) {
      if (!byId.values.any((c) => c.desktop.typed == h)) {
        byId['manual:$h'] = Computer(Desktop(id: 'manual:$h', host: h, address: h, typed: h));
      }
    }
    computers = byId.values.toList()..sort((a, b) => a.name.toLowerCase().compareTo(b.name.toLowerCase()));
    _changed();
  }

  Future<void> _syncAll() async {
    final t = now;
    // Typed-in ones are tried too: discovery datagrams may not get through where TCP does.
    final targets = computers.where((c) => c.desktop.online(t) || c.desktop.manual).toList();
    if (targets.isEmpty) return;
    syncing = true;
    _changed();
    for (final c in targets) {
      final withTools = t - c.toolsAt > toolsEvery || tools.isEmpty;
      Computer next;
      try {
        final r = await syncWith(c.desktop, store, withTools: withTools);
        next = Computer(c.desktop.seenAt(now), info: r.info, lastSync: now, toolsAt: r.tools != null ? now : c.toolsAt);
        final lib = r.tools;
        if (lib != null && !lib.isEmpty) {
          tools = lib;
          settings.tools = lib;
        }
      } catch (e) {
        next = Computer(c.desktop, info: c.info, error: '$e', lastSync: c.lastSync, toolsAt: c.toolsAt);
      }
      computers = [for (final x in computers) x.desktop.id == c.desktop.id ? next : x];
      _changed();
    }
    syncing = false;
    designs = store.live();
    _changed();
  }

  /// Sync shortly, once the edits stop coming.
  void _syncSoon() {
    _soon?.cancel();
    _soon = Timer(const Duration(milliseconds: 1500), () {
      if (_busy) {
        _syncSoon();
      } else {
        _busy = true;
        _syncAll().whenComplete(() => _busy = false);
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
    final copy = store.save(d.copyWith(id: newId(), name: '${d.name} copy'));
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
        x.desktop.id == c.desktop.id ? Computer(x.desktop, info: info, lastSync: x.lastSync, toolsAt: x.toolsAt) : x
    ];
    _changed();
  }

  /// Send [d] to open on that computer (and machine), and start it when asked.
  Future<SendResult> send(Design d, Computer c, String machineId, bool start) async {
    try {
      return await CccpConnection.use(
          c.desktop.address, c.desktop.port, (k) => k.send(d, machine: machineId, start: start, from: deviceName));
    } finally {
      unawaited(syncNow());
    }
  }
}
