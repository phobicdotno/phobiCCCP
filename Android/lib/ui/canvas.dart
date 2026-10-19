// The design drawn on its stock, sized for a pad: tap to select, drag the
// selected shape to move it, pinch to zoom, drag elsewhere to pan.

import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../model.dart';
import 'theme.dart';

/// Millimetres on the stock (Y up) to pixels on the view (Y down): fitted with
/// a margin, then zoomed by [zoom] about the view's centre and moved by [pan].
class StockView {
  final Design design;
  late final double scale, _ox, _oy;

  StockView(this.design, Size size, {double zoom = 1, Offset pan = Offset.zero, double margin = 32}) {
    final fit = math.min((size.width - 2 * margin) / design.stock.width, (size.height - 2 * margin) / design.stock.height);
    scale = math.max(fit, 0.01) * zoom;
    _ox = size.width / 2 - design.stock.width * scale / 2 + pan.dx;
    _oy = size.height / 2 - design.stock.height * scale / 2 + pan.dy;
  }

  Offset toScreen(double x, double y) => Offset(_ox + x * scale, _oy + (design.stock.height - y) * scale);
  (double, double) toMm(Offset p) => ((p.dx - _ox) / scale, design.stock.height - (p.dy - _oy) / scale);
}

/// Zoom and pan of the drawing, kept by the editor so a Fit button can reset it.
class CanvasZoom extends ChangeNotifier {
  double zoom = 1;
  Offset pan = Offset.zero;
  bool get zoomed => zoom != 1 || pan != Offset.zero;
  void fit() {
    zoom = 1;
    pan = Offset.zero;
    notifyListeners();
  }

  void set(double z, Offset p) {
    zoom = z;
    pan = p;
    notifyListeners();
  }
}

/// The shape under (x, y) mm, within [slop] mm; the smallest one wins.
String? hitShape(Design d, double x, double y, double slop) {
  Shape? best;
  double bestSize = double.infinity;
  for (final s in d.shapes) {
    final b = s.bounds();
    if (x < b[0] - slop || x > b[2] + slop || y < b[1] - slop || y > b[3] + slop) continue;
    final size = math.sqrt(math.pow(b[2] - b[0], 2) + math.pow(b[3] - b[1], 2));
    if (size < bestSize) {
      best = s;
      bestSize = size;
    }
  }
  return best?.id;
}

class DesignCanvas extends StatefulWidget {
  final Design design;
  final String? selected;
  final Set<String> highlighted;
  final List<List<double>> pathDraft;
  final CanvasZoom zoom;
  final ValueChanged<String?> onSelect;
  final void Function(String id, double dx, double dy) onMove;
  final VoidCallback onMoveDone;

  /// Set in path mode: every tap adds a point there.
  final void Function(double x, double y)? onAddPoint;

  const DesignCanvas({
    super.key,
    required this.design,
    required this.selected,
    required this.zoom,
    required this.onSelect,
    required this.onMove,
    required this.onMoveDone,
    this.highlighted = const {},
    this.pathDraft = const [],
    this.onAddPoint,
  });

  @override
  State<DesignCanvas> createState() => _DesignCanvasState();
}

class _DesignCanvasState extends State<DesignCanvas> {
  Size _size = const Size(1, 1);
  String? _dragging;
  bool _moved = false;
  double _lastScale = 1;

  StockView _view() => StockView(widget.design, _size, zoom: widget.zoom.zoom, pan: widget.zoom.pan);

  void _tap(TapUpDetails t) {
    final v = _view();
    final (x, y) = v.toMm(t.localPosition);
    final add = widget.onAddPoint;
    if (add != null) {
      add(x, y);
    } else {
      widget.onSelect(hitShape(widget.design, x, y, 14 / v.scale));
    }
  }

  void _scaleStart(ScaleStartDetails s) {
    _lastScale = 1;
    _moved = false;
    _dragging = null;
    if (s.pointerCount == 1 && widget.onAddPoint == null && widget.selected != null) {
      final v = _view();
      final (x, y) = v.toMm(s.localFocalPoint);
      if (hitShape(widget.design, x, y, 18 / v.scale) == widget.selected) _dragging = widget.selected;
    }
  }

  void _scaleUpdate(ScaleUpdateDetails s) {
    final id = _dragging;
    if (id != null && s.pointerCount == 1) {
      final k = _view().scale;
      widget.onMove(id, s.focalPointDelta.dx / k, -s.focalPointDelta.dy / k);
      _moved = true;
      return;
    }
    if (id != null) {
      // A second finger came down: stop moving the shape, zoom instead.
      if (_moved) widget.onMoveDone();
      _dragging = null;
    }
    final before = _view();
    final (mx, my) = before.toMm(s.localFocalPoint);
    final z = (widget.zoom.zoom * s.scale / _lastScale).clamp(0.5, 40.0);
    _lastScale = s.scale;
    final after = StockView(widget.design, _size, zoom: z, pan: widget.zoom.pan);
    widget.zoom.set(z, widget.zoom.pan + (s.localFocalPoint - after.toScreen(mx, my)) + s.focalPointDelta);
  }

  void _scaleEnd(ScaleEndDetails s) {
    if (_dragging != null && _moved) widget.onMoveDone();
    _dragging = null;
  }

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(builder: (context, box) {
      _size = Size(box.maxWidth, box.maxHeight);
      return GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTapUp: _tap,
        onScaleStart: _scaleStart,
        onScaleUpdate: _scaleUpdate,
        onScaleEnd: _scaleEnd,
        child: ListenableBuilder(
          listenable: widget.zoom,
          builder: (context, _) => CustomPaint(
            size: _size,
            painter: DesignPainter(widget.design,
                zoom: widget.zoom.zoom,
                pan: widget.zoom.pan,
                selected: widget.selected,
                highlighted: widget.highlighted,
                pathDraft: widget.pathDraft),
          ),
        ),
      );
    });
  }
}

/// A still picture of a design, for the design list.
class DesignThumbnail extends StatelessWidget {
  final Design design;
  const DesignThumbnail(this.design, {super.key});
  @override
  Widget build(BuildContext context) => CustomPaint(painter: DesignPainter(design, margin: 10, thin: true), size: Size.infinite);
}

class DesignPainter extends CustomPainter {
  final Design design;
  final double zoom, margin;
  final Offset pan;
  final String? selected;
  final Set<String> highlighted;
  final List<List<double>> pathDraft;
  final bool thin;

  DesignPainter(this.design,
      {this.zoom = 1,
      this.pan = Offset.zero,
      this.selected,
      this.highlighted = const {},
      this.pathDraft = const [],
      this.margin = 32,
      this.thin = false});

  @override
  void paint(Canvas canvas, Size size) {
    final v = StockView(design, size, zoom: zoom, pan: pan, margin: margin);
    final a = v.toScreen(0, design.stock.height), b = v.toScreen(design.stock.width, 0);
    canvas.drawRect(Rect.fromPoints(a, b), Paint()..color = Brand.stock);
    canvas.drawRect(Rect.fromPoints(a, b), Paint()
      ..color = Brand.stockEdge
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.5);
    if (!thin) {
      // X0 Y0: the corner phobiCCCP zeroes on.
      final o = v.toScreen(0, 0);
      canvas.drawLine(o, o + const Offset(30, 0), Paint()..color = Brand.red..strokeWidth = 3);
      canvas.drawLine(o, o + const Offset(0, -30), Paint()..color = Brand.good..strokeWidth = 3);
    }
    for (final s in design.shapes) {
      final color = s.id == selected
          ? Brand.gold
          : highlighted.contains(s.id)
              ? Brand.amber
              : Brand.text;
      _shape(canvas, v, s, color, s.id == selected ? 3 : (thin ? 1.2 : 1.8));
    }
    if (pathDraft.isNotEmpty) {
      final p = Paint()
        ..color = Brand.blue
        ..strokeWidth = 2.5;
      final pts = pathDraft.map((q) => v.toScreen(q[0], q[1])).toList();
      for (var i = 1; i < pts.length; i++) {
        canvas.drawLine(pts[i - 1], pts[i], p);
      }
      for (final q in pts) {
        canvas.drawCircle(q, 6, p);
      }
    }
  }

  void _shape(Canvas canvas, StockView v, Shape s, Color color, double width) {
    final paint = Paint()
      ..color = color
      ..style = PaintingStyle.stroke
      ..strokeWidth = width
      ..isAntiAlias = true;
    switch (s.type) {
      case Shape.rect:
        canvas.drawRect(Rect.fromPoints(v.toScreen(s.x, s.y + (s.h ?? 0)), v.toScreen(s.x + (s.w ?? 0), s.y)), paint);
      case Shape.circle:
        canvas.drawCircle(v.toScreen(s.x, s.y), (s.r ?? 0) * v.scale, paint);
      case Shape.polygon:
        final n = (s.sides ?? 6).clamp(3, 100);
        final r = s.r ?? 0;
        final path = Path();
        for (var i = 0; i <= n; i++) {
          // As phobiCCCP makes it: vertex 0 on +X, counter-clockwise.
          final a = 2 * math.pi * i / n;
          final p = v.toScreen(s.x + r * math.cos(a), s.y + r * math.sin(a));
          i == 0 ? path.moveTo(p.dx, p.dy) : path.lineTo(p.dx, p.dy);
        }
        canvas.drawPath(path, paint);
      case Shape.text:
        // size is the letters' ascent, as on the desktop.
        final tp = TextPainter(
          text: TextSpan(
              text: s.label ?? '',
              style: TextStyle(
                  fontSize: (s.size ?? 10) * 1.07 * v.scale,
                  height: 1,
                  foreground: Paint()
                    ..color = color
                    ..style = PaintingStyle.stroke
                    ..strokeWidth = 1.2)),
          textDirection: TextDirection.ltr,
        )..layout();
        final base = tp.computeDistanceToActualBaseline(TextBaseline.alphabetic);
        tp.paint(canvas, v.toScreen(s.x, s.y) - Offset(0, base));
      case Shape.path:
        final pts = s.points ?? const [];
        if (pts.length < 2) return;
        final path = Path();
        for (var i = 0; i < pts.length; i++) {
          final p = v.toScreen(pts[i][0], pts[i][1]);
          i == 0 ? path.moveTo(p.dx, p.dy) : path.lineTo(p.dx, p.dy);
        }
        if (s.closed == true) path.close();
        canvas.drawPath(path, paint);
      case Shape.point:
        final p = v.toScreen(s.x, s.y);
        canvas.drawLine(p - const Offset(8, 0), p + const Offset(8, 0), paint);
        canvas.drawLine(p - const Offset(0, 8), p + const Offset(0, 8), paint);
    }
    if (s.id == selected) {
      final b = s.bounds();
      final r = Rect.fromPoints(v.toScreen(b[0], b[3]), v.toScreen(b[2], b[1])).inflate(6);
      final dash = Paint()
        ..color = color.withValues(alpha: 0.5)
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1;
      canvas.drawRect(r, dash);
    }
  }

  @override
  bool shouldRepaint(DesignPainter old) =>
      old.design != design ||
      old.zoom != zoom ||
      old.pan != pan ||
      old.selected != selected ||
      old.highlighted != highlighted ||
      old.pathDraft != pathDraft;
}
