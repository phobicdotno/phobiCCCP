# Changelog

## Unreleased
- File → Import SVG… / Import DXF… (Carbide Create's "Import DXF and SVG"),
  plus drag-and-drop of .svg/.dxf (imported) and .c2d (opened) onto the main
  window. Imports are one undo step. Geometry lands in mm, Y-up; if it does
  not already fit inside the stock its bounding box is moved so the bottom-left
  corner sits at (10, 10) mm. Curves stay cubic beziers in the CC path schema
  (`point_type` 3 + `cp1`/`cp2`); the result is written in the same row model
  as `Element::makePath`, so the saved file still opens in Carbide Create.
- SVG reader (`src/importers.cpp`, QXmlStreamReader, no QtSvg): path with the
  full `d` grammar (M/L/H/V/C/S/Q/T/A/Z, absolute and relative, arcs converted
  to beziers), rect (rx/ry), circle, ellipse, line, polyline, polygon, nested
  `g` transforms (translate/scale/rotate/skewX/skewY/matrix), viewBox with
  width/height in px (96 dpi), mm, cm, in, pt, pc and preserveAspectRatio;
  `display:none` and `defs`/`symbol`/`clipPath`/`mask` content is not drawn.
  text, image, use and gradients are ignored and counted in the summary.
- DXF reader (ASCII R12–2018 group codes): LINE, LWPOLYLINE and
  POLYLINE/VERTEX/SEQEND with bulge arcs, CIRCLE, ARC, ELLIPSE, SPLINE
  (NURBS evaluated by de Boor and flattened to 0.02 mm; fit-point-only
  splines interpolated with Catmull-Rom cubics), INSERT/BLOCK expansion with
  insertion point, scale, rotation and column/row arrays, mirrored extrusion
  (230 = -1), `$INSUNITS` scaling (unitless = mm). POINT is ignored silently;
  TEXT/MTEXT/HATCH/DIMENSION and 3D polylines are skipped and reported.
- New CTest target `test_import` (`tests/test_import.cpp`) with hand-written
  fixtures in `tests/data/` asserting element counts, bounding boxes in mm,
  the CC row schema, unit scaling and the placement rule (333 checks).
  `test_import <in.c2d> <out.c2d>` also writes a document with every fixture
  imported for a `--shot` visual check.

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
