// The Area tool's fill (paint bucket), as phobiCCCP's vec::fillFace: the
// area around a tap, bounded by every closed shape around it and any it
// overlaps - not whole shapes. The outlines are cut into one planar map
// (every crossing a vertex) and the face holding the tap is traced in it.
// Millimetres; rings come back without a repeated closing point.

import 'dart:math' as math;

import 'model.dart';

typedef Pt = (double, double);

/// A closed shape's outline as a ring, or null for open paths, points and
/// text (the pad has no glyph outlines to fill against).
List<Pt>? shapeRing(Shape s) {
  switch (s.type) {
    case Shape.rect:
      final w = s.w ?? 0, h = s.h ?? 0;
      if (w <= 0 || h <= 0) return null;
      return [(s.x, s.y), (s.x + w, s.y), (s.x + w, s.y + h), (s.x, s.y + h)];
    case Shape.circle:
      final r = s.r ?? 0;
      if (r <= 0) return null;
      // Chords within 0.005 mm of the circle, as the desktop flattens it.
      final n = (math.pi / math.acos(math.max(0.0, 1 - 0.005 / r))).ceil().clamp(24, 720);
      return [
        for (var i = 0; i < n; i++) (s.x + r * math.cos(2 * math.pi * i / n), s.y + r * math.sin(2 * math.pi * i / n)),
      ];
    case Shape.polygon:
      final n = (s.sides ?? 6).clamp(3, 100), r = s.r ?? 0;
      if (r <= 0) return null;
      // Vertex 0 on +X, counter-clockwise, as phobiCCCP makes it.
      return [
        for (var i = 0; i < n; i++) (s.x + r * math.cos(2 * math.pi * i / n), s.y + r * math.sin(2 * math.pi * i / n)),
      ];
    case Shape.path:
      if (s.closed != true) return null;
      final pts = [
        for (final p in s.points ?? const <List<double>>[])
          if (p.length >= 2) (p[0], p[1]),
      ];
      if (pts.length > 1 && pts.first == pts.last) pts.removeLast();
      return pts.length >= 3 ? pts : null;
    default:
      return null;
  }
}

/// Signed area (positive = counter-clockwise).
double ringArea(List<Pt> r) {
  var a = 0.0;
  for (var i = 0; i < r.length; i++) {
    final p = r[i], q = r[(i + 1) % r.length];
    a += p.$1 * q.$2 - q.$1 * p.$2;
  }
  return a / 2;
}

bool ringContains(List<Pt> r, Pt p) {
  var inside = false;
  for (var i = 0, j = r.length - 1; i < r.length; j = i++) {
    final a = r[i], b = r[j];
    if ((a.$2 > p.$2) != (b.$2 > p.$2) && p.$1 < (b.$1 - a.$1) * (p.$2 - a.$2) / (b.$2 - a.$2) + a.$1) {
      inside = !inside;
    }
  }
  return inside;
}

// Grid the map is built on: 0.1 µm, so shared corners and edges of shapes
// meet exactly.
const _q = 10000.0;

// Gaps this narrow (0.02 mm, on the grid) count as touching: the desktop
// shrinks the face by 0.01 mm and grows it back, which cuts any channel
// narrower than twice that.
const _touch = 200;

typedef _V = (int, int);

// Flattened, a circle touching a side no longer touches it: the pieces hang
// together by a slit a few microns wide. Each corner within [_touch] of
// another shape's edge gets a short bridge to that edge, which cuts the slit
// the way the desktop's shrink-and-regrow does. The outlines themselves stay
// exactly as drawn (moving or bending them broke the map where outlines run
// within microns of each other).
List<(_V, _V, int)> _bridges(List<List<_V>> rings, List<List<int>> edgeOf) {
  final out = <(_V, _V, int)>[];
  final box = [for (final r in rings) [r.map((p) => p.$1).reduce(math.min) - _touch, r.map((p) => p.$2).reduce(math.min) - _touch, r.map((p) => p.$1).reduce(math.max) + _touch, r.map((p) => p.$2).reduce(math.max) + _touch]];
  // Arc length along each ring to each vertex: a ring's own edges count as
  // another shape's once they are more than 3 touch widths away along it.
  final arc = [
    for (final r in rings)
      () {
        final l = <double>[0];
        for (var k = 1; k <= r.length; k++) {
          final a = r[k - 1], b = r[k % r.length];
          l.add(l.last + math.sqrt(((b.$1 - a.$1) * (b.$1 - a.$1) + (b.$2 - a.$2) * (b.$2 - a.$2)).toDouble()));
        }
        return l;
      }()
  ];
  for (var ri = 0; ri < rings.length; ri++) {
    for (var pi = 0; pi < rings[ri].length; pi++) {
      final p = rings[ri][pi];
      for (var rj = 0; rj < rings.length; rj++) {
        final bx = box[rj];
        if (p.$1 < bx[0] || p.$1 > bx[2] || p.$2 < bx[1] || p.$2 > bx[3]) continue;
        final r = rings[rj];
        var best = double.infinity;
        var bestSeg = -1;
        _V bestQ = p;
        for (var k = 0; k < r.length; k++) {
          final a = r[k], b = r[(k + 1) % r.length];
          if (a == b || edgeOf[rj][k] < 0) continue;
          if (rj == ri) {
            final L = arc[ri].last;
            double along(int i, int j) { final d = (arc[ri][i] - arc[ri][j]).abs(); return math.min(d, L - d); }
            if (math.min(along(pi, k), along(pi, k + 1)) < 3 * _touch) continue;
          }
          if (p.$1 < math.min(a.$1, b.$1) - _touch || p.$1 > math.max(a.$1, b.$1) + _touch) continue;
          if (p.$2 < math.min(a.$2, b.$2) - _touch || p.$2 > math.max(a.$2, b.$2) + _touch) continue;
          final dx = (b.$1 - a.$1).toDouble(), dy = (b.$2 - a.$2).toDouble();
          final t = (((p.$1 - a.$1) * dx + (p.$2 - a.$2) * dy) / (dx * dx + dy * dy)).clamp(0.0, 1.0);
          final q = ((a.$1 + dx * t).round(), (a.$2 + dy * t).round());
          final ex = (p.$1 - q.$1).toDouble(), ey = (p.$2 - q.$2).toDouble();
          final d2 = ex * ex + ey * ey;
          if (d2 < best) {
            best = d2;
            bestSeg = edgeOf[rj][k];
            bestQ = q;
          }
        }
        // Touching already (on the edge, or a shared corner): nothing to cut.
        if (bestSeg < 0 || best > _touch * _touch || best <= 2) continue;
        out.add((p, bestQ, bestSeg));
      }
    }
  }
  return out;
}

/// The face around [at] bounded by [rings]: its outline first, then its
/// holes (the shapes standing inside it), wound the other way. Empty when
/// [at] is inside none.
List<List<Pt>> fillFace(List<List<Pt>> rings, Pt at) {
  // A tap inside none of the shapes (a window framed by overlapping bars)
  // fills nothing, as on the computer.
  if (!rings.any((r) => r.length >= 3 && ringContains(r, at))) return const [];
  // Every edge, on the grid, after closing the slits between near-touching
  // shapes.
  final grid = [for (final r in rings) [for (final p in r) _snap(p)]];
  final segs = <(_V, _V)>[];
  final edgeOf = <List<int>>[];
  for (final r in grid) {
    final ids = <int>[];
    for (var i = 0; i < r.length; i++) {
      final a = r[i], b = r[(i + 1) % r.length];
      ids.add(a != b ? segs.length : -1);
      if (a != b) segs.add((a, b));
    }
    edgeOf.add(ids);
  }
  if (segs.isEmpty) return const [];
  final bridges = _bridges(grid, edgeOf);
  final shapeSegs = segs.length;
  for (final (p, q, _) in bridges) {
    segs.add((p, q));
  }

  // Split each edge wherever another crosses or touches it.
  final cuts = [
    for (final s in segs) <_V>{s.$1, s.$2},
  ];
  // A bridge ends on its edge to within the grid's rounding: cut it there.
  for (final (_, q, seg) in bridges) {
    if (seg < shapeSegs) cuts[seg].add(q);
  }
  for (var i = 0; i < segs.length; i++) {
    final (a, b) = segs[i];
    for (var j = i + 1; j < segs.length; j++) {
      final (c, d) = segs[j];
      if (math.max(a.$1, b.$1) < math.min(c.$1, d.$1) ||
          math.max(c.$1, d.$1) < math.min(a.$1, b.$1) ||
          math.max(a.$2, b.$2) < math.min(c.$2, d.$2) ||
          math.max(c.$2, d.$2) < math.min(a.$2, b.$2)) {
        continue;
      }
      final d1 = _cross(a, b, c), d2 = _cross(a, b, d), d3 = _cross(c, d, a), d4 = _cross(c, d, b);
      if (d1 == 0 && d2 == 0) {
        // Collinear: each one's ends that lie on the other cut it.
        for (final p in [c, d]) {
          if (_onSeg(a, b, p)) cuts[i].add(p);
        }
        for (final p in [a, b]) {
          if (_onSeg(c, d, p)) cuts[j].add(p);
        }
        continue;
      }
      if (d1 == 0 && _onSeg(a, b, c)) cuts[i].add(c);
      if (d2 == 0 && _onSeg(a, b, d)) cuts[i].add(d);
      if (d3 == 0 && _onSeg(c, d, a)) cuts[j].add(a);
      if (d4 == 0 && _onSeg(c, d, b)) cuts[j].add(b);
      if ((d1 > 0) != (d2 > 0) && (d3 > 0) != (d4 > 0) && d1 != 0 && d2 != 0 && d3 != 0 && d4 != 0) {
        final t = d1 / (d1 - d2);
        final p = ((c.$1 + (d.$1 - c.$1) * t).round(), (c.$2 + (d.$2 - c.$2) * t).round());
        cuts[i].add(p);
        cuts[j].add(p);
      }
    }
  }

  // The planar map: each piece once, both ways round.
  final edges = <(_V, _V)>{};
  for (var i = 0; i < segs.length; i++) {
    final (a, b) = segs[i];
    final dx = (b.$1 - a.$1).toDouble(), dy = (b.$2 - a.$2).toDouble();
    final pts = cuts[i].toList()
      ..sort((p, q) => ((p.$1 - a.$1) * dx + (p.$2 - a.$2) * dy).compareTo((q.$1 - a.$1) * dx + (q.$2 - a.$2) * dy));
    for (var k = 0; k + 1 < pts.length; k++) {
      final u = pts[k], v = pts[k + 1];
      if (u == v || edges.contains((v, u))) continue;
      edges.add((u, v));
    }
  }
  final out = <_V, List<_V>>{};
  final parent = <_V, _V>{};
  _V find(_V v) {
    var r = v;
    while (parent[r] != r) {
      r = parent[r]!;
    }
    return parent[v] = r;
  }

  for (final (u, v) in edges) {
    (out[u] ??= []).add(v);
    (out[v] ??= []).add(u);
    parent.putIfAbsent(u, () => u);
    parent.putIfAbsent(v, () => v);
    parent[find(u)] = find(v);
  }
  for (final e in out.entries) {
    final o = e.key;
    e.value.sort(
      (p, q) => math
          .atan2((p.$2 - o.$2).toDouble(), (p.$1 - o.$1).toDouble())
          .compareTo(math.atan2((q.$2 - o.$2).toDouble(), (q.$1 - o.$1).toDouble())),
    );
  }

  // Walk every face: from u->v, leave v on the edge next clockwise from v->u,
  // which keeps the face on the left. Bounded faces come out
  // counter-clockwise; a connected piece's outside comes out clockwise.
  final seen = <(_V, _V)>{};
  final cycles = <(List<Pt>, double, _V)>[];
  for (final u in out.keys) {
    for (final v0 in out[u]!) {
      if (seen.contains((u, v0))) continue;
      final ring = <Pt>[];
      var a = u, b = v0;
      while (seen.add((a, b))) {
        ring.add((a.$1 / _q, a.$2 / _q));
        final around = out[b]!;
        final k = around.indexOf(a);
        final c = around[(k - 1 + around.length) % around.length];
        a = b;
        b = c;
      }
      cycles.add((ring, ringArea(ring), find(u)));
    }
  }

  // The bounded face holding the tap: the smallest counter-clockwise cycle around it.
  int smallestAround(Pt p, _V? notIn) {
    var best = -1;
    for (var i = 0; i < cycles.length; i++) {
      final (r, a, comp) = cycles[i];
      if (a <= 1e-9 || comp == notIn || !ringContains(r, p)) continue;
      if (best < 0 || a < cycles[best].$2) best = i;
    }
    return best;
  }

  final outer = smallestAround(at, null);
  if (outer < 0) return const [];
  final res = <List<Pt>>[cycles[outer].$1];
  // Holes: the outsides of other pieces whose nearest face around them is this one.
  for (final (r, a, comp) in cycles) {
    if (a >= -1e-9 || comp == cycles[outer].$3 || !ringContains(cycles[outer].$1, r.first)) continue;
    if (smallestAround(r.first, comp) == outer) res.add(r);
  }
  return res;
}

_V _snap(Pt p) => ((p.$1 * _q).round(), (p.$2 * _q).round());

int _cross(_V o, _V a, _V b) => (a.$1 - o.$1) * (b.$2 - o.$2) - (a.$2 - o.$2) * (b.$1 - o.$1);

bool _onSeg(_V a, _V b, _V p) =>
    _cross(a, b, p) == 0 &&
    p.$1 >= math.min(a.$1, b.$1) &&
    p.$1 <= math.max(a.$1, b.$1) &&
    p.$2 >= math.min(a.$2, b.$2) &&
    p.$2 <= math.max(a.$2, b.$2);

/// The same outline as [ring], to within a sliver: same box, corners on each other's outline.
bool sameRing(List<Pt> a, List<Pt> b) {
  List<double> box(List<Pt> r) => [
    r.map((p) => p.$1).reduce(math.min),
    r.map((p) => p.$2).reduce(math.min),
    r.map((p) => p.$1).reduce(math.max),
    r.map((p) => p.$2).reduce(math.max),
  ];
  final ba = box(a), bb = box(b);
  for (var i = 0; i < 4; i++) {
    if ((ba[i] - bb[i]).abs() > 0.05) return false;
  }
  // Every corner of each lies on the other's outline. An area rule let a
  // notch of a few mm on a big shape count as the whole shape; a notch's
  // corners are off the outline by its own size. The same rule as the
  // desktop's Area tool.
  return _near(a, b) && _near(b, a);
}

double _segDist(Pt p, Pt s, Pt e) {
  final dx = e.$1 - s.$1, dy = e.$2 - s.$2;
  final l2 = dx * dx + dy * dy;
  final t = l2 > 0 ? (((p.$1 - s.$1) * dx + (p.$2 - s.$2) * dy) / l2).clamp(0.0, 1.0) : 0.0;
  final x = s.$1 + t * dx - p.$1, y = s.$2 + t * dy - p.$2;
  return math.sqrt(x * x + y * y);
}

bool _near(List<Pt> from, List<Pt> to) {
  for (final v in from) {
    var best = double.infinity;
    for (var i = 0; i < to.length; i++) {
      best = math.min(best, _segDist(v, to[i], to[(i + 1) % to.length]));
    }
    if (best > 0.1) return false;
  }
  return true;
}
