// Send a design to any PhobiCCCP that is open on the network, on any of its
// machines, and optionally start cutting it straight away. Starting is asked
// for on every send (off by default) and confirmed once more before it goes.

import 'package:flutter/material.dart';

import '../app_state.dart';
import '../model.dart';
import 'theme.dart';

/// Asked once more before a send that starts the machine.
Future<bool> confirmStart(BuildContext context, String design, String machine, String computer) async {
  final sure = await showDialog<bool>(
    context: context,
    builder: (ctx) => AlertDialog(
      title: const Text('Start cutting now?'),
      content: SizedBox(
        width: 460,
        child: Text('“$design” will start on $machine at $computer as soon as it arrives, and the spindle '
            'will spin up. Make sure the stock is clamped, the right bit is in, the zero is set and nobody is near the machine.'),
      ),
      actions: [
        TextButton(onPressed: () => Navigator.pop(ctx, false), child: const Text('Cancel')),
        FilledButton(onPressed: () => Navigator.pop(ctx, true), child: const Text('Send and start')),
      ],
    ),
  );
  return sure == true;
}

/// What to tell the user after a send.
String sendWords(SendResult r, bool start, String computer) =>
    (start && r.started ? 'Cutting started on $computer.' : (r.message.isEmpty ? 'Sent to $computer.' : r.message)) +
    (r.notes.isEmpty ? '' : ' ${r.notes.join('; ')}');

Future<String?> showSendDialog(BuildContext context, AppState app, Design d) =>
    // Not closable by tapping outside or Back while a send is on its way: the
    // answer (started, or why not) must reach the user, or they send twice.
    showDialog<String>(context: context, barrierDismissible: false, builder: (_) => SendDialog(app, d));

class SendDialog extends StatefulWidget {
  final AppState app;
  final Design design;
  const SendDialog(this.app, this.design, {super.key});
  @override
  State<SendDialog> createState() => _SendDialogState();
}

class _SendDialogState extends State<SendDialog> {
  String? _computer, _machine;
  bool _start = false, _busy = false;
  // Picked from the list, not just the machine the computer had in use.
  bool _machineByHand = false;
  String? _error;

  AppState get app => widget.app;

  @override
  void initState() {
    super.initState();
    // Computers come and go while the dialog is open.
    app.addListener(_changed);
    final last = app.lastTarget.split('/');
    final reach = _reachable();
    final c = reach.where((c) => c.desktop.id == last.first).firstOrNull ?? reach.firstOrNull;
    _computer = c?.desktop.id;
    // The machine the computer uses now: the last send's machine may have
    // been switched away from since, and sending would switch it back.
    _machine = c?.info?.activeMachine;
  }

  @override
  void dispose() {
    app.removeListener(_changed);
    super.dispose();
  }

  void _changed() {
    if (!mounted) return;
    setState(() {
      // Follow the computer's machine in use until one is picked by hand:
      // sending to the one in use when the dialog opened switched the
      // computer back to it, reconnecting and losing its homing.
      if (!_machineByHand) {
        final c = _reachable().where((c) => c.desktop.id == _computer).firstOrNull;
        final now = c?.info?.activeMachine;
        if (now != null) _machine = now;
      }
    });
  }

  List<Computer> _reachable() => app.computers.where((c) => c.reachable(app.now)).toList();

  /// Toolpaths that computer cannot make: their tool is not in its own
  /// library (the pad picks tools from whichever computer synced last), or
  /// has no feeds there for the material. Empty while its library is not known.
  List<String> _missingTools(Computer c) {
    final lib = app.toolsOf(c.desktop.id);
    if (lib.isEmpty) return const [];
    final d = widget.design;
    // As phobiCCCP builds it (src/padlink.cpp): an unknown material as soft wood.
    final material = lib.materials.any((m) => m.id == d.material) ? d.material : 'softwood';
    final out = <String>[];
    for (final t in d.toolpaths.where((t) => t.shapes.isNotEmpty)) {
      final tool = lib.tool(t.tool);
      if (tool == null) {
        out.add('${t.name}: T${t.tool} is not in its tool library');
      } else if (!tool.materials.contains(material)) {
        out.add('${t.name}: T${t.tool} has no feeds for ${lib.materialName(material)} there');
      } else if (veeLeftOut(t, tool)) {
        out.add('${t.name}: T${t.tool} is a V-bit; ${t.type == 'pocket' ? 'a pocket' : 'an inside or outside profile'} needs an end mill');
      }
    }
    return out;
  }

  Future<void> _go() async {
    final c = _reachable().where((c) => c.desktop.id == _computer).firstOrNull;
    if (c == null) return;
    // The machine named in the question is the one sent to: the dialog
    // follows the computer's machine in use, which can change while the
    // question is open, and must not start a machine nobody confirmed.
    final machineId = _machine;
    if (_start) {
      final machine = c.info?.machines.where((m) => m.id == machineId).firstOrNull?.name ?? 'the machine';
      if (!await confirmStart(context, widget.design.name, machine, c.name)) return;
    }
    setState(() {
      _busy = true;
      _error = null;
    });
    app.lastTarget = '${c.desktop.id}/${machineId ?? ''}';
    try {
      final r = await app.send(widget.design, c, machineId ?? '', _start);
      if (!mounted) return;
      Navigator.pop(context, sendWords(r, _start, c.name));
    } catch (e) {
      if (mounted) {
        setState(() {
          _busy = false;
          _error = '$e';
        });
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    final reach = _reachable();
    final c = reach.where((c) => c.desktop.id == _computer).firstOrNull;
    final info = c?.info;
    final missing = c == null ? const <String>[] : _missingTools(c);
    String? warn;
    if (info != null && _machine != null && _machine != info.activeMachine && info.status.connected && !info.allowStart) {
      warn = 'That machine is not the one in use, and this computer does not let a pad switch machines while one is '
          'connected (Machine › Pads may start cutting). Disconnect it there, or send to the machine in use.';
    } else if (_start && info != null) {
      if (!info.allowStart) {
        warn = 'This computer does not allow starting from a pad (Machine › Pads may start cutting). '
            'The design is opened there, not started.';
      } else if (_machine != null && _machine != info.activeMachine) {
        warn = 'That machine is not the one in use; phobiCCCP switches to it and reconnects, '
            'and it will most likely need Initialize before it can cut.';
      } else if (!info.status.readyToCut) {
        warn = 'The machine is not ready (${info.status.words()}); the design will be opened, not started.';
      } else if (widget.design.toolpaths.isEmpty) {
        warn = 'This design has no toolpaths, so there is nothing to cut.';
      }
    }
    return PopScope(
      canPop: !_busy,
      child: AlertDialog(
      title: Text('Send “${widget.design.name}”'),
      content: SizedBox(
        width: 480,
        child: reach.isEmpty
            ? const Text('No PhobiCCCP is answering right now. Open it on a computer on this network '
                '(or add it under Machines) and try again.', style: TextStyle(color: C.sub))
            : SingleChildScrollView(
                child: Column(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.start, children: [
                  const Text('To', style: TextStyle(color: C.sub, fontSize: 12)),
                  RadioGroup<String>(
                    groupValue: _computer,
                    onChanged: (v) => setState(() {
                      _computer = v;
                      _machineByHand = false;
                      _machine = reach.where((x) => x.desktop.id == v).firstOrNull?.info?.activeMachine;
                    }),
                    child: Column(children: [
                      for (final x in reach)
                        RadioListTile<String>(
                            value: x.desktop.id,
                            contentPadding: EdgeInsets.zero,
                            title: Text(x.name),
                            subtitle: Text(x.info?.status.words() ?? '')),
                    ]),
                  ),
                  if ((info?.machines.length ?? 0) > 1) ...[
                    const Text('Machine', style: TextStyle(color: C.sub, fontSize: 12)),
                    RadioGroup<String>(
                      groupValue: _machine,
                      onChanged: (v) => setState(() {
                        _machine = v;
                        _machineByHand = true;
                      }),
                      child: Column(children: [
                        for (final m in info!.machines)
                          RadioListTile<String>(
                              value: m.id, contentPadding: EdgeInsets.zero, title: Text(m.active ? '${m.name}  (in use)' : m.name)),
                      ]),
                    ),
                  ],
                  CheckboxListTile(
                    value: _start,
                    onChanged: (v) => setState(() => _start = v ?? false),
                    contentPadding: EdgeInsets.zero,
                    controlAffinity: ListTileControlAffinity.leading,
                    title: const Text('Start cutting right away'),
                  ),
                  if (missing.isNotEmpty)
                    Text('${c!.name} cannot make this as it is: ${missing.join('; ')}. Pick a tool it has, or sync with it first.',
                        style: const TextStyle(color: C.amber, fontSize: 13)),
                  if (warn != null) Text(warn, style: const TextStyle(color: C.amber, fontSize: 13)),
                  if (_busy)
                    const Padding(
                      padding: EdgeInsets.only(top: 12),
                      child: Row(children: [
                        SizedBox(width: 18, height: 18, child: CircularProgressIndicator(strokeWidth: 2)),
                        SizedBox(width: 10),
                        Expanded(child: Text('Sending… (phobiCCCP may ask about unsaved work first)', style: TextStyle(color: C.sub))),
                      ]),
                    ),
                  if (_error != null) Text(_error!, style: TextStyle(color: Theme.of(context).colorScheme.error)),
                ]),
              ),
      ),
      actions: [
        TextButton(onPressed: _busy ? null : () => Navigator.pop(context), child: const Text('Cancel')),
        FilledButton(onPressed: c == null || _busy ? null : _go, child: Text(missing.isNotEmpty ? 'Send anyway' : (_start ? 'Send and start…' : 'Send'))),
      ],
    ),
    );
  }
}

/// A toolpath phobiCCCP leaves out: a pocket or an inside/outside profile
/// with a V-bit (an offset is a tool radius, far wider than a V-bit cuts;
/// src/gcodeexport.cpp). One turned off is not made, so not left out either.
bool veeLeftOut(ToolpathSpec t, Tool tool) =>
    t.enabled && tool.isVee && (t.type == 'pocket' || (t.type == 'profile' && t.side != 'on'));
