# Changelog

## Unreleased
- Path tool draws splines pen-style: click = corner node, click-drag pulls out
  symmetric tangent handles, live curve preview, Enter finishes open, clicking
  the start closes. Written in Carbide Create's path schema (cp1 = previous
  out-handle, cp2 = own in-handle per anchor, point_type 1/3 per segment,
  duplicate-start + close rows), so CC renders the same curve.
- Node edit tool (N, palette icon): anchors as squares (diamonds for smooth
  nodes) and handles as circles on tangent lines; drag anchors/handles
  (Alt/Shift-drag breaks handle symmetry), double-click a segment to insert a
  node (de Casteljau split, curve preserved), Delete removes the selected node,
  right-click a node for Corner / Smooth / Symmetric / straight lines. Works on
  open and closed paths; every drag/insert/delete/kind change is one undo step.
  Editing a node of a rectangle/circle/polygon converts it to a path.
- Right-click → Convert to path (Select or Nodes tool, also a Properties
  button): rectangles/circles/polygons become one editable path keeping their
  id; text becomes one closed path per glyph contour.
- Text along arcs: `arc_enabled` / `arc_radius` / `arc_center` /
  `arc_angle_offset` / `arc_text_on_bottom` are honoured when text is
  (re)rendered — glyphs placed along the circle with the baseline on the arc,
  centred on the top (or the bottom, upright, extending inwards) turned by the
  offset angle. The `rendered` outlines stored in the element are the arc
  layout, so toolpaths cut what is shown. Text without `rendered` is laid out
  on load.
- Properties panel for text: string, font family, height, bold/italic, arc
  on/off, arc radius, angle offset, text on bottom — all undoable and
  rewriting `font` / `qtfont` / `rendered` like CC.
- `font_height` now scales the font so its ascent equals the value (matches
  CC's Helvetica specimen: cap height = 0.93 × font_height).
- Selection survives document rebuilds (property edits no longer blank the
  Properties panel).
- Tests: `test_paths` (bezier JSON round trip, node insert/delete, node kinds,
  arc text centring/span/side/offset, conversions) and `test_canvas` (pen and
  node tools driven with synthetic events, undo steps) wired into CTest;
  `--shot` honours `PHOBICCCP_SHOT_SELECT=<geometryType>` and
  `PHOBICCCP_SHOT_TOOL=nodes` for headless screenshots.

## v0.2.4 (build 12) — 2026-09-05
- CLI modes (`--export`, `--selftest`, `--grbl-*`, `--shot`) run without a
  display: the offscreen Qt platform is selected automatically when no
  DISPLAY / WAYLAND_DISPLAY is set instead of aborting.
- `tools/flowtest.sh` injects the longer second tool right after the reference
  measurement instead of racing the second probe.

## v0.2.3 (build 11) — 2026-09-05
- Hold-to-jog now streams short `$J=` increments (2 ticks of travel every
  120 ms) instead of one long jog, so motion stops by itself within a fraction
  of a second if the release event is ever lost; release still jog-cancels.
- Jog buttons give the Machine panel keyboard focus, so arrow / PgUp / PgDn
  jogging works right after clicking the pad.
- Zeroing sends `Z0` (no stray space); dead code removed from `zero()`.
- README: verification status of the BitSetter / tool-change flow (simulator
  only so far).

## v0.2.2 (build 10) — 2026-09-05
- G-code export honours `stock_to_leave`: contours offset by tool radius +
  stock (inside or outside), pockets inset their walls by tool radius + stock.
  Cutouts never leave stock.
- Geometry test updated for arc output and for every Carbide Create toolpath
  type now being supported; all 24 checks and all three CTest tests pass.

## v0.2.1 (build 9) — 2026-09-05
- V-carve: medial-axis spurs at flattening vertices are pruned (a vertex where
  the boundary turns < 20° is a curve, not a corner), so circles carve as one
  plunge and curved letters no longer get one spoke per polygon vertex. The
  v-carve now uses the same fine curve flattening as the other toolpaths.
  All-types specimen: v-carve motion 16702 → 6592 mm, same coverage.
- Removed the dead pre-streamer machine dialog (`machinedialog.*`,
  `grblsender.*`).
- Build split into a `phobicccp_core` library + app; `tests/test_geometry.cpp`
  is built again (`test_geometry`) and wired into CTest together with the
  simulator self-test and the machine flow test: `ctest --test-dir build`.
- Self-test's g-code stage checks every toolpath in the file instead of a
  hard-coded count, so it also passes on the all-types specimen.

## v0.2.0 (build 8) — 2026-09-04
- Machine dock rewritten: work + machine position display (WCO tracking),
  jog speeds and press-and-hold continuous jogging with jog-cancel, keyboard
  jogging, Zero X/Y/Z/XY/all, go-to X0Y0, editable port with USB description.
- BitSetter tool-length probing: configurable button position (machine
  coords), safe Z, fast/slow feeds, tool-change spot; reference tool taken
  when zeroing Z; later measurements apply `G43.1` offsets, re-applied after
  reset/unlock; settings persisted.
- Automatic tool-change flow while streaming: `M0 ;T<n>` parks the machine,
  prompts, measures the new tool, applies the offset, continues. Ad-hoc
  commands, macros and the program stream no longer confuse each other's acks.
- Isometric animated 3D preview tab of the machining route with live machine
  position.
- CLI: `--grbl-probe`, `--grbl-run` (headless tool-change flow) and
  `tools/grblsim.py`, a GRBL 1.1h simulator on a pty for testing without a
  machine.

## v0.1.6 (build 7) — 2026-09-04
- G-code export: fixed a 0.12 mm undersize on every offset toolpath. Element
  curves were flattened at Qt's default tolerance (a Ø6.6 circle became a
  ~16-gon) before Clipper offset them, so rings shrank by 0.06 mm at a 3 mm
  tool radius. Curves are now flattened at 0.005 mm; the baseplate holes come
  out Ø6.60 / Ø12.70 instead of Ø6.48 / Ø12.58. V-carve keeps the coarse
  input on purpose (its medial axis grows one spoke per polygon vertex).

## v0.1.5 (build 6) — 2026-09-04
- G-code export: consecutive toolpaths that use the same tool (pocket /
  contour / cutout) are now pooled and ordered per site, so e.g. a hole's
  counterbore and through-hole are both finished before the spindle travels
  to the next hole, instead of running each toolpath's full tour separately.
  An earlier toolpath's job always runs before a later one that overlaps it,
  so document order is preserved wherever it matters (pocket before the
  cutout around it). One tool change and one spindle start per pool.

## v0.1.4 (build 5) — 2026-09-04
- Rename completed everywhere: binary/CMake target `phobicccp`, launcher
  `phobicccp.desktop`, icon `phobicccp`, MIME file `phobicccp-c2d.xml`,
  window title and Qt application name `phobiCCCP`, CMake option
  `PHOBICCCP_WITH_CLIPPER2`. Old `phobiccc` install files must be removed by
  hand on machines that had the previous version.

## v0.1.3 (build 4) — 2026-09-04
- Vector tool palette (Select/Circle/Rect/Polygon/Path/Text) moved from a
  vertical column on the left to a horizontal row above the canvas, ahead of
  the options bar.
- Canvas: the first fit-to-board now waits for the viewport to be laid out
  (resizeEvent) instead of fitting into the pre-layout placeholder size, which
  could leave a freshly opened file at 0% zoom.

## v0.1.2 (build 3) — 2026-09-04
- Project renamed to **phobiCCCP — Phobic Carbide Create Clone Project**
  (GitHub repo `phobicdotno/phobiCCCP`, was `phobicCC-linux`). Binary,
  launcher and CMake target stay `phobiccc`.

## v0.1.1 (build 2) — 2026-09-04
- Toolbar: the polygon "sides" spinner is now only shown while the Polygon
  tool (P) is active, instead of permanently. Canvas gained a `toolChanged`
  signal for this.

## v0.1.0 (build 1) — 2026-09-02
- `cmake --install` now installs desktop integration, not just the binary:
  app-menu launcher (`phobicCC`, Graphics/Engineering), SVG icon, and an
  `application/x-carbide-create` MIME type so `.c2d` files open on double-click.
- Project now carries a version (`project(... VERSION 0.1.0)`, `VERSION` file).
