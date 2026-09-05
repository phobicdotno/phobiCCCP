# Changelog

## Unreleased
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
