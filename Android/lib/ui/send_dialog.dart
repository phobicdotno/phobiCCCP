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
    showDialog<String>(context: context, builder: (_) => SendDialog(app, d));

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
  String? _error;

  AppState get app => widget.app;

  @override
  void initState() {
    super.initState();
    final last = app.lastTarget.split('/');
    final reach = _reachable();
    final c = reach.where((c) => c.desktop.id == last.first).firstOrNull ?? reach.firstOrNull;
    _computer = c?.desktop.id;
    final machines = c?.info?.machines ?? const [];
    _machine = machines.any((m) => last.length > 1 && m.id == last[1]) ? last[1] : c?.info?.activeMachine;
  }

  List<Computer> _reachable() => app.computers.where((c) => c.reachable(app.now)).toList();

  Future<void> _go() async {
    final c = _reachable().where((c) => c.desktop.id == _computer).firstOrNull;
    if (c == null) return;
    if (_start) {
      final machine = c.info?.machines.where((m) => m.id == _machine).firstOrNull?.name ?? 'the machine';
      if (!await confirmStart(context, widget.design.name, machine, c.name)) return;
    }
    setState(() {
      _busy = true;
      _error = null;
    });
    app.lastTarget = '${c.desktop.id}/${_machine ?? ''}';
    try {
      final r = await app.send(widget.design, c, _machine ?? '', _start);
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
    String? warn;
    if (_start && info != null) {
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
    return AlertDialog(
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
                      onChanged: (v) => setState(() => _machine = v),
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
        FilledButton(onPressed: c == null || _busy ? null : _go, child: Text(_start ? 'Send and start…' : 'Send')),
      ],
    );
  }
}
