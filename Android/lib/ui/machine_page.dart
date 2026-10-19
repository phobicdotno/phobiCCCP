// 3 · Machine: the pad's counterpart of the desktop's Machine page
// (machinepanel.cpp): the phobiCCCPs on the network and their machines on the
// left, the job and the big amber Start on the right.

import 'package:flutter/material.dart';

import '../app_state.dart';
import '../model.dart';
import 'send_dialog.dart';
import 'theme.dart';
import 'widgets.dart';

class MachinePage extends StatefulWidget {
  final AppState app;
  final Design? design;
  final ValueChanged<String> onStatus;
  final VoidCallback onAddComputer;
  const MachinePage({super.key, required this.app, required this.design, required this.onStatus, required this.onAddComputer});
  @override
  State<MachinePage> createState() => _MachinePageState();
}

class _MachinePageState extends State<MachinePage> {
  String? _computer, _machine;
  bool _busy = false;
  AppState get app => widget.app;

  List<Computer> _reach() => app.computers.where((c) => c.reachable(app.now)).toList();

  Computer? _target() {
    final reach = _reach();
    final last = app.lastTarget.split('/');
    final id = _computer ?? last.first;
    return reach.where((c) => c.desktop.id == id).firstOrNull ?? reach.firstOrNull;
  }

  String? _targetMachine(Computer c) {
    final ms = c.info?.machines ?? const [];
    final last = app.lastTarget.split('/');
    final want = _machine ?? (last.length > 1 && last.first == c.desktop.id ? last[1] : null);
    return ms.any((m) => m.id == want) ? want : c.info?.activeMachine;
  }

  static const _caps = TextStyle(fontSize: 12, color: C.label, letterSpacing: 1.2, fontWeight: FontWeight.w500);

  Future<void> _send(bool start) async {
    final d = widget.design;
    final c = _target();
    if (d == null || c == null) return;
    final m = _targetMachine(c) ?? '';
    if (start) {
      final name = c.info?.machines.where((x) => x.id == m).firstOrNull?.name ?? 'the machine';
      if (!await confirmStart(context, d.name, name, c.name)) return;
    }
    setState(() => _busy = true);
    app.lastTarget = '${c.desktop.id}/$m';
    try {
      final r = await app.send(d, c, m, start);
      widget.onStatus(sendWords(r, start, c.name));
    } catch (e) {
      widget.onStatus('Not sent: $e');
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: app,
      builder: (context, _) => Padding(
        padding: const EdgeInsets.all(14),
        child: Row(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
          Expanded(child: _panel(_computers())),
          const SizedBox(width: 12),
          Expanded(child: _panel(_job())),
        ]),
      ),
    );
  }

  Widget _panel(Widget child) => Container(
        decoration: BoxDecoration(color: C.card, borderRadius: BorderRadius.circular(10)),
        padding: const EdgeInsets.all(18),
        child: child,
      );

  Widget _computers() {
    final now = app.now;
    return Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
      const Text('PHOBICCCP ON THIS NETWORK', style: _caps),
      const SizedBox(height: 10),
      Expanded(
        child: app.computers.isEmpty
            ? const Text(
                'Looking for phobiCCCP… Open it on a computer on the same network, with File › Receive from Syntexophobic '
                'and pads on. Over Tailscale or another subnet, add it by name or address below.',
                style: TextStyle(color: C.sub, fontSize: 13))
            : ListView(children: [for (final c in app.computers) _computerBox(c, now)]),
      ),
      const SizedBox(height: 10),
      Row(children: [
        Expanded(child: PushBtn('Add computer…', height: 46, fontSize: 15, onTap: widget.onAddComputer)),
        const SizedBox(width: 10),
        Expanded(child: PushBtn('Sync now', height: 46, fontSize: 15, onTap: app.syncNow)),
      ]),
    ]);
  }

  Widget _computerBox(Computer c, int now) {
    final info = c.info;
    final online = c.reachable(now);
    final target = _target()?.desktop.id == c.desktop.id;
    return Container(
      margin: const EdgeInsets.only(bottom: 10),
      padding: const EdgeInsets.all(12),
      decoration: BoxDecoration(
        color: C.window,
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: target && online ? C.amber : C.cardBorder, width: target && online ? 2 : 1),
      ),
      child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        InkWell(
          onTap: online ? () => setState(() {
                _computer = c.desktop.id;
                _machine = null;
              }) : null,
          child: Row(children: [
            Container(width: 10, height: 10, decoration: BoxDecoration(shape: BoxShape.circle, color: online ? C.preview : C.disabled)),
            const SizedBox(width: 10),
            Expanded(
              child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                Text(c.name, style: const TextStyle(fontSize: 16, fontWeight: FontWeight.w600)),
                Text(
                    [
                      if (c.desktop.address != c.name) c.desktop.address,
                      if (info != null && info.version.isNotEmpty) 'phobiCCCP ${info.version}',
                      if (!online) 'not answering',
                    ].join(' · '),
                    style: const TextStyle(color: C.sub, fontSize: 12)),
              ]),
            ),
            if (c.desktop.manual)
              InkWell(
                onTap: () => app.removeHost(c.desktop.typed),
                child: const Padding(padding: EdgeInsets.all(6), child: Text('Forget', style: TextStyle(color: C.label, fontSize: 13))),
              ),
          ]),
        ),
        if (c.error != null && !online)
          Padding(
            padding: const EdgeInsets.only(top: 6),
            child: Text(c.error!, style: const TextStyle(color: Color(0xFFE05050), fontSize: 12)),
          ),
        if (info != null) ...[
          const SizedBox(height: 8),
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
            decoration: BoxDecoration(color: C.card, borderRadius: BorderRadius.circular(8), border: Border.all(color: C.cardBorder)),
            child: Text(info.status.words() + (info.document.isNotEmpty ? ' · ${info.document}' : ''),
                style: TextStyle(fontWeight: FontWeight.w600, color: info.status.readyToCut ? C.preview : C.text)),
          ),
          const SizedBox(height: 6),
          for (final m in info.machines)
            InkWell(
              onTap: !online || m.active
                  ? null
                  : () async {
                      try {
                        await app.selectMachine(c, m.id);
                        widget.onStatus('${c.name} now uses ${m.name}');
                      } catch (e) {
                        widget.onStatus('Could not switch: $e');
                      }
                    },
              child: Padding(
                padding: const EdgeInsets.symmetric(vertical: 6),
                child: Row(children: [
                  Container(
                    width: 18,
                    height: 18,
                    decoration: BoxDecoration(shape: BoxShape.circle, border: Border.all(color: m.active ? C.amber : C.disabled, width: 2)),
                    child: m.active
                        ? Center(
                            child: Container(width: 8, height: 8, decoration: const BoxDecoration(shape: BoxShape.circle, color: C.amber)))
                        : null,
                  ),
                  const SizedBox(width: 10),
                  Text(m.name, style: const TextStyle(fontSize: 14)),
                  if (m.size.isNotEmpty) Text('   ${m.size}', style: const TextStyle(color: C.sub, fontSize: 12)),
                ]),
              ),
            ),
          Text(info.allowStart ? 'Pads may start cutting here.' : 'Starting from a pad is off here (Machine › Pads may start cutting).',
              style: const TextStyle(color: C.sub, fontSize: 12)),
        ],
      ]),
    );
  }

  Widget _job() {
    final d = widget.design;
    final c = _target();
    final info = c?.info;
    final m = c == null ? null : _targetMachine(c);
    final machine = info?.machines.where((x) => x.id == m).firstOrNull;
    String? warn;
    if (d == null) {
      warn = 'Open a design first (File › Open).';
    } else if (c == null) {
      warn = 'No phobiCCCP is answering right now.';
    } else if (d.toolpaths.where((t) => t.enabled).isEmpty) {
      warn = 'This design has no toolpaths, so there is nothing to cut.';
    } else if (info != null && !info.allowStart) {
      warn = 'Start is off on ${c.name} (Machine › Pads may start cutting there); Send still opens it.';
    } else if (m != null && m != info?.activeMachine) {
      warn = 'Starting switches ${c.name} to ${machine?.name ?? 'that machine'}; it will most likely need Initialize first.';
    } else if (info != null && !info.status.readyToCut) {
      warn = 'The machine is not ready (${info.status.words()}); the design will be opened, not started.';
    }
    final canStart = d != null && c != null && !_busy && d.toolpaths.any((t) => t.enabled);
    return Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
      const Text('JOB', style: _caps),
      const SizedBox(height: 10),
      Text(d == null ? 'No design open' : (d.name.isEmpty ? 'Untitled' : d.name),
          style: const TextStyle(fontSize: 19, fontWeight: FontWeight.w600)),
      const SizedBox(height: 4),
      if (d != null)
        Text('${d.toolpaths.where((t) => t.enabled).length} toolpath${d.toolpaths.where((t) => t.enabled).length == 1 ? '' : 's'}',
            style: const TextStyle(color: C.label, fontSize: 13)),
      const SizedBox(height: 14),
      Container(
        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
        decoration: BoxDecoration(color: C.base, borderRadius: BorderRadius.circular(6)),
        child: Text(c == null ? 'To: —' : 'To: ${c.name}${machine == null ? '' : ' · ${machine.name}'}',
            style: const TextStyle(fontSize: 14)),
      ),
      if (warn != null) Padding(padding: const EdgeInsets.only(top: 10), child: Text(warn, style: const TextStyle(color: C.amber, fontSize: 13))),
      const Spacer(),
      if (_busy)
        const Padding(
          padding: EdgeInsets.only(bottom: 10),
          child: Text('Sending… (phobiCCCP may ask about unsaved work first)', style: TextStyle(color: C.label)),
        ),
      SizedBox(
        height: 64,
        child: Material(
          color: canStart ? C.amber : C.amber.withAlpha(90),
          borderRadius: BorderRadius.circular(8),
          child: InkWell(
            borderRadius: BorderRadius.circular(8),
            onTap: canStart ? () => _send(true) : null,
            child: const Center(
              child: Row(mainAxisSize: MainAxisSize.min, children: [
                Icon(Icons.play_arrow_rounded, color: C.base, size: 30),
                SizedBox(width: 6),
                Text('Start', style: TextStyle(fontSize: 18, fontWeight: FontWeight.w600, color: C.base)),
              ]),
            ),
          ),
        ),
      ),
      const SizedBox(height: 10),
      PushBtn('Send without starting',
          height: 52, fontSize: 15, onTap: d != null && c != null && !_busy ? () => _send(false) : null),
    ]);
  }
}
