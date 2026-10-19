import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../model.dart';
import 'theme.dart';

/// A millimetre (or count) field that only reports numbers it can read.
class NumberField extends StatefulWidget {
  final String label;
  final double value;
  final ValueChanged<double> onChanged;
  final double min, max;
  const NumberField(this.label, this.value, this.onChanged, {super.key, this.min = -10000, this.max = 10000});

  @override
  State<NumberField> createState() => _NumberFieldState();
}

class _NumberFieldState extends State<NumberField> {
  late final TextEditingController _c = TextEditingController(text: fmt(widget.value));
  bool _bad = false;

  @override
  void didUpdateWidget(NumberField old) {
    super.didUpdateWidget(old);
    final shown = double.tryParse(_c.text.replaceAll(',', '.'));
    if (shown != widget.value) _c.text = fmt(widget.value);
  }

  @override
  void dispose() {
    _c.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => TextField(
        controller: _c,
        keyboardType: const TextInputType.numberWithOptions(decimal: true, signed: true),
        inputFormatters: [FilteringTextInputFormatter.allow(RegExp(r'[-0-9.,]'))],
        decoration: InputDecoration(labelText: widget.label, errorText: _bad ? '${fmt(widget.min)} to ${fmt(widget.max)}' : null,
            border: const OutlineInputBorder(), isDense: true),
        onChanged: (t) {
          final v = double.tryParse(t.replaceAll(',', '.'));
          final ok = v != null && v >= widget.min && v <= widget.max;
          if (ok != !_bad) setState(() => _bad = !ok);
          if (ok) widget.onChanged(v);
        },
      );
}

/// Stock size and material, for the new-design and design dialogs.
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
    return Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
      Row(children: [
        Expanded(child: NumberField('Width X (mm)', stock.width, (v) => onStock(stock.copyWith(width: v)), min: 1)),
        const SizedBox(width: 12),
        Expanded(child: NumberField('Height Y (mm)', stock.height, (v) => onStock(stock.copyWith(height: v)), min: 1)),
        const SizedBox(width: 12),
        Expanded(child: NumberField('Thickness Z (mm)', stock.thickness, (v) => onStock(stock.copyWith(thickness: v)), min: 0.1, max: 1000)),
      ]),
      const SizedBox(height: 12),
      DropdownButtonFormField<String>(
        initialValue: mats.any((m) => m.id == material) ? material : mats.first.id,
        decoration: const InputDecoration(labelText: 'Material', border: OutlineInputBorder(), isDense: true),
        items: [for (final m in mats) DropdownMenuItem(value: m.id, child: Text(m.name))],
        onChanged: (v) => v == null ? null : onMaterial(v),
      ),
      if (tools.materials.isEmpty)
        const Padding(
          padding: EdgeInsets.only(top: 6),
          child: Text('The materials come from phobiCCCP\'s tool library once the pad has found it.',
              style: TextStyle(color: Brand.textDim, fontSize: 12)),
        ),
    ]);
  }
}

void toast(BuildContext context, String text) =>
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(text), behavior: SnackBarBehavior.floating));
