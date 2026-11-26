// "All settings" under the toolpath form: every setting the computer's form
// has for this toolpath (toolpath_fields.dart), grouped in its cards, folded
// until opened.

import 'package:flutter/material.dart';

import '../model.dart';
import '../toolpath_fields.dart';
import 'theme.dart';
import 'widgets.dart';

class ToolpathSettings extends StatefulWidget {
  final ToolpathSpec toolpath;
  final ValueChanged<ToolpathSpec> onChanged;

  /// Open at first (tests; the panel starts it folded, as the desktop's Advanced).
  final bool initiallyOpen;
  const ToolpathSettings(this.toolpath, this.onChanged, {super.key, this.initiallyOpen = false});

  @override
  State<ToolpathSettings> createState() => _ToolpathSettingsState();
}

class _ToolpathSettingsState extends State<ToolpathSettings> {
  late bool _open = widget.initiallyOpen;

  Widget _editor(ToolpathSpec t, TpField f) {
    final v = tpSetting(t, f.key);
    void put(Object nv) => widget.onChanged(tpWithSetting(t, f, nv));
    switch (f.kind) {
      case TpFieldKind.depth:
      case TpFieldKind.number:
      case TpFieldKind.integer:
        return StepperField(
          v is num ? (f.kind == TpFieldKind.depth ? v.toDouble().abs() : v.toDouble()) : 0,
          (d) => put(f.kind == TpFieldKind.integer ? d.round() : d),
          step: f.kind == TpFieldKind.integer ? 1 : f.step,
          min: f.min,
          max: f.max,
          decimals: f.decimals,
          suffix: f.unit.isEmpty ? '' : ' ${f.unit}',
        );
      case TpFieldKind.toggle:
        return Switch(value: v == true, onChanged: (b) => put(b));
      case TpFieldKind.invToggle:
        return Switch(value: v != true, onChanged: (b) => put(!b));
      case TpFieldKind.choice:
        return SizedBox(
          width: 218,
          child: Combo<Object>(
            value: f.choices.firstWhere((c) => c.$1 == v, orElse: () => (v ?? '', '$v')).$1,
            items: f.choices,
            onChanged: put,
          ),
        );
    }
  }

  Widget _row(ToolpathSpec t, TpField f) {
    final on = tpFieldEnabled(t, f);
    Widget label = Text(f.label, style: const TextStyle(color: C.label, fontSize: 14.5));
    label = Tooltip(message: f.tip.isEmpty ? '(${f.key})' : '${f.tip}\n(${f.key})', child: label);
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 3),
      child: Opacity(
        opacity: on ? 1 : 0.4,
        child: IgnorePointer(
          ignoring: !on,
          child: Row(children: [
            Expanded(child: Padding(padding: const EdgeInsets.only(left: 4), child: label)),
            _editor(t, f),
          ]),
        ),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final t = widget.toolpath;
    final fields = tpVisibleFields(t);
    final others = tpOtherKeys(t);
    final children = <Widget>[];
    String? card;
    for (final f in fields) {
      if (f.card != card) {
        card = f.card;
        children.add(Padding(
          padding: const EdgeInsets.fromLTRB(4, 10, 4, 2),
          child: Text((tpCardTitles[card] ?? card).toUpperCase(),
              style: const TextStyle(color: C.sub, fontSize: 12, fontWeight: FontWeight.bold)),
        ));
      }
      children.add(_row(t, f));
    }
    if (others.isNotEmpty) {
      children.add(const Padding(
        padding: EdgeInsets.fromLTRB(4, 10, 4, 2),
        child: Text('SET ON THE COMPUTER', style: TextStyle(color: C.sub, fontSize: 12, fontWeight: FontWeight.bold)),
      ));
      for (final k in others) {
        children.add(Padding(
          padding: const EdgeInsets.symmetric(horizontal: 4, vertical: 2),
          child: Text('$k: ${t.set[k]}', style: const TextStyle(color: C.label, fontSize: 13)),
        ));
      }
    }
    return PCard(
      child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        InkWell(
          onTap: () => setState(() => _open = !_open),
          child: SizedBox(
            height: 40,
            child: Row(children: [
              Icon(_open ? Icons.expand_less_rounded : Icons.expand_more_rounded, color: C.sub),
              const SizedBox(width: 6),
              Text('All settings (${fields.length + others.length})',
                  style: const TextStyle(fontWeight: FontWeight.bold, fontSize: 14)),
            ]),
          ),
        ),
        if (_open) ...children,
      ]),
    );
  }
}
