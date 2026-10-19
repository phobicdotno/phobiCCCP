// The drawing, as phobiCCCP's canvas shows it (canvas.cpp): the board with its
// 5 mm grid, light outlines with a faint fill, amber when selected, the
// selected toolpath's shapes in an amber halo. Touch: the tool draws or
// selects with one finger, two fingers always pan and zoom.

import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../model.dart';
import 'theme.dart';

/// Millimetres on the stock (Y up) to pixels on the view (Y down): fitted with
/// a margin, then zoomed by [zoom] about the view's centre and moved by [pan].
class StockView {
  final Stock stock;
  late final double fit, scale, _ox, _oy;

  StockView(this.stock, Size size, {double zoom = 1, Offset pan = Offset.zero, double margin = 60}) {
    fit = math.max(math.min((size.width - 2 * margin) / stock.width, (size.height - 2 * margin) / stock.height), 0.01);
    scale = fit * zoom;
    _ox = size.width / 2 - stock.width * scale / 2 + pan.dx;
    _oy = size.height / 2 - stock.height * scale / 2 + pan.dy;
  }

  Offset toScreen(double x, double y) => Offset(_ox + x * scale, _oy + (stock.height - y) * scale);
  (double, double) toMm(Offset p) => ((p.dx - _ox) / scale, stock.height - (p.dy - _oy) / scale);
}

/// Zoom and pan of the drawing, shared with the zoom bar and Fit.
class CanvasZoom extends ChangeNotifier {
  double zoom = 1;
  Offset pan = Offset.zero;

  /// Pixels per millimetre at zoom 1, set by the canvas once laid out; the
  /// zoom bar shows zoom × this as a percentage, as the desktop does.
  double fit = 1;
  (double, double)? cursor;

  void reset() {
    zoom = 1;
    pan = Offset.zero;
    notifyListeners();
  }

  void set(double z, Offset p) {
    zoom = z;
    pan = p;
    notifyListeners();
  }

  void at((double, double)? mm) {
    cursor = mm;
    notifyListeners();
  }

  void zoomBy(double k) {
    final z = (zoom * k).clamp(0.2, 60.0).toDouble();
    set(z, pan * (z / zoom));
  }
}

enum DrawTool { select, hand, area, circle, rect, polygon, text, path, point }

/// The shape under (x, y) mm, within [slop] mm; closed shapes from anywhere
/// inside, the smallest one winning.
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

/// What a drag with a drawing tool would make: shown while dragging.
class Draft {
  final DrawTool tool;
  final double x0, y0, x1, y1;
  const Draft(this.tool, this.x0, this.y0, this.x1, this.y1);
}

class DesignCanvas extends StatefulWidget {
  final Design design;
  final DrawTool tool;
  final bool snap;
  final Set<String> selected;
  final Set<String> highlighted;
  final List<List<double>> pathDraft;
  final CanvasZoom zoom;
  final void Function(Set<String> ids) onSelect;
  final void Function(Set<String> ids, double dx, double dy) onMove;
  final VoidCallback onMoveDone;
  final void Function(Draft d) onDraw;
  final void Function(double x, double y) onTapAt;

  const DesignCanvas({
    super.key,
    required this.design,
    required this.tool,
    required this.zoom,
    required this.onSelect,
    required this.onMove,
    required this.onMoveDone,
    required this.onDraw,
    required this.onTapAt,
    this.snap = true,
    this.selected = const {},
    this.highlighted = const {},
    this.pathDraft = const [],
  });

  @override
  State<DesignCanvas> createState() => _DesignCanvasState();
}

class _DesignCanvasState extends State<DesignCanvas> {
  Size _size = const Size(1, 1);
  int _pointers = 0;
  bool _moving = false, _moved = false;
  // What a drag moves: the parent's selection may not have caught up with the tap yet.
  Set<String> _dragIds = const {};
  Offset? _bandStart, _bandEnd;
  (double, double)? _drawStart;
  Draft? _draft;
  double _lastScale = 1;

  StockView _view() => StockView(widget.design.stock, _size, zoom: widget.zoom.zoom, pan: widget.zoom.pan);

  (double, double) _snapped(double x, double y) {
    if (!widget.snap) return (x, y);
    const g = 5.0;
    return ((x / g).roundToDouble() * g, (y / g).roundToDouble() * g);
  }

  void _tap(TapUpDetails t) {
    final v = _view();
    final (x, y) = v.toMm(t.localPosition);
    widget.zoom.at((x, y));
    switch (widget.tool) {
      case DrawTool.select:
        final id = hitShape(widget.design, x, y, 7 / v.scale);
        widget.onSelect(id == null ? {} : {id});
      case DrawTool.area:
        widget.onTapAt(x, y);
      case DrawTool.text:
      case DrawTool.path:
      case DrawTool.point:
        final (sx, sy) = _snapped(x, y);
        widget.onTapAt(sx, sy);
      default:
        break;
    }
  }

  void _start(ScaleStartDetails s) {
    _lastScale = 1;
    _moving = _moved = false;
    _bandStart = _bandEnd = null;
    _drawStart = null;
    _draft = null;
    if (s.pointerCount != 1) return;
    final v = _view();
    final (x, y) = v.toMm(s.localFocalPoint);
    switch (widget.tool) {
      case DrawTool.select:
        final id = hitShape(widget.design, x, y, 9 / v.scale);
        if (id != null) {
          if (widget.selected.contains(id)) {
            _dragIds = widget.selected;
          } else {
            _dragIds = {id};
            widget.onSelect({id});
          }
          _moving = true;
        } else {
          _bandStart = _bandEnd = s.localFocalPoint;
        }
      case DrawTool.circle:
      case DrawTool.rect:
      case DrawTool.polygon:
        _drawStart = _snapped(x, y);
      default:
        break;
    }
  }

  void _update(ScaleUpdateDetails s) {
    final v = _view();
    if (s.pointerCount == 1 && _pointers <= 1) {
      final (x, y) = v.toMm(s.localFocalPoint);
      widget.zoom.at((x, y));
      if (_moving) {
        final ids = _dragIds;
        if (ids.isNotEmpty) widget.onMove(ids, s.focalPointDelta.dx / v.scale, -s.focalPointDelta.dy / v.scale);
        _moved = true;
        return;
      }
      if (_bandStart != null) {
        setState(() => _bandEnd = s.localFocalPoint);
        return;
      }
      final d = _drawStart;
      if (d != null) {
        final (sx, sy) = _snapped(x, y);
        setState(() => _draft = Draft(widget.tool, d.$1, d.$2, sx, sy));
        return;
      }
      if (widget.tool != DrawTool.hand) return;
    } else {
      // Two fingers: stop whatever one finger was doing.
      if (_moving && _moved) widget.onMoveDone();
      _moving = false;
      if (_bandStart != null || _draft != null) {
        setState(() {
          _bandStart = _bandEnd = null;
          _draft = null;
        });
      }
      _drawStart = null;
    }
    final (mx, my) = v.toMm(s.localFocalPoint);
    final z = (widget.zoom.zoom * s.scale / _lastScale).clamp(0.2, 60.0);
    _lastScale = s.scale;
    final after = StockView(widget.design.stock, _size, zoom: z, pan: widget.zoom.pan);
    widget.zoom.set(z, widget.zoom.pan + (s.localFocalPoint - after.toScreen(mx, my)) + s.focalPointDelta);
  }

  void _end(ScaleEndDetails s) {
    if (_moving && _moved) widget.onMoveDone();
    _moving = false;
    final a = _bandStart, b = _bandEnd;
    if (a != null && b != null) {
      final v = _view();
      final (x0, y0) = v.toMm(a);
      final (x1, y1) = v.toMm(b);
      final r = Rect.fromPoints(Offset(x0, y0), Offset(x1, y1));
      if (r.width * v.scale > 4 || r.height * v.scale > 4) {
        widget.onSelect({
          for (final sh in widget.design.shapes)
            if (r.contains(Offset(sh.bounds()[0], sh.bounds()[1])) && r.contains(Offset(sh.bounds()[2], sh.bounds()[3]))) sh.id
        });
      }
    }
    final d = _draft;
    if (d != null && (d.x1 != d.x0 || d.y1 != d.y0)) widget.onDraw(d);
    setState(() {
      _bandStart = _bandEnd = null;
      _draft = null;
    });
    _drawStart = null;
  }

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(builder: (context, box) {
      _size = Size(box.maxWidth, box.maxHeight);
      widget.zoom.fit = StockView(widget.design.stock, _size).fit;
      return Listener(
        onPointerDown: (_) => _pointers++,
        onPointerUp: (_) => _pointers = math.max(0, _pointers - 1),
        onPointerCancel: (_) => _pointers = math.max(0, _pointers - 1),
        child: GestureDetector(
          behavior: HitTestBehavior.opaque,
          onTapUp: _tap,
          onScaleStart: _start,
          onScaleUpdate: _update,
          onScaleEnd: _end,
          child: ListenableBuilder(
            listenable: widget.zoom,
            builder: (context, _) => CustomPaint(
              size: _size,
              painter: DesignPainter(widget.design,
                  zoom: widget.zoom.zoom,
                  pan: widget.zoom.pan,
                  selected: widget.selected,
                  highlighted: widget.highlighted,
                  pathDraft: widget.pathDraft,
                  draft: _draft,
                  band: _bandStart != null && _bandEnd != null ? Rect.fromPoints(_bandStart!, _bandEnd!) : null),
            ),
          ),
        ),
      );
    });
  }
}

/// A still picture of a design, for File › Open.
class DesignThumbnail extends StatelessWidget {
  final Design design;
  const DesignThumbnail(this.design, {super.key});
  @override
  Widget build(BuildContext context) =>
      CustomPaint(painter: DesignPainter(design, margin: 8, thumbnail: true), size: Size.infinite);
}

class DesignPainter extends CustomPainter {
  final Design design;
  final double zoom, margin;
  final Offset pan;
  final Set<String> selected, highlighted;
  final List<List<double>> pathDraft;
  final Draft? draft;
  final Rect? band;
  final bool thumbnail;

  DesignPainter(this.design,
      {this.zoom = 1,
      this.pan = Offset.zero,
      this.selected = const {},
      this.highlighted = const {},
      this.pathDraft = const [],
      this.draft,
      this.band,
      this.margin = 60,
      this.thumbnail = false});

  @override
  void paint(Canvas canvas, Size size) {
    canvas.drawRect(Offset.zero & size, Paint()..color = C.viewBg);
    final v = StockView(design.stock, size, zoom: zoom, pan: pan, margin: margin);
    final w = design.stock.width, h = design.stock.height;
    final board = Rect.fromPoints(v.toScreen(0, h), v.toScreen(w, 0));
    canvas.drawRect(board, Paint()..color = C.boardFill);

    // Grid: minor lines every 5 mm, majors every 5th; no minors closer than 6 px.
    const g = 5.0;
    final minors = g * v.scale >= 6;
    final minor = Paint()
      ..color = C.gridMinor
      ..strokeWidth = 1;
    final major = Paint()
      ..color = C.gridMajor
      ..strokeWidth = 1;
    for (var i = 1; i * g < w; i++) {
      if (i % 5 != 0 && !minors) continue;
      final x = v.toScreen(i * g, 0).dx;
      canvas.drawLine(Offset(x, board.top), Offset(x, board.bottom), i % 5 == 0 ? major : minor);
    }
    for (var i = 1; i * g < h; i++) {
      if (i % 5 != 0 && !minors) continue;
      final y = v.toScreen(0, i * g).dy;
      canvas.drawLine(Offset(board.left, y), Offset(board.right, y), i % 5 == 0 ? major : minor);
    }
    canvas.drawRect(
        board,
        Paint()
          ..color = C.boardEdge
          ..style = PaintingStyle.stroke
          ..strokeWidth = 1);
    final o = v.toScreen(0, 0);
    canvas.drawLine(o, v.toScreen(math.min(w, 15), 0), Paint()..color = C.axisX);
    canvas.drawLine(o, v.toScreen(0, math.min(h, 15)), Paint()..color = C.axisY);

    // The selected toolpath's shapes: a wide translucent pass, then a crisp one.
    for (final pass in [0, 1]) {
      final hp = Paint()
        ..color = pass == 0 ? C.halo.withAlpha(0x60) : C.halo
        ..style = PaintingStyle.stroke
        ..strokeWidth = pass == 0 ? 9 : 2
        ..strokeJoin = StrokeJoin.round
        ..strokeCap = StrokeCap.round;
      for (final s in design.shapes) {
        if (highlighted.contains(s.id)) _outline(canvas, v, s, hp);
      }
    }

    for (final s in design.shapes) {
      final sel = selected.contains(s.id);
      final pen = Paint()
        ..color = sel ? C.amber : C.element
        ..style = PaintingStyle.stroke
        ..strokeWidth = thumbnail ? 1 : (sel ? 2.2 : 1.3)
        ..isAntiAlias = true;
      if (s.type == Shape.point) {
        final p = v.toScreen(s.x, s.y);
        const k = 5.0;
        canvas.drawLine(p - const Offset(k, k), p + const Offset(k, k), pen);
        canvas.drawLine(p - const Offset(k, -k), p + const Offset(k, -k), pen);
        continue;
      }
      if (_closed(s)) {
        _outline(canvas, v, s, Paint()..color = (sel ? C.amber : Colors.white).withAlpha(sel ? 30 : 14));
      }
      _outline(canvas, v, s, pen);
    }

    final d = draft;
    if (d != null) {
      final s = shapeFromDraft(d, '');
      if (s != null) {
        _outline(
            canvas,
            v,
            s,
            Paint()
              ..color = C.preview
              ..style = PaintingStyle.stroke
              ..strokeWidth = 1.5);
      }
    }
    if (pathDraft.isNotEmpty) {
      final p = Paint()
        ..color = C.preview
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1.5;
      final pts = pathDraft.map((q) => v.toScreen(q[0], q[1])).toList();
      for (var i = 1; i < pts.length; i++) {
        canvas.drawLine(pts[i - 1], pts[i], p);
      }
      for (final q in pts) {
        canvas.drawRect(Rect.fromCenter(center: q, width: 7, height: 7), Paint()..color = C.preview);
      }
    }
    final b = band;
    if (b != null) {
      canvas.drawRect(b, Paint()..color = C.hoverLine.withAlpha(30));
      canvas.drawRect(
          b,
          Paint()
            ..color = C.hoverLine
            ..style = PaintingStyle.stroke
            ..strokeWidth = 1);
    }
  }

  static bool _closed(Shape s) => s.type != Shape.path ? s.type != Shape.point : s.closed == true;

  void _outline(Canvas canvas, StockView v, Shape s, Paint paint) {
    switch (s.type) {
      case Shape.rect:
        canvas.drawRect(Rect.fromPoints(v.toScreen(s.x, s.y + (s.h ?? 0)), v.toScreen(s.x + (s.w ?? 0), s.y)), paint);
      case Shape.circle:
        canvas.drawCircle(v.toScreen(s.x, s.y), (s.r ?? 0) * v.scale, paint);
      case Shape.polygon:
        final n = (s.sides ?? 6).clamp(3, 100);
        final r = s.r ?? 0;
        final path = Path();
        for (var i = 0; i < n; i++) {
          // As phobiCCCP makes it: vertex 0 on +X, counter-clockwise.
          final a = 2 * math.pi * i / n;
          final p = v.toScreen(s.x + r * math.cos(a), s.y + r * math.sin(a));
          i == 0 ? path.moveTo(p.dx, p.dy) : path.lineTo(p.dx, p.dy);
        }
        path.close();
        canvas.drawPath(path, paint);
      case Shape.text:
        // Glyph outlines in Liberation Sans (what Helvetica is on Linux);
        // `size` is the font's ascent in mm, as the desktop renders it.
        final style = paint.style == PaintingStyle.stroke
            ? (Paint()
              ..color = paint.color
              ..style = PaintingStyle.stroke
              ..strokeWidth = math.min(paint.strokeWidth, 2.2)
              ..strokeJoin = StrokeJoin.round)
            : paint;
        final tp = TextPainter(
          text: TextSpan(
              text: s.label ?? '',
              style: TextStyle(fontFamily: 'LiberationSans', fontSize: (s.size ?? 10) / 0.905 * v.scale, foreground: style)),
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
        canvas.drawLine(p - const Offset(5, 5), p + const Offset(5, 5), paint);
        canvas.drawLine(p - const Offset(5, -5), p + const Offset(5, -5), paint);
    }
  }

  @override
  bool shouldRepaint(DesignPainter old) =>
      old.design != design ||
      old.zoom != zoom ||
      old.pan != pan ||
      old.selected != selected ||
      old.highlighted != highlighted ||
      old.pathDraft != pathDraft ||
      old.draft != draft ||
      old.band != band;
}

double _r2(double v) => (v * 100).roundToDouble() / 100;

/// The shape a drawing drag makes, as the desktop's tools do: a rectangle
/// corner to corner, a circle or hexagon from its centre out to the finger.
Shape? shapeFromDraft(Draft d, String id) {
  final r = _r2(math.sqrt(math.pow(d.x1 - d.x0, 2) + math.pow(d.y1 - d.y0, 2)));
  switch (d.tool) {
    case DrawTool.rect:
      final w = (d.x1 - d.x0).abs(), h = (d.y1 - d.y0).abs();
      if (w < 0.01 || h < 0.01) return null;
      return Shape(id: id, type: Shape.rect, x: _r2(math.min(d.x0, d.x1)), y: _r2(math.min(d.y0, d.y1)), w: _r2(w), h: _r2(h));
    case DrawTool.circle:
      return r < 0.01 ? null : Shape(id: id, type: Shape.circle, x: _r2(d.x0), y: _r2(d.y0), r: r);
    case DrawTool.polygon:
      return r < 0.01 ? null : Shape(id: id, type: Shape.polygon, x: _r2(d.x0), y: _r2(d.y0), r: r, sides: 6);
    default:
      return null;
  }
}
