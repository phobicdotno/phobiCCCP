// The desktop's controls, in Flutter: tool buttons, panel heads, cards, the
// ▼ value ▲ steppers of the toolpath form and the compact spin boxes of the
// properties panel (main.cpp, mainwindow.cpp, toolpathform.cpp).

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../model.dart';
import 'icons.dart';
import 'theme.dart';

/// A toolbar button: icon (and a label under it, as the toolpath toolbar has),
/// #3a4150 while checked, #333945 under the finger.
class ToolBtn extends StatelessWidget {
  final Widget icon;
  final String? label;
  final String tip;
  final bool checked, enabled;
  final VoidCallback? onTap;
  const ToolBtn({super.key, required this.icon, required this.tip, this.label, this.checked = false, this.enabled = true, this.onTap});

  @override
  Widget build(BuildContext context) {
    final child = label == null
        ? Padding(padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 9), child: icon)
        : Padding(
            padding: const EdgeInsets.fromLTRB(9, 4, 9, 3),
            child: Column(mainAxisSize: MainAxisSize.min, children: [
              icon,
              const SizedBox(height: 2),
              Text(label!, style: TextStyle(fontSize: 12.5, color: enabled ? C.text : C.disabled)),
            ]),
          );
    return Tooltip(
      message: tip,
      waitDuration: const Duration(milliseconds: 600),
      child: Material(
        color: checked ? C.checked : Colors.transparent,
        borderRadius: BorderRadius.circular(4),
        child: InkWell(
          borderRadius: BorderRadius.circular(4),
          onTap: enabled ? onTap : null,
          child: Opacity(opacity: enabled ? 1 : 0.4, child: child),
        ),
      ),
    );
  }
}

/// A text-only toolbar action with a small icon in front, as Delete / Snap / Fit.
class TextAction extends StatelessWidget {
  final String text;
  final String? icon;
  final bool checked, enabled;
  final VoidCallback? onTap;
  const TextAction(this.text, {super.key, this.icon, this.checked = false, this.enabled = true, this.onTap});
  @override
  Widget build(BuildContext context) => Material(
        color: checked ? C.checked : Colors.transparent,
        borderRadius: BorderRadius.circular(4),
        child: InkWell(
          borderRadius: BorderRadius.circular(4),
          onTap: enabled ? onTap : null,
          child: Padding(
            padding: const EdgeInsets.symmetric(horizontal: 9, vertical: 11),
            child: Row(mainAxisSize: MainAxisSize.min, children: [
              if (icon != null) ...[PIcon(icon!, size: 16, enabled: enabled), const SizedBox(width: 5)],
              Text(text,
                  style: TextStyle(fontSize: 12.5, fontWeight: FontWeight.w600, color: enabled ? C.text : C.disabled.withAlpha(170))),
            ]),
          ),
        ),
      );
}

/// The 34 px title strip over a side panel: icon and bold title.
class PanelHead extends StatelessWidget {
  final String title, icon;
  final List<Widget> trailing;
  const PanelHead(this.title, this.icon, {super.key, this.trailing = const []});
  @override
  Widget build(BuildContext context) => Container(
        height: 34,
        padding: const EdgeInsets.symmetric(horizontal: 10),
        decoration: const BoxDecoration(color: C.panelHead, border: Border(bottom: BorderSide(color: C.border))),
        child: Row(children: [
          PIcon(icon, size: 18),
          const SizedBox(width: 8),
          Text(title, style: const TextStyle(fontWeight: FontWeight.bold, fontSize: 13)),
          const Spacer(),
          ...trailing,
        ]),
      );
}

/// A rounded card (#tpCard), optionally with a collapsible "▾ TITLE" head.
class PCard extends StatelessWidget {
  final String? title;
  final Widget child;
  final EdgeInsets padding;
  const PCard({super.key, this.title, required this.child, this.padding = const EdgeInsets.fromLTRB(10, 6, 10, 10)});
  @override
  Widget build(BuildContext context) => Container(
        margin: const EdgeInsets.symmetric(vertical: 4),
        decoration: BoxDecoration(color: C.card, border: Border.all(color: C.cardBorder), borderRadius: BorderRadius.circular(8)),
        padding: padding,
        child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, mainAxisSize: MainAxisSize.min, children: [
          if (title != null)
            Padding(
              padding: const EdgeInsets.fromLTRB(4, 4, 0, 6),
              child: Row(children: [
                const Caret(size: 7),
                const SizedBox(width: 8),
                Text(title!.toUpperCase(), style: const TextStyle(fontWeight: FontWeight.bold, fontSize: 13.5, letterSpacing: 0.3)),
              ]),
            ),
          child,
        ]),
      );
}

/// The small ▾ of a menu button or a collapsible card head (Inter has no glyph for it).
class Caret extends StatelessWidget {
  final double size;
  final Color color;
  const Caret({super.key, this.size = 7, this.color = C.label});
  @override
  Widget build(BuildContext context) => CustomPaint(size: Size(size, size * 0.6), painter: _CaretPainter(color));
}

class _CaretPainter extends CustomPainter {
  final Color color;
  _CaretPainter(this.color);
  @override
  void paint(Canvas c, Size s) => c.drawPath(
      Path()
        ..moveTo(0, 0)
        ..lineTo(s.width, 0)
        ..lineTo(s.width / 2, s.height)
        ..close(),
      Paint()..color = color);
  @override
  bool shouldRepaint(_CaretPainter old) => old.color != color;
}

/// A QPushButton: flat #2a2e36, rounded, light text.
class PushBtn extends StatelessWidget {
  final String text;
  final VoidCallback? onTap;
  final double height, fontSize;
  final Color color, textColor;
  final Widget? leading;
  const PushBtn(this.text,
      {super.key,
      this.onTap,
      this.height = 36,
      this.fontSize = 14,
      this.color = C.button,
      this.textColor = C.text,
      this.leading});
  @override
  Widget build(BuildContext context) => SizedBox(
        height: height,
        child: Material(
          color: onTap == null ? color.withAlpha(110) : color,
          borderRadius: BorderRadius.circular(6),
          child: InkWell(
            borderRadius: BorderRadius.circular(6),
            onTap: onTap,
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 14),
              child: Row(mainAxisSize: MainAxisSize.min, mainAxisAlignment: MainAxisAlignment.center, children: [
                if (leading != null) ...[leading!, const SizedBox(width: 8)],
                Flexible(
                  child: Text(text,
                      overflow: TextOverflow.ellipsis,
                      style: TextStyle(fontSize: fontSize, color: onTap == null ? textColor.withAlpha(120) : textColor, fontWeight: FontWeight.w500)),
                ),
              ]),
            ),
          ),
        ),
      );
}

/// A number box that reports only numbers it can read: the compact spin box
/// of the properties panel ("0.000 mm"), or the big centred one between a
/// stepper's ▼ and ▲.
class NumBox extends StatefulWidget {
  final double value;
  final ValueChanged<double> onChanged;
  final double min, max;
  final int decimals;
  final String suffix;
  final bool big;
  const NumBox(this.value, this.onChanged,
      {super.key, this.min = -10000, this.max = 10000, this.decimals = 3, this.suffix = ' mm', this.big = false});
  @override
  State<NumBox> createState() => _NumBoxState();
}

class _NumBoxState extends State<NumBox> {
  late final TextEditingController _c = TextEditingController(text: _show(widget.value));
  final _focus = FocusNode();

  String _show(double v) => '${v.toStringAsFixed(widget.decimals)}${widget.suffix}';

  @override
  void initState() {
    super.initState();
    _focus.addListener(() {
      if (_focus.hasFocus) {
        _c.text = widget.value.toStringAsFixed(widget.decimals);
        _c.selection = TextSelection(baseOffset: 0, extentOffset: _c.text.length);
      } else {
        _commit();
      }
    });
  }

  @override
  void didUpdateWidget(NumBox old) {
    super.didUpdateWidget(old);
    if (!_focus.hasFocus) _c.text = _show(widget.value);
  }

  @override
  void dispose() {
    _c.dispose();
    _focus.dispose();
    super.dispose();
  }

  void _commit() {
    // A plain number, maybe with its unit; anything else ("1/4 in", "2in")
    // is left alone rather than read as some other number.
    final t = _c.text.trim().toLowerCase().replaceAll(',', '.').replaceFirst(RegExp(r'\s*(mm|°)$'), '');
    final v = double.tryParse(t);
    if (v != null && v.isFinite) {
      final k = v.clamp(widget.min, widget.max).toDouble();
      if (k != widget.value) widget.onChanged(k);
    }
    _c.text = _show(widget.value);
  }

  @override
  Widget build(BuildContext context) => SizedBox(
        height: widget.big ? 40 : 30,
        child: TextField(
          controller: _c,
          focusNode: _focus,
          textAlign: widget.big ? TextAlign.center : TextAlign.left,
          style: TextStyle(fontSize: widget.big ? 15 : 13, color: C.text),
          keyboardType: const TextInputType.numberWithOptions(decimal: true, signed: true),
          inputFormatters: [FilteringTextInputFormatter.allow(RegExp(r'[-0-9.,a-z °]'))],
          decoration: InputDecoration(
            contentPadding: EdgeInsets.symmetric(horizontal: 6, vertical: widget.big ? 10 : 7),
            fillColor: widget.big ? C.base : C.base,
          ),
          onSubmitted: (_) => _commit(),
          // A finger tap elsewhere does not take the focus on a pad: commit
          // here, before the tap selects another shape or presses OK.
          onTapOutside: (_) => _focus.unfocus(),
        ),
      );
}

/// ▼ value ▲, the finger-sized stepper of the toolpath form.
class StepperField extends StatelessWidget {
  final double value, step, min, max;
  final ValueChanged<double> onChanged;
  final String suffix;
  final int decimals;
  const StepperField(this.value, this.onChanged,
      {super.key, this.step = 0.1, this.min = 0, this.max = 1000, this.suffix = ' mm', this.decimals = 2});

  Widget _btn(String t, double d) => SizedBox(
        width: 46,
        height: 40,
        child: Material(
          color: C.modeOn,
          borderRadius: BorderRadius.circular(6),
          child: InkWell(
            borderRadius: BorderRadius.circular(6),
            onTap: () => onChanged((((value + d) / step).roundToDouble() * step).clamp(min, max).toDouble()),
            child: Center(child: Text(t, style: const TextStyle(fontSize: 18, color: C.text))),
          ),
        ),
      );

  @override
  Widget build(BuildContext context) => Row(mainAxisSize: MainAxisSize.min, children: [
        _btn('▼', -step),
        const SizedBox(width: 4),
        SizedBox(width: 118, child: NumBox(value, onChanged, min: min, max: max, decimals: decimals, suffix: suffix, big: true)),
        const SizedBox(width: 4),
        _btn('▲', step),
      ]);
}

/// A label on the left, the editor on the right, as a QFormLayout row.
class FormRow extends StatelessWidget {
  final String label;
  final Widget child;
  final double labelWidth;
  const FormRow(this.label, this.child, {super.key, this.labelWidth = 72});
  @override
  Widget build(BuildContext context) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 3),
        child: Row(children: [
          SizedBox(width: labelWidth, child: Text(label, style: const TextStyle(color: C.label, fontSize: 13))),
          Expanded(child: child),
        ]),
      );
}

/// A one-line text box for a dialog, owning its controller: the controller is
/// disposed with the box, once the dialog's closing animation is over, not
/// while the box is still on screen.
class LineBox extends StatefulWidget {
  final String initial;
  final ValueChanged<String> onChanged;
  final bool autofocus;
  final int? maxLength;
  const LineBox({super.key, this.initial = '', required this.onChanged, this.autofocus = false, this.maxLength});
  @override
  State<LineBox> createState() => _LineBoxState();
}

class _LineBoxState extends State<LineBox> {
  late final _c = TextEditingController(text: widget.initial);

  @override
  void dispose() {
    _c.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => SizedBox(
        height: 34,
        child: TextField(
          controller: _c,
          autofocus: widget.autofocus,
          inputFormatters: [if (widget.maxLength != null) LengthLimitingTextInputFormatter(widget.maxLength)],
          onChanged: widget.onChanged,
        ),
      );
}

/// Stock size and material, for File › New and Document setup.
class StockFields extends StatelessWidget {
  final Stock stock;
  final String material;
  final ToolLibrary tools;
  final ValueChanged<Stock> onStock;
  final ValueChanged<String> onMaterial;
  const StockFields(
      {super.key, required this.stock, required this.material, required this.tools, required this.onStock, required this.onMaterial});

  @override
  Widget build(BuildContext context) {
    final mats = tools.materials.isEmpty ? const [MaterialInfo('softwood', 'Soft wood')] : tools.materials;
    return Column(crossAxisAlignment: CrossAxisAlignment.stretch, mainAxisSize: MainAxisSize.min, children: [
      FormRow('Width X', NumBox(stock.width, (v) => onStock(stock.copyWith(width: v)), min: 1), labelWidth: 96),
      FormRow('Height Y', NumBox(stock.height, (v) => onStock(stock.copyWith(height: v)), min: 1), labelWidth: 96),
      FormRow('Thickness Z', NumBox(stock.thickness, (v) => onStock(stock.copyWith(thickness: v)), min: 0.1, max: 1000),
          labelWidth: 96),
      FormRow(
          'Material',
          Combo<String>(
            value: mats.any((m) => m.id == material) ? material : mats.first.id,
            items: [for (final m in mats) (m.id, m.name)],
            onChanged: onMaterial,
          ),
          labelWidth: 96),
      if (tools.materials.isEmpty)
        const Padding(
          padding: EdgeInsets.only(top: 6),
          child: Text('The materials come from phobiCCCP\'s tool library once the pad has found it.',
              style: TextStyle(color: C.sub, fontSize: 12)),
        ),
    ]);
  }
}

/// A QComboBox: the value in a box with a small ▾ at the right.
class Combo<T> extends StatelessWidget {
  final T value;
  final List<(T, String)> items;
  final ValueChanged<T> onChanged;
  const Combo({super.key, required this.value, required this.items, required this.onChanged});
  @override
  Widget build(BuildContext context) {
    final label = items.where((i) => i.$1 == value).map((i) => i.$2).firstOrNull ?? '';
    return PopupMenuButton<T>(
      initialValue: value,
      onSelected: onChanged,
      tooltip: '',
      position: PopupMenuPosition.under,
      itemBuilder: (_) => [for (final i in items) PopupMenuItem(value: i.$1, height: 36, child: Text(i.$2))],
      child: Container(
        height: 34,
        padding: const EdgeInsets.symmetric(horizontal: 6),
        decoration: BoxDecoration(color: C.button, border: Border.all(color: C.cardBorder), borderRadius: BorderRadius.circular(2)),
        child: Row(children: [
          Expanded(child: Text(label, overflow: TextOverflow.ellipsis, style: const TextStyle(fontSize: 13.5))),
          const Caret(size: 7),
        ]),
      ),
    );
  }
}

/// A dialog in the desktop's style: title, content, OK/Cancel on the right.
Future<T?> showPDialog<T>(BuildContext context,
    {required String title, required Widget content, List<Widget> Function(BuildContext ctx)? actions, double width = 460}) {
  return showDialog<T>(
    context: context,
    builder: (ctx) => AlertDialog(
      title: Text(title),
      titlePadding: const EdgeInsets.fromLTRB(18, 16, 18, 8),
      contentPadding: const EdgeInsets.fromLTRB(18, 4, 18, 8),
      actionsPadding: const EdgeInsets.fromLTRB(18, 8, 18, 14),
      content: SizedBox(width: width, child: content),
      actions: actions?.call(ctx) ?? [TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('OK'))],
    ),
  );
}
