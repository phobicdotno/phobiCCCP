// The desktop's own icons, drawn the same way (mainwindow.cpp toolIcon,
// drawModeIcon, toolpathIcon and fullScreenIcon; tooldiagram.cpp; brandlogo.h),
// so the pad shows the same pictures at the same sizes.

import 'dart:math' as math;

import 'package:flutter/material.dart';

import 'theme.dart';

/// A toolbar icon on the desktop's 20 × 20 grid (toolIcon / drawModeIcon).
class PIcon extends StatelessWidget {
  final String kind;
  final double size;
  final bool enabled;
  const PIcon(this.kind, {super.key, this.size = 20, this.enabled = true});
  @override
  Widget build(BuildContext context) => Opacity(
        opacity: enabled ? 1 : 0.35,
        child: CustomPaint(size: Size.square(size), painter: _ToolIconPainter(kind)),
      );
}

/// A toolpath type icon on the desktop's 26 × 26 grid (toolpathIcon).
class TpIcon extends StatelessWidget {
  final String type; // profile | pocket | drill | engrave
  final double size;
  final bool enabled;
  const TpIcon(this.type, {super.key, this.size = 26, this.enabled = true});
  @override
  Widget build(BuildContext context) => Opacity(
        opacity: enabled ? 1 : 0.35,
        child: CustomPaint(size: Size.square(size), painter: _TpIconPainter(type)),
      );
}

/// The bit as the tool list draws it (tooldiagram.cpp, small).
class ToolGlyph extends StatelessWidget {
  final String kind;
  final double height;
  const ToolGlyph(this.kind, {super.key, this.height = 30});
  @override
  Widget build(BuildContext context) => CustomPaint(size: Size(height * 24 / 36, height), painter: _ToolPainter(kind));
}

/// The phobiCCCP logo (brandlogo.h).
class Logo extends StatelessWidget {
  final double size;
  const Logo({super.key, this.size = 24});
  @override
  Widget build(BuildContext context) => CustomPaint(size: Size.square(size), painter: _LogoPainter());
}

Paint _stroke(Color c, double w) => Paint()
  ..color = c
  ..style = PaintingStyle.stroke
  ..strokeWidth = w
  ..isAntiAlias = true;

Paint _fill(Color c) => Paint()
  ..color = c
  ..style = PaintingStyle.fill
  ..isAntiAlias = true;

Path _poly(List<Offset> pts, {bool close = false}) {
  final p = Path()..moveTo(pts.first.dx, pts.first.dy);
  for (final q in pts.skip(1)) {
    p.lineTo(q.dx, q.dy);
  }
  if (close) p.close();
  return p;
}

/// [path] cut into dashes of [dash] with [gap] between, as Qt's dash patterns.
Path dashed(Path path, double dash, double gap) {
  final out = Path();
  for (final m in path.computeMetrics()) {
    for (double t = 0; t < m.length; t += dash + gap) {
      out.addPath(m.extractPath(t, math.min(t + dash, m.length)), Offset.zero);
    }
  }
  return out;
}

void _dashedLine(Canvas c, Offset a, Offset b, Paint paint, {double dash = 3, double gap = 2}) {
  final d = b - a;
  final len = d.distance;
  if (len == 0) return;
  final u = d / len;
  for (double t = 0; t < len; t += dash + gap) {
    c.drawLine(a + u * t, a + u * math.min(t + dash, len), paint);
  }
}

class _ToolIconPainter extends CustomPainter {
  final String kind;
  _ToolIconPainter(this.kind);

  @override
  void paint(Canvas c, Size size) {
    c.scale(size.width / 20);
    final pen = _stroke(C.text, 1.6);
    final blue = _stroke(C.hoverLine, 1.0);
    switch (kind) {
      case 'select':
        final arrow = _poly(const [Offset(6, 3), Offset(6, 15), Offset(9.5, 11.5), Offset(12, 16), Offset(13.8, 15), Offset(11.4, 10.6), Offset(15, 10)],
            close: true);
        c.drawPath(arrow, _fill(C.text));
        c.drawPath(arrow, pen);
      case 'hand':
        final h = Path()
          ..moveTo(6.2, 17.5)
          ..cubicTo(4.5, 15.5, 3.2, 13.0, 2.6, 11.2)
          ..cubicTo(2.2, 10.0, 3.6, 9.4, 4.4, 10.3)
          ..lineTo(5.6, 11.9)
          ..lineTo(5.6, 4.6)
          ..cubicTo(5.6, 3.4, 7.4, 3.4, 7.4, 4.6)
          ..lineTo(7.4, 9.4)
          ..lineTo(7.4, 3.3)
          ..cubicTo(7.4, 2.1, 9.3, 2.1, 9.3, 3.3)
          ..lineTo(9.3, 9.4)
          ..lineTo(9.3, 3.9)
          ..cubicTo(9.3, 2.7, 11.2, 2.7, 11.2, 3.9)
          ..lineTo(11.2, 9.6)
          ..lineTo(11.2, 5.6)
          ..cubicTo(11.2, 4.4, 13.0, 4.4, 13.0, 5.6)
          ..lineTo(13.0, 12.8)
          ..cubicTo(13.0, 15.0, 12.0, 16.5, 11.2, 17.5)
          ..close();
        c.drawPath(h, pen);
      case 'circle':
        c.drawOval(const Rect.fromLTWH(4, 4, 12, 12), pen);
        _dashedLine(c, const Offset(10, 10), const Offset(16, 10), blue);
        c.drawCircle(const Offset(10, 10), 1.7, _fill(C.hoverLine));
      case 'rect':
        c.drawRect(const Rect.fromLTWH(4, 5, 12, 10), pen);
      case 'polygon':
        final hex = <Offset>[
          for (var i = 0; i < 6; i++)
            Offset(10 + 6.5 * math.cos(math.pi / 3 * i - math.pi / 6), 10 + 6.5 * math.sin(math.pi / 3 * i - math.pi / 6))
        ];
        c.drawPath(_poly(hex, close: true), pen);
      case 'point':
        c.drawLine(const Offset(5, 5), const Offset(15, 15), pen);
        c.drawLine(const Offset(5, 15), const Offset(15, 5), pen);
      case 'path':
        c.drawPath(_poly(const [Offset(3, 16), Offset(8, 6), Offset(12, 12), Offset(17, 4)]), pen);
        for (final v in const [Offset(3, 16), Offset(8, 6), Offset(12, 12), Offset(17, 4)]) {
          c.drawCircle(v, 1.6, _fill(C.text));
        }
      case 'text':
        _text(c, 'T', 15, FontWeight.bold);
      case 'gcode':
        _text(c, 'G1', 11, FontWeight.bold);
      case 'delete':
        c.drawLine(const Offset(4, 6), const Offset(16, 6), pen);
        c.drawPath(_poly(const [Offset(8, 6), Offset(8, 4), Offset(12, 4), Offset(12, 6)]), pen);
        c.drawPath(_poly(const [Offset(5.5, 6), Offset(6.5, 17), Offset(13.5, 17), Offset(14.5, 6)]), pen);
        c.drawLine(const Offset(8.5, 9), const Offset(8.8, 14), pen);
        c.drawLine(const Offset(11.5, 9), const Offset(11.2, 14), pen);
      case 'fit':
        c.drawRect(const Rect.fromLTWH(4, 4, 12, 12), pen);
        c.drawLine(const Offset(8, 10), const Offset(12, 10), pen);
        c.drawLine(const Offset(10, 8), const Offset(10, 12), pen);
      case 'snap':
        for (var x = 4.0; x <= 16; x += 6) {
          for (var y = 4.0; y <= 16; y += 6) {
            c.drawCircle(Offset(x, y), 0.9, _fill(C.text));
          }
        }
        c.drawCircle(const Offset(10, 10), 3.2, pen);
      case 'properties':
        for (var i = 0; i < 3; i++) {
          final y = 5.0 + i * 5, k = i == 1 ? 13.0 : (i == 0 ? 7.0 : 10.0);
          c.drawLine(Offset(3, y), Offset(17, y), pen);
          c.drawCircle(Offset(k, y), 1.8, _fill(C.text));
        }
      case 'toolpaths':
        c.drawRect(const Rect.fromLTWH(3, 3, 14, 14), pen);
        c.drawPath(
            _poly(const [Offset(6, 6), Offset(14, 6), Offset(14, 10), Offset(6, 10), Offset(6, 14), Offset(14, 14)]),
            _stroke(const Color(0xFFF0A030), 1.6));
      case 'document':
        c.drawPath(_poly(const [Offset(5, 2.5), Offset(12, 2.5), Offset(16, 6.5), Offset(16, 17.5), Offset(5, 17.5)], close: true), pen);
        c.drawLine(const Offset(12, 2.5), const Offset(12, 6.5), pen);
        c.drawLine(const Offset(12, 6.5), const Offset(16, 6.5), pen);
        c.drawLine(const Offset(7.5, 10), const Offset(13.5, 10), pen);
        c.drawLine(const Offset(7.5, 13.5), const Offset(13.5, 13.5), pen);
      case 'machine':
        c.drawLine(const Offset(2, 17), const Offset(18, 17), pen);
        c.drawLine(const Offset(4, 17), const Offset(4, 4), pen);
        c.drawLine(const Offset(16, 17), const Offset(16, 4), pen);
        c.drawLine(const Offset(4, 4), const Offset(16, 4), pen);
        c.drawRect(const Rect.fromLTWH(8, 5, 4, 6), pen);
        c.drawLine(const Offset(10, 11), const Offset(10, 14), pen);
      case 'send':
        c.drawPath(_poly(const [Offset(3, 10), Offset(17, 3), Offset(12, 17), Offset(9.5, 11)], close: true), pen);
        c.drawLine(const Offset(9.5, 11), const Offset(17, 3), pen);
      case 'fullscreen':
        final p2 = _stroke(C.text, 2.0)
          ..strokeCap = StrokeCap.round
          ..strokeJoin = StrokeJoin.round;
        c.scale(20 / 22);
        c.drawPath(_poly(const [Offset(7, 3), Offset(3, 3), Offset(3, 19), Offset(7, 19)]), p2);
        c.drawPath(_poly(const [Offset(15, 3), Offset(19, 3), Offset(19, 19), Offset(15, 19)]), p2);
        c.drawLine(const Offset(7, 11), const Offset(15, 11), p2);
        c.drawLine(const Offset(11, 7), const Offset(11, 15), p2);
      case 'sync':
        c.drawArc(const Rect.fromLTWH(4, 4, 12, 12), -math.pi * 0.9, math.pi * 0.8, false, pen);
        c.drawArc(const Rect.fromLTWH(4, 4, 12, 12), math.pi * 0.1, math.pi * 0.8, false, pen);
        c.drawPath(_poly(const [Offset(14.5, 2.8), Offset(15.6, 6.4), Offset(12, 6.6)]), pen);
        c.drawPath(_poly(const [Offset(5.5, 17.2), Offset(4.4, 13.6), Offset(8, 13.4)]), pen);
    }
  }

  void _text(Canvas c, String s, double px, FontWeight w) {
    final tp = TextPainter(
      text: TextSpan(text: s, style: TextStyle(fontFamily: kFont, fontSize: px, fontWeight: w, color: C.text, height: 1)),
      textDirection: TextDirection.ltr,
    )..layout();
    tp.paint(c, Offset((20 - tp.width) / 2, (20 - tp.height) / 2));
  }

  @override
  bool shouldRepaint(_ToolIconPainter old) => old.kind != kind;
}

class _TpIconPainter extends CustomPainter {
  final String type;
  _TpIconPainter(this.type);

  @override
  void paint(Canvas c, Size size) {
    c.scale(size.width / 26);
    final pen = _stroke(C.text, 1.6)
      ..strokeCap = StrokeCap.round
      ..strokeJoin = StrokeJoin.round;
    const box = Rect.fromLTWH(5, 5, 16, 16);
    switch (type) {
      case 'profile':
        c.drawRRect(RRect.fromRectAndRadius(box, const Radius.circular(3)), pen);
        final dash = _stroke(C.amber, 1.4)..strokeCap = StrokeCap.round;
        c.drawPath(dashed(Path()..addRRect(RRect.fromRectAndRadius(box.inflate(2.5), const Radius.circular(4))), 4.2, 2.8), dash);
      case 'pocket':
        c.drawRRect(RRect.fromRectAndRadius(box, const Radius.circular(3)), pen);
        for (var i = 0; i < 5; i++) {
          c.drawLine(Offset(7, 8 + i * 2.6), Offset(19, 8 + i * 2.6), _stroke(C.amber, 1.2));
        }
      case 'drill':
        for (final o in const [Offset(8, 8), Offset(18, 8), Offset(8, 18), Offset(18, 18)]) {
          c.drawCircle(o, 2.6, _fill(C.amber));
        }
        c.drawLine(const Offset(13, 10), const Offset(13, 16), pen);
        c.drawLine(const Offset(10, 13), const Offset(16, 13), pen);
      case 'engrave':
        final a = Path()
          ..moveTo(4, 20)
          ..cubicTo(6, 4, 12, 4, 13, 20)
          ..cubicTo(14, 10, 18, 8, 22, 18);
        c.drawPath(
            a,
            _stroke(C.amber, 1.4)
              ..strokeCap = StrokeCap.round
              ..strokeJoin = StrokeJoin.round);
      default:
        c.drawRRect(RRect.fromRectAndRadius(box, const Radius.circular(3)), pen);
    }
  }

  @override
  bool shouldRepaint(_TpIconPainter old) => old.type != type;
}

class _ToolPainter extends CustomPainter {
  final String kind;
  _ToolPainter(this.kind);

  @override
  void paint(Canvas c, Size size) {
    final k = math.min(size.width / 200, size.height / 300);
    c.translate(size.width / 2 - 100 * k, size.height / 2 - 150 * k);
    c.scale(k);
    final lineW = 2.4 / math.max(k, 0.05) * 0.5;
    const sh = 30.0, top = 20.0, joint = 150.0, tip = 270.0;
    var cutHalf = 24.0;
    final out = Path()
      ..moveTo(100 - sh, top)
      ..lineTo(100 + sh, top);
    final flutes = <(Offset, Offset)>[];
    if (kind == 'vbit' || kind == 'engraver') {
      final flat = kind == 'engraver' ? 4.0 : 0.0;
      out
        ..lineTo(100 + sh, joint)
        ..lineTo(100 + flat, tip);
      if (flat > 0) out.lineTo(100 - flat, tip);
      out
        ..lineTo(100 - sh, joint)
        ..close();
      flutes.add((const Offset(100 - sh * 0.55, joint + 10), const Offset(108, tip - 30)));
      flutes.add((const Offset(100 + sh * 0.2, joint + 6), const Offset(100 + sh * 0.7, joint + 40)));
    } else {
      out
        ..lineTo(100 + sh, joint - 20)
        ..lineTo(100 + cutHalf, joint);
      if (kind == 'ball') {
        out
          ..lineTo(100 + cutHalf, tip - cutHalf)
          ..arcTo(Rect.fromLTWH(100 - cutHalf, tip - 2 * cutHalf, 2 * cutHalf, 2 * cutHalf), 0, math.pi, false);
      } else {
        out
          ..lineTo(100 + cutHalf, tip)
          ..lineTo(100 - cutHalf, tip);
      }
      out
        ..lineTo(100 - cutHalf, joint)
        ..lineTo(100 - sh, joint - 20)
        ..close();
      for (var i = 0; i < 3; i++) {
        final y = joint + 12 + i * 32;
        flutes.add((Offset(100 - cutHalf, y), Offset(100 + cutHalf, y + 26)));
      }
    }
    c.drawPath(out, _fill(const Color(0x0EFFFFFF)));
    c.drawPath(
        out,
        _stroke(const Color(0xFFC5CAD3), lineW)
          ..strokeJoin = StrokeJoin.round
          ..strokeCap = StrokeCap.round);
    for (final (a, b) in flutes) {
      c.drawLine(a, b, _stroke(const Color(0xFF6B7380), lineW * 0.8));
    }
  }

  @override
  bool shouldRepaint(_ToolPainter old) => old.kind != kind;
}

class _LogoPainter extends CustomPainter {
  @override
  void paint(Canvas c, Size size) {
    c.scale(size.width / 128);
    c.drawRRect(RRect.fromRectAndRadius(const Rect.fromLTWH(0, 0, 128, 128), const Radius.circular(22)), _fill(C.red));
    final grid = _stroke(const Color(0xFFA30000), 1);
    for (final v in const [32.0, 64.0, 96.0]) {
      c.drawLine(Offset(16, v), Offset(112, v), grid);
      c.drawLine(Offset(v, 16), Offset(v, 112), grid);
    }
    final dash = _stroke(const Color(0xFFFF8A80), 1.5);
    c.drawPath(dashed(Path()..addOval(Rect.fromCircle(center: const Offset(64, 66), radius: 42)), 6, 4.5), dash);
    c.drawPath(
        _poly(const [Offset(64, 24), Offset(88.7, 100), Offset(24.1, 53), Offset(103.9, 53), Offset(39.3, 100)], close: true), dash);
    c.drawPath(
        _poly(const [
          Offset(64.0, 24.0), Offset(74.0, 52.2), Offset(103.9, 53.0), Offset(80.2, 71.3), Offset(88.7, 100.0), //
          Offset(64.0, 83.0), Offset(39.3, 100.0), Offset(47.8, 71.3), Offset(24.1, 53.0), Offset(54.0, 52.2),
        ], close: true),
        _fill(C.gold));
  }

  @override
  bool shouldRepaint(_LogoPainter old) => false;
}
