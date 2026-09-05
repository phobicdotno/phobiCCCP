# Changelog

## Unreleased
- Opening a truncated or corrupt file now says so instead of showing an empty
  design: SQLite opens a zero-byte file as a valid empty database, so the
  loader checks that the container really holds a Carbide Create document.
  The background loader reports an unreadable container too.
- `tests/test_robustness.cpp` (CTest `robustness`): damaged documents (missing
  or emptied tables, null / corrupt / mis-sized payloads, five truncation
  points) and malformed vector imports (unterminated SVG, NaN and 1e400
  coordinates, a DXF claiming 999999999 vertices, a block that inserts
  itself) must fail cleanly or salvage what they can — never crash, hang or
  allocate on a bad file's word. Imported geometry is checked to be finite.

## v0.4.2 (build 17) — 2026-09-05
- Node edit: fixed a redraw that was accidentally made conditional while
  removing dead state in v0.4.1.
- Build is warning-clean again under -Wall -Wextra.

## v0.4.1 (build 16) — 2026-09-05
- Node edit tool: clicking a shape now selects it. The tool fell through into
  the shape-drawing branch, which swallowed the click before the canvas saw
  it, so nothing could be picked unless it had been selected with another tool
  first.
- Convert to path no longer deletes an element whose outline is empty (a text
  of spaces, or a font that renders nothing).
- Background images: saving never overwrites a background the loader could not
  decode, and a document that never had one no longer gains an empty
  background row and its parameters.
- DXF import: a spline carrying fit-point X codes with no matching Y codes is
  dropped instead of crashing the import.
- Simulation: editing the program while a simulation runs cancels it, so a
  result computed for the previous program is no longer shown as current.
- Tiled export: each tile keeps only the toolpath blocks it actually cuts, so
  a tile no longer changes tools and starts the spindle for work that happens
  in another tile; a tile with nothing to cut comes out empty.
- `tests/test_integration.cpp` (CTest `integration`): cross-feature checks that
  the per-module tests cannot catch — a relief built by the modeller reaches
  the 3D toolpaths through the real provider and the simulated cut follows it;
  the same document saved, closed and reloaded still machines the same relief;
  an imported SVG welded with a boolean pockets correctly.
- Verified that saving preserves Carbide Create's binary MODEL row, the
  document params and every item row (only the SVG previews and the encrypted
  g-code are blanked, which CC regenerates).

## v0.4.0 (build 15) — 2026-09-05
Carbide Create parity wave 2 — toolpath lifecycle, engraving, 3D modelling and 3D machining:
- 3D modelling (Carbide Create Pro's "Model" tab): new right-side "Model"
  tab after Simulation. Relief components are composited in list order into
  the document's HeightModel (src/heightmodel.h: z ≤ 0 from the stock top,
  peak at 0, baseZ = −relief height, untouched cells NoModel) and served to
  the 3D toolpaths through `heightModelFor(doc)` (`installModelProvider()`,
  lazy rebuild when components or vectors change).
- Components from vectors: the selected closed vectors become a relief with a
  Flat, Round (circular cross-section), Angle (slope in degrees, capped at the
  height), Smooth (S-curve) or Dome (parabolic) profile; height, base height,
  combine mode Add / Subtract / Merge (max) / Multiply (fade), enable, rename,
  reorder. Profiles are a function of the exact Euclidean distance to the
  outline (Felzenszwalb two-pass transform on the rasterised mask), so a round
  profile is right for any shape, text included.
- Image heightmaps: PNG/JPG → luma (white high, black low; invert, Gaussian
  blur, amplitude, base), placed like the background image (one pixel =
  1/96 in at natural size, or a width in mm), and textures: an image tiled at
  a chosen tile width over the selected vectors (or the whole model) with an
  add/subtract amplitude.
- STL import (binary and ASCII, auto-detected): triangles rasterised from
  above into a z-buffer at the model resolution (vertices stamped too, so tiny
  triangles never leave holes); dialog for file units (mm / inch), fit to a
  width, position and model height (0 = the mesh's own scaled height).
- Model tab: component table (on, name, kind, height, combine) with
  add/remove/reorder, per-kind property editor, resolution selector (Auto keeps
  the grid at ≤ 4 M cells and ≥ 0.1 mm; Fine/Normal/Coarse/Draft), shaded relief
  view (light from the upper left, floor-to-peak tint, wheel zoom, drag pan,
  cursor X/Y/Z readout with height above the floor), debounced automatic
  rebuild on a worker thread with progress and cancel, plus a manual Rebuild.
- Persistence: the components (parameters, referenced vector uuids) are stored
  in the .c2d as `sqlar/phobi_model3d.json`, embedded image/STL bytes as
  `sqlar/phobi_model3d/<component id>` (raw rows, like background.png); the
  grid is recomputed on load. Carbide Create's own binary `MODEL` item is left
  untouched (its sample layout is undocumented), so CC still opens the file.
- `--shot <file.c2d> out.png model` raises the Model tab and composites
  synchronously before the grab; `tests/test_model3d.cpp` (CTest `model3d`):
  distance transform, round/angle/flat profiles, subtract/merge/base height,
  image gradient + invert + blur, texture tiling, STL pyramid (ASCII + binary,
  inch units, fit to width), auto resolution, render, persistence round trip,
  provider registration.

## v0.3.1 (build 14) — 2026-09-05
Fixes from an independent code review of the machine-control work:
- Zero X/Y/Z/all sent a G10 line without coordinates (never zeroed). Fixed.
- V-carve: rounded slots, ellipses and round-ended strokes collapsed to a
  single plunge after spur pruning. The pruning now stops at the axis.
- BitSetter settings and the reference tool were overwritten with defaults on
  every start. Fixed.
- A tool-length offset could be injected into a running program (Clear
  reference / Unlock). It is now queued until the line is free, and it is
  re-applied after reset, Unlock and Home.
- A lost ack on the wake-up line (Arduino boards reset on open) blocked every
  stream and macro silently. The wake-up is no longer counted and a stuck
  ad-hoc command is dropped after 3 s of Idle.
- G-code after a tool change repeated no modal words, so the first moves
  inherited the probe's slow feed. Every word is re-emitted after `M0 ;T`.
- Port list kept the wrong device after a rescan; the port name (not its
  description) is now tracked and persisted.
- Disconnecting mid tool-change no longer leaves a prompt armed for the next
  connection; a lost key release or focus change now cancels hold-to-jog.
- A program ending in a tool marker no longer reports "finished" while parked.
- Toolpath lifecycle (Toolpaths panel): **New ▾** creates a Contour, Pocket,
  Drill, Texture, V-Carve, Keyhole, Cutout or Engrave toolpath for the vectors
  selected on the canvas (or empty, with a hint), using Carbide Create's own
  default parameter set for the type — the exact key names and value types CC
  853 writes (depths as 3-decimal strings in the file's sign convention, rates
  as numbers, `ofset_dir` spelt CC's way), named "Contour Toolpath N" etc.,
  in the file's first toolpath group. The tool is the document's last-used
  cutter of the right kind (flat / V-bit / engraver, `tool_pocket` included),
  else the embedded library's #201 / #301 / #501 with the material's feeds.
- **Duplicate**, **Delete**, **Rename** (double-click the name, F2 or the
  context menu), an **enabled** checkbox per row, and **Move up / down** —
  the list order is the machining order. All undoable (Ctrl+Z), via new
  document-level commands (`src/toolpathcommands.h`).
- `Document::insertToolpath / removeToolpath / moveToolpath / toolpathIndex`,
  `toolpathGroups()` / `defaultToolpathGroup()` (group rows are now read).
  `save()` rewrites the toolpath rows like the element rows — DELETE + INSERT
  in memory order — so deletions and reorders actually reach the .c2d (the
  old in-place UPDATE could neither delete nor reorder); `params.num_toolpaths`
  is kept equal to the row count, `items.name` mirrors the type as CC does.
- New toolpath type **Engrave** (`engrave_toolpath`): `mode` = `outline`
  (trace the vectors exactly, no offset), `fill` (parallel hatch of the closed
  regions at `line_spacing` mm and `angle`°, clipped to the region inset by
  the tool radius — for a V / engraving cutter the radius it has at the cut
  depth — then an outline pass on that inset boundary) or `both` (fill plus
  the exact trace); `crosshatch` adds a second hatch at angle + 90°;
  `end_depth` / `stepdown` as usual. The hatch is an analytic line/edge
  clip; runs on neighbouring lines are chained into one zigzag by walking the
  inset boundary between their ends, so the engraver stays down (a 10 × 10 mm
  square at 1 mm spacing is one continuous move, not 9 plunges). Exported to
  g-code, counted in the job stats, drawn in the on-canvas preview and the 3D
  views like every other type. **Carbide Create has no engraving type in its
  file format** — phobiCCCP's loader keeps `engrave_toolpath` rows (any type
  string round-trips), but CC itself will ignore or refuse a file containing
  one: delete the engrave toolpath before opening the file in CC.
- `src/toolpathfactory.h`: `makeToolpath(doc, type, ids)`,
  `duplicateToolpath()`, `toolpathKinds()`, `depthString()` (core library).
- tests/test_geometry.cpp: engrave outline follows a rectangle exactly; fill
  at 1 mm on a 10 × 10 square gives 9 hatch runs inside the inset region with
  fewer than 9 retracts; a minimal CC-schema container round-trips three
  factory toolpaths (names, group, defaults, engrave type) and keeps count,
  order and `num_toolpaths` after delete + move + save + reload.

- 3D roughing and finishing over the modeller's relief (Carbide Create Pro's
  "3D Rough Machining" / "3D Finish Machining"): `3d_rough_toolpath` clears
  the model level by level with the pocket ring machinery (stay-down links
  checked against the relief), `3d_finish_toolpath` rasters the
  tool-compensated surface (ball, flat and V-bit footprints) at a chosen
  angle, stepover (mm or %), zigzag/climb/conventional, optional cross pass,
  with an optional boundary vector and offset. 3D jobs are never pooled with
  the 2D ring jobs around them.
- `tests/test_cam3d.cpp` (CTest `cam3d`): verifies against the material
  simulation that roughing stops at the model plus stock-to-leave and that a
  3 mm ball finish reproduces a 20 mm hemisphere to within 0.043 mm without
  ever cutting below it.
## v0.3.0 (build 13) — 2026-09-05
Carbide Create parity wave 1 — six features merged from parallel branches:
- Material-removal simulation (Carbide Create's "3D Simulation"): new
  right-side "Simulation" tab after Preview. The stock is a heightmap (cell
  size chosen so the map stays ≤ 4 M cells, never finer than the selected
  fine/normal/coarse setting) and every cutting move stamps the tool's swept
  footprint — flat end mill disc, ball sphere, V-bit cone, bull-nose corner
  radius; arcs tessellated to ≤ 0.2 mm chords, ramps split to ≤ 0.05 mm drops,
  rapids ignored. Runs on a worker thread with progress and cancel.
- Simulation view: shaded top-down image (light from the upper left, uncut
  stock light, deeper floors darker, through-cuts in slate blue), wheel zoom,
  drag pan, live X/Y/Z readout under the cursor, through-cut / unknown-tool
  warnings in the status line. `SimPanel::image()` / `heightMap()` expose the
  result for the isometric preview.
- `toolGeometry(const Document&)` (gcodeexport.h): tool table keyed by tool
  number, built from the toolpaths' embedded `tool` / `tool_pocket` JSON
  (diameter, V-bit angle, corner radius, CC type code).
- `--shot <file.c2d> out.png simulation` raises the Simulation tab and runs the
  simulation synchronously before the grab.
- `tests/test_simulation.cpp` (CTest `simulation`): pocket floor depth ± 0.01,
  untouched stock outside, 90° V-bit surface width ≈ 4 mm at −2 mm, ball
  profile, arc tessellation, through-cut flag, cancellation, tool table,
  100k-segment throughput on a 2000×2000 map.
- Edit → Vectors: Carbide Create's Booleans — **Union (Weld)** (Ctrl+Shift+U),
  **Subtract** (Ctrl+Shift+D; first-selected minus the others — click order,
  or the largest bounding box when several were selected at once) and
  **Intersect** (Ctrl+Shift+I). Computed with Clipper2 on the vectors
  flattened at 0.005 mm; each element is filled even-odd (text counters
  survive), elements weld non-zero. Results replace the inputs as closed
  `path` elements in CC's exact JSON shape, one per ring (holes are separate
  nested paths), on the first input's layer.
- Edit → Vectors → **Offset…** (Ctrl+Shift+O): distance, inside/outside, keep
  original. Closed vectors offset as one even-odd region with round joins;
  open paths become the outline of a stroke of width 2·d.
- Edit → Vectors → **Align** (left/right/top/bottom/centers to the selection's
  bounding box, Ctrl+Shift+arrows / H / V), **Center on stock** (H, V, both;
  Ctrl+Shift+C) and **Distribute** (even center spacing, Ctrl+Alt+H / V).
  Union / Subtract / Intersect / Offset / Center also sit on a small icon bar.
- All vector operations are single undo steps; the results are selected
  afterwards. Toolpaths that referenced consumed inputs are re-pointed at the
  result elements (and restored on undo), so `elements[].uuid` never dangles.
- `tests/test_vectorops.cpp` (CTest `vectorops`): union/intersect/subtract
  areas, hole rings, offset bbox growth, stroke outlines, alignment maths.
- Canvas no longer fires `selectionChanged` into a half-destroyed view when
  closed with a selection active (crash at exit).
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
- Tool library / speeds and feeds: `data/tool-library.json` (embedded as a Qt
  resource) carries the Carbide 3D cutter catalogue (#101/#102/#111/#112/#122/
  #201/#202/#251/#278/#301/#302/#321/#322/#501/#502/#602) with per-material
  rpm / feed / plunge / depth-per-pass / stepover for MDF-plywood, soft wood,
  hard wood, acrylic, HDPE and aluminium; unverified catalogue numbers and
  non-CC-default (conservative) feeds are flagged in the data and the dialog.
  `ToolLibrary` loads it; the Toolpaths panel's new "Tool library…" button
  (searchable list, material selector, preview of the numbers) writes the
  chosen tool into the selected toolpath's `tool` / `speeds` objects and its
  `stepdown` / `depth_per_pass` / `stepover` using CC's own key names, as an
  embedded user tool, so the file still opens in Carbide Create.
- Pocket rest machining: `enable_rest` with a `rest_diameter` larger than the
  tool ring-fills only the corners and channels the previous cutter could not
  reach (tool-centre region inset by the tool, minus the centres whose whole
  disc lies in the opening the big cutter already cleared), with one tool
  radius of overlap into the cleared floor. Geometry test: 40×20 pocket, Ø6.35
  after Ø12.7 → four corner rings, a fraction of the full pocket's cut length.
- Toolpath tiling: File → Export G-code (tiled)… and `--export-tiled in.c2d
  outbase [tile_height]` write `<outbase>_tile1.nc`, `_tile2.nc`, … — tile k
  holds the motion with Y in [k·h, (k+1)·h) shifted by −k·h, moves crossing a
  boundary are split there (arcs tessellated first), each cut is made exactly
  once and every tile is a complete program (header, tool changes, spindle,
  retract-and-re-enter around every split, M02). The plain export offers
  tiling when the document has `tiling_enabled` and the job exceeds
  `tile_height`. New `test_tiling` (CTest `tiling`) checks a synthetic 3-tile
  job and a document export: every tile's Y ⊆ [0, h], nothing lost or doubled.
- Background images (Carbide Create compatible): File → Background image →
  Set… / Clear / Adjust… places a picture under the grid at millimetre scale
  (X/Y, width with aspect kept, rotation, opacity, lock). Stored the way CC
  stores it — `params.background_*` plus `sqlar/background.png` — so the same
  file shows the picture in Carbide Create.
- Trace image (File → Trace image…, Ctrl+Shift+T): threshold/invert, blur,
  despeckle (min area mm²), target width, Douglas–Peucker simplify and
  corner-preserving Bézier smoothing with a live preview; inserts closed
  path elements (outer outlines and holes separately) as one undoable step.
  Pure Qt contour tracing; a 2000×2000 px picture traces in well under a
  second.
- New `trace` CTest (tests/test_trace.cpp): circle/ring contour areas and
  bounds, simplification, smoothing schema, background round-trip.
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
