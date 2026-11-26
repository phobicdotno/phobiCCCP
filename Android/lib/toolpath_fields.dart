// The desktop toolpath form's fields (src/toolpathfields.cpp catalog,
// unused-for rules and cards), for the pad's "All settings": every setting
// a toolpath carries in ToolpathSpec.set, with the same labels, units,
// ranges and tips. The computer holds every value to the same range again
// when it builds the toolpath (padlink.cpp, applyPadSettings).

import 'model.dart';

enum TpFieldKind { depth, number, integer, toggle, invToggle, choice }

class TpField {
  final String key, card, label, unit, tip;
  final TpFieldKind kind;
  final double min, max, step;
  final int decimals;
  final List<(Object, String)> choices;

  /// A switch key: the field is greyed while it is off.
  final String? enabledBy;

  const TpField(this.key, this.card, this.label, this.kind,
      {this.unit = '',
      this.min = 0,
      this.max = 100000,
      this.step = 0.1,
      this.decimals = 2,
      this.choices = const [],
      this.tip = '',
      this.enabledBy});

  bool get isNumber => kind == TpFieldKind.depth || kind == TpFieldKind.number || kind == TpFieldKind.integer;
}

/// Card order and titles, as the desktop form shows them.
const tpCardOrder = ['depth', 'cut', 'entry', 'tabs', 'rest', 'finish'];
const tpCardTitles = {
  'depth': 'Depth',
  'cut': 'Cutting',
  'entry': 'Entry',
  'tabs': 'Tabs',
  'rest': 'Rest machining',
  'finish': 'Finishing passes',
};

/// The computer's type string for a pad toolpath type.
String ccToolpathType(String padType) =>
    const {'profile': 'contour', 'pocket': 'pocket_toolpath', 'drill': 'drilling_toolpath', 'engrave': 'engrave_toolpath'}[
        padType] ??
    padType;

TpField _mm(String key, String card, String label, {double max = 1000, String tip = '', String? enabledBy}) =>
    TpField(key, card, label, TpFieldKind.number, unit: 'mm', max: max, tip: tip, decimals: 3, enabledBy: enabledBy);
TpField _depth(String key, String label, {String tip = ''}) =>
    TpField(key, 'depth', label, TpFieldKind.depth, unit: 'mm', max: 1000, tip: tip, decimals: 3);
TpField _deg(String key, String card, String label, double min, double max, {String tip = '', String? enabledBy}) =>
    TpField(key, card, label, TpFieldKind.number,
        unit: '°', min: min, max: max, step: 1, decimals: 1, tip: tip, enabledBy: enabledBy);
TpField _sw(String key, String card, String label, {String tip = '', String? enabledBy}) =>
    TpField(key, card, label, TpFieldKind.toggle, tip: tip, enabledBy: enabledBy);
TpField _choice(String key, String card, String label, List<(Object, String)> ch, {String tip = ''}) =>
    TpField(key, card, label, TpFieldKind.choice, choices: ch, tip: tip);

/// The designed fields, in display order, for the computer's toolpath [type]
/// (a key may come twice: the first one counts, as on the desktop).
List<TpField> tpFieldCatalog(String type) {
  final c = <TpField>[];
  // Depth
  c.add(_depth('start_depth', 'Start', tip: 'Depth the first pass starts from'));
  c.add(type == 'fluting_toolpath'
      ? _depth('end_depth', 'Flute depth', tip: 'Depth of the groove between the ramps')
      : _depth('end_depth', 'End'));
  c.add(_depth('cut_depth', 'Cut depth'));
  c.add(_depth('break_through', 'Break through', tip: 'Extra depth below the stock'));
  c.add(_depth('min_depth', 'Shallowest'));
  c.add(_depth('max_depth', 'Deepest'));
  c.add(_mm('stepdown', 'depth', 'Per pass', max: 100));
  c.add(_mm('depth_per_pass', 'depth', 'Per pass', max: 100));
  c.add(_mm('thread_depth', 'depth', 'Thread depth', max: 50, tip: '0 = from the pitch'));

  // Cutting
  if (type == 'contour') {
    c.add(_choice('ofset_dir', 'cut', 'Side', const [(-1, 'Inside'), (0, 'On the line'), (1, 'Outside')]));
  }
  if (type == 'thread_toolpath') {
    c.add(_choice('side', 'cut', 'Thread', const [('internal', 'Internal (nut)'), ('external', 'External (bolt)')]));
  } else if (type == 'circular_toolpath') {
    c.add(_choice('side', 'cut', 'Shape', const [('inside', 'Round pocket'), ('outside', 'Boss')]));
  } else {
    c.add(_choice('side', 'cut', 'Side', const [('outside', 'Outside'), ('inside', 'Inside')]));
  }
  c.add(_sw('flip_inside_outside', 'cut', 'Cut inside the line'));
  if (type == 'contour' || type == 'pocket_toolpath' || type == 'cutout') {
    c.add(_choice('corners', 'cut', 'Corners', const [('off', 'Sharp (off)'), ('dogbone', 'Dogbone'), ('tbone', 'T-bone')],
        tip: 'Cut a bit-sized bite into each inside corner so a square part fits. '
            'Dogbone goes diagonally, T-bone along the longer side.'));
  }
  c.add(_choice('mode', 'cut', 'Engrave', const [('outline', 'Outline'), ('fill', 'Hatch fill'), ('both', 'Outline + fill')]));
  if (type == '3d_finish_toolpath') {
    c.add(_choice('strategy', 'cut', 'Strategy',
        const [('raster', 'Raster'), ('waterline', 'Waterline (Z-level)'), ('pencil', 'Pencil (creases only)')],
        tip: 'Raster: parallel passes over the surface. Waterline: one pass around every wall per stepdown. '
            'Pencil: only the valleys and inside corners a bigger bit could not reach'));
    c.add(_mm('pencil_ref_diameter', 'cut', 'Bigger bit Ø',
        max: 100,
        tip: 'Pencil: cut only where this bigger bit (the one that did the main finish) left material. 0 = twice this bit'));
    c.add(_choice('direction', 'cut', 'Direction',
        const [('climb', 'Climb'), ('conventional', 'Conventional'), ('zigzag', 'Zigzag')]));
  }
  if (type == 'pocket_toolpath') {
    c.add(_choice('pocket_strategy', 'cut', 'Strategy', const [('offset', 'Offset (rings)'), ('raster', 'Raster')],
        tip: 'Offset: rings that follow the walls inward. Raster: one lap of the wall, then straight '
            'back-and-forth lines. With "Only what a bigger tool left" on, rings are cut either way'));
    c.add(_deg('raster_angle', 'cut', 'Raster angle', -180, 180,
        tip: 'Direction of the raster lines; 0 = along X, 90 = along Y'));
  }
  c.add(_mm('stepover', 'cut', 'Stepover', max: 100, tip: 'Sideways step between passes'));
  c.add(_mm('stock_to_leave', 'cut', 'Leave', max: 50, tip: 'Material left on the walls for a later pass'));
  c.add(_sw('climb', 'cut', 'Climb milling'));
  c.add(_mm('chamfer_width', 'cut', 'Chamfer width', max: 50));
  c.add(_mm('tip_offset', 'cut', 'Tip offset', max: 20));
  c.add(_mm('boss_clearance', 'cut', 'Boss clearance', max: 100));
  c.add(const TpField('pitch', 'cut', 'Pitch', TpFieldKind.number, unit: 'mm', max: 20, decimals: 4));
  c.add(const TpField('passes', 'cut', 'Passes', TpFieldKind.integer, min: 1, max: 20, step: 1, decimals: 0));
  c.add(_sw('left_hand', 'cut', 'Left-hand thread'));
  c.add(_choice('ramp_ends', 'entry', 'Ramp', const [('both', 'Both ends'), ('start', 'Start only'), ('end', 'End only')]));
  c.add(const TpField('ramp_length', 'entry', 'Ramp length', TpFieldKind.number,
      max: 1000, decimals: 2, step: 1, tip: 'How long the groove takes to reach full depth, in mm or % of the vector'));
  c.add(_choice('ramp_unit', 'entry', 'Ramp length in', const [('percent', '% of the vector'), ('mm', 'mm')]));
  c.add(_choice('ramp_shape', 'entry', 'Ramp shape', const [('linear', 'Straight'), ('smooth', 'Smooth (rounded)')]));
  c.add(_mm('length', 'cut', 'Slot length', max: 500));
  c.add(_deg('angle', 'cut', 'Angle', -360, 360));
  c.add(_mm('line_spacing', 'cut', 'Line spacing', max: 50));
  c.add(_sw('crosshatch', 'cut', 'Crosshatch'));
  c.add(_mm('min_length', 'cut', 'Shortest stroke', max: 1000));
  c.add(_mm('max_length', 'cut', 'Longest stroke', max: 1000));
  c.add(const TpField('stepover_variation', 'cut', 'Stepover variation', TpFieldKind.number, max: 1, step: 0.05));
  c.add(_mm('peck_distance', 'cut', 'Peck', max: 100, tip: '0 = drill in one go'));
  c.add(const TpField('dwell', 'cut', 'Dwell', TpFieldKind.number, unit: 's', max: 60));
  c.add(_sw('chip_break', 'cut', 'Chip break'));
  c.add(_mm('chip_retract', 'cut', 'Back off', max: 10, enabledBy: 'chip_break'));
  c.add(const TpField('speeds.feedrate', 'cut', 'Feed', TpFieldKind.number, unit: 'mm/min', max: 20000, step: 50, decimals: 0));
  c.add(const TpField('speeds.plungerate', 'cut', 'Plunge', TpFieldKind.number,
      unit: 'mm/min', max: 20000, step: 50, decimals: 0));
  c.add(const TpField('speeds.rpm', 'cut', 'Spindle', TpFieldKind.number, unit: 'rpm', max: 30000, step: 500, decimals: 0));

  // Entry
  c.add(_sw('enable_ramping', 'entry', 'Ramp in'));
  c.add(type == 'adaptive_toolpath'
      ? _deg('ramp_angle', 'entry', 'Ramp angle', 0.5, 15)
      : _deg('ramp_angle', 'entry', 'Ramp angle', 0, 45));
  c.add(_sw('helix_entry', 'entry', 'Helix in'));
  c.add(_mm('helix_diameter', 'entry', 'Helix Ø', max: 200, enabledBy: 'helix_entry'));
  c.add(_deg('helix_angle', 'entry', 'Helix angle', 0.5, 15, enabledBy: 'helix_entry'));
  c.add(_mm('lead_radius', 'entry', 'Lead-in arc', max: 50, tip: '0 = straight in'));

  // Tabs
  c.add(const TpField('ignore_tabs', 'tabs', 'Tabs', TpFieldKind.invToggle,
      tip: 'Hold the part with small bridges on the last passes'));
  c.add(_mm('tab_height', 'tabs', 'Height', max: 50));
  c.add(_mm('tab_width', 'tabs', 'Width', max: 30));

  // Rest machining
  if (type == '3d_rough_toolpath') {
    c.add(_sw('enable_rest', 'rest', 'Rest: only what a bigger bit left',
        tip: 'Cut only the corners and narrow spots a bigger 3D Rough could not get into'));
    c.add(_sw('rest_auto', 'rest', 'Bigger bit: the 3D Rough above',
        tip: 'Use the bit of the nearest 3D Rough above this one in the list', enabledBy: 'enable_rest'));
    c.add(_mm('rest_diameter', 'rest', 'Previous bit Ø',
        max: 100, tip: 'Used when auto is off or there is no 3D Rough above', enabledBy: 'enable_rest'));
  }
  c.add(_sw('rest_auto', 'rest', 'Skip what the toolpaths above cut'));
  c.add(_mm('rest_overlap', 'rest', 'Overlap', max: 50, enabledBy: 'rest_auto'));
  c.add(_sw('enable_rest', 'rest', 'Only what a bigger tool left'));
  c.add(_mm('rest_diameter', 'rest', 'Bigger tool Ø', max: 100, enabledBy: 'enable_rest'));

  // Finishing
  c.add(const TpField('finish_passes', 'finish', 'Passes', TpFieldKind.integer, min: 0, max: 10, step: 1, decimals: 0));
  c.add(_mm('finish_stepover', 'finish', 'Stepover', max: 10));
  c.add(_sw('spring_pass', 'finish', 'Spring pass'));
  return c;
}

/// A key this type carries that the export does not use: no field
/// (toolpathFieldUnused).
bool tpFieldUnused(String type, String k) {
  bool isIn(List<String> t) => t.contains(type);
  switch (k) {
    case 'enable_ramping':
      return isIn(const ['drilling_toolpath', 'keyhole_toolpath', 'advanced_vcarve_toolpath', 'face_toolpath',
        'bore_toolpath', 'thread_toolpath', 'slot_toolpath', 'circular_toolpath', 'adaptive_toolpath', 'fluting_toolpath']);
    case 'ramp_angle':
      return isIn(const ['cutout', 'drilling_toolpath', 'keyhole_toolpath', 'advanced_vcarve_toolpath', 'face_toolpath',
        'bore_toolpath', 'thread_toolpath', 'slot_toolpath', 'fluting_toolpath']);
    case 'stock_to_leave':
      return isIn(const ['cutout', 'drilling_toolpath', 'keyhole_toolpath', 'advanced_vcarve_toolpath']);
    case 'stepdown':
      return isIn(const ['drilling_toolpath', 'keyhole_toolpath', 'thread_toolpath', 'fluting_toolpath']);
    case 'stepover':
      return isIn(const ['drilling_toolpath', 'keyhole_toolpath']);
    case 'angle':
      return isIn(const ['pocket_toolpath', 'drilling_toolpath', 'advanced_vcarve_toolpath']);
    case 'start_depth':
      return isIn(const ['3d_rough_toolpath', '3d_finish_toolpath']);
  }
  return false;
}

/// Keys the pad toolpath holds in its own fields rather than in `set`
/// (padlink.cpp padSimpleKeys). [shownElsewhere]: the ones the toolpath
/// form already has a row for (End, Side, Tabs), left out of All settings.
bool _shownElsewhere(String padType, String key) =>
    key == 'end_depth' ||
    (padType == 'profile' && const {'ofset_dir', 'ignore_tabs', 'tab_height', 'tab_width'}.contains(key));

/// The value of desktop setting [key] for [t]: its own field for the keys the
/// pad has one for, else from [ToolpathSpec.set]. Null: the toolpath has no
/// such setting.
Object? tpSetting(ToolpathSpec t, String key) {
  if (key == 'end_depth') return t.depth;
  if (t.type == 'profile') {
    switch (key) {
      case 'ofset_dir':
        return t.side == 'inside' ? -1 : t.side == 'on' ? 0 : 1;
      case 'ignore_tabs':
        return !t.tabs;
      case 'tab_height':
        return t.tabHeight ?? 3.0;
      case 'tab_width':
        return t.tabWidth ?? 12.0;
    }
  }
  if (t.type == 'profile' || t.type == 'pocket') {
    if (key == 'corners') return t.extra['corners'] is String ? t.extra['corners'] : 'off';
  }
  if (t.type == 'pocket') {
    if (key == 'pocket_strategy') return t.extra['strategy'] == 'raster' ? 'raster' : 'offset';
    if (key == 'raster_angle') return t.extra['rasterAngle'] is num ? t.extra['rasterAngle'] : 0.0;
  }
  return t.set[key];
}

/// [t] with desktop setting [key] set to [v] (held to [f]'s range), in the
/// pad's own field where it has one, else in [ToolpathSpec.set].
ToolpathSpec tpWithSetting(ToolpathSpec t, TpField f, Object v) {
  final key = f.key;
  if (v is num && f.isNumber) {
    final d = v.toDouble().clamp(f.min, f.max).toDouble();
    v = f.kind == TpFieldKind.integer ? d.round() : d;
  }
  if (key == 'end_depth' && v is num) return t.copyWith(depth: v.toDouble());
  if (t.type == 'profile') {
    switch (key) {
      case 'ofset_dir':
        return t.copyWith(side: v == -1 ? 'inside' : v == 0 ? 'on' : 'outside');
      case 'ignore_tabs':
        return t.copyWith(tabs: v != true);
      case 'tab_height':
        return t.copyWith(tabHeight: (v as num).toDouble());
      case 'tab_width':
        return t.copyWith(tabWidth: (v as num).toDouble());
    }
  }
  if ((t.type == 'profile' || t.type == 'pocket') && key == 'corners') {
    final e = Map<String, Object?>.of(t.extra);
    v == 'off' ? e.remove('corners') : e['corners'] = v;
    return t.copyWith(extra: e);
  }
  if (t.type == 'pocket' && (key == 'pocket_strategy' || key == 'raster_angle')) {
    final e = Map<String, Object?>.of(t.extra);
    if (key == 'pocket_strategy') {
      v == 'raster' ? e['strategy'] = 'raster' : e.remove('strategy');
    } else {
      v == 0 ? e.remove('rasterAngle') : e['rasterAngle'] = v;
    }
    return t.copyWith(extra: e);
  }
  return t.copyWith(set: {...t.set, key: v});
}

/// The fields of [t] for All settings, in card order: the designed fields it
/// has a value for, as the desktop form shows them (unused ones left out,
/// raster angle only for a raster pocket, the bigger bit only for pencil).
List<TpField> tpVisibleFields(ToolpathSpec t) {
  final type = ccToolpathType(t.type);
  final seen = <String>{};
  final byCard = <String, List<TpField>>{};
  for (final f in tpFieldCatalog(type)) {
    if (!seen.add(f.key)) continue;
    if (tpFieldUnused(type, f.key) || _shownElsewhere(t.type, f.key)) continue;
    final v = tpSetting(t, f.key);
    if (v == null) continue;
    if (f.key == 'raster_angle' && tpSetting(t, 'pocket_strategy') != 'raster') continue;
    if (f.key == 'pencil_ref_diameter' && tpSetting(t, 'strategy') != 'pencil') continue;
    (byCard[f.card] ??= []).add(f);
  }
  return [for (final card in tpCardOrder) ...?byCard[card]];
}

/// Keys of [ToolpathSpec.set] no designed field shows: listed read-only.
List<String> tpOtherKeys(ToolpathSpec t) {
  final known = {for (final f in tpFieldCatalog(ccToolpathType(t.type))) f.key};
  return [for (final k in t.set.keys) if (!known.contains(k)) k]..sort();
}

/// False while the switch [f] depends on is off (greyed, as on the desktop).
bool tpFieldEnabled(ToolpathSpec t, TpField f) => f.enabledBy == null || tpSetting(t, f.enabledBy!) != false;
