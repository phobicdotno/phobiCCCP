# phobiCCCP — Phobic Carbide Create Clone Project

A Linux/Qt6 CAD/CAM application for Carbide Create `.c2d` files and GRBL
machines (Shapeoko) — a Carbide-Create-and-Carbide-Motion-free workflow on
Linux, built on the reverse-engineered format documentation in
[shapeoko-c2d](https://github.com/phobicdotno/shapeoko-c2d).

## What it does

**Files**
- Opens modern (v7/v8, SQLite container) `.c2d` files natively
- Saves them back so Carbide Create 8 still opens them (round-trip verified);
  toolpath edits and newly created toolpaths are written into the container
- All 5 element types render: circle, rectangle, regular polygon, path, text

**Vector editing** (Carbide Create parity)
- **Booleans**: Union (weld), Subtract, Intersect (Edit → Vectors, icon bar);
  **Offset** inside/outside by distance; **Align** left/right/top/bottom/
  centres, **Center on stock**, **Distribute** — all single undo steps, and
  toolpaths that referenced the inputs follow the results
- **Bezier splines**: pen-style Path tool (click = corner, drag = smooth
  handles) and a **Node edit** tool (N): drag anchors/handles, insert
  (double-click), delete, corner/smooth/symmetric, convert shapes and text to
  paths
- **Text on arc** (radius, angle offset, top/bottom) plus font family, bold,
  italic in the Properties panel
- **Import SVG and DXF** (File menu or drag-and-drop): paths with beziers,
  transforms, units; DXF LINE/LWPOLYLINE/ARC/CIRCLE/ELLIPSE/SPLINE/INSERT
- **Background image** under the grid (position, width, rotation, opacity,
  lock; stored in the .c2d the way CC does) and **Trace image** (threshold,
  blur, despeckle, simplify, smoothing) to closed vector paths
- Original tools still there:
- Tools: Select (V), Circle (C), Rectangle (R), Polygon (P), Path/polyline (L),
  Text (T) — with live preview, grid + snap (G), zoom (wheel / F to fit),
  middle-mouse pan
- Select / rubber-band, drag to move, drag the amber handle to resize,
  Del to delete, full undo/redo (Ctrl+Z / Ctrl+Shift+Z)
- Properties panel: numeric center / radius / W×H / sides / rotation
- New elements are written in CC's exact JSON schema (key-verified against
  CC-853 specimens), so toolpaths and CC itself accept them

**CAM — every Carbide Create toolpath type, plus engraving and 3D**
- **Create, duplicate, rename, enable, reorder and delete toolpaths** (the
  Toolpaths panel's New menu covers all seven CC types plus Engrave)
- **Engrave**: outline, hatch fill (spacing, angle, crosshatch) or both
- **3D modelling** (Model tab): components from vectors (flat / round / angle
  / dome / smooth), images as heightmaps, tiled textures, STL import; the
  relief is stored in the .c2d and drives the 3D toolpaths
- **3D roughing and finishing**: Z-level clearing and raster finishing on the
  tool-compensated surface (ball, flat, V), boundary vectors, stepover in mm
  or %, raster angle, zigzag/climb/conventional, optional cross pass
- **Tool library** with Carbide 3D's cutters and per-material speeds & feeds
  (Toolpaths → Tool library…; values that are guesses are flagged as such)
- **Rest machining** for pockets (`enable_rest` / `rest_diameter`)
- **Toolpath tiling** (File → Export G-code (tiled)… or `--export-tiled`):
  one complete program per tile, Y-shifted, cuts split cleanly at the
  boundary
- **3D Simulation** tab: material-removal heightmap of the whole program
  (end mill / ball / V-bit / bull-nose), shaded top-down view with depth
  readout and through-cut warning
- Toolpaths panel: edit any toolpath parameter (depths, feeds, stepover,
  tool…) and assign the canvas selection as a toolpath's vectors; edits are
  saved back into the `.c2d` in place
- **G-code export** (File → Export G-code, Ctrl+G, or
  `phobicccp --export in.c2d out.nc`) in Carbide Create's own GRBL dialect:
  - pocket: connected-component inset rings, innermost-first, stay-down
    linking, alternating pass direction (no track-back), plunge-once-per-depth
  - contour: no-offset / inside / outside; cutout: `cut_depth +
    break_through`, auto-tabs, ramp entries
  - drilling: peck cycles; keyhole: plunge + slide + return; texture:
    deterministic random hatch strokes
  - **advanced v-carve: true medial-axis carving** (segment Voronoi) — the
    V-bit rides the skeleton with continuously varying depth, runs out into
    sharp corners, and clears flat bottoms where it maxes out
  - **v-carve inlay**: one click generates the mirrored male (inverse region
    in a border, depths shifted by the glue gap) from the female toolpath
  - exact offsetting via Clipper2; circular rings become true G2/G3 arcs and
    all other curves are greedily arc/line-fitted (0.01 mm tolerance), so the
    GRBL planner never starves on 1 mm segment chatter
  - jobs ordered nearest-neighbor to minimize rapids; consecutive same-tool
    toolpaths are interleaved per site (counterbore + through-hole at one
    hole, then the next), keeping document order wherever jobs overlap
- **On-canvas toolpath preview** (Ctrl+P): rapids dashed, cuts colored by
  depth, plunge markers
- Export/run shows job stats: extents, cut/rapid length, time estimate

**Machine control (Machine dock)**
- Connects to a GRBL controller over serial (115200) — hardware-verified
  against a Shapeoko 5 Pro (GRBL 1.1h). Ports are listed with their USB
  description; a device path can also be typed by hand.
- Live status: state, **work** and **machine** position side by side
  (WCO tracked from the status reports), feed / spindle, tool-length offset,
  probe-pin indicator.
- **Jog**: step sizes 0.1 / 1 / 10 / 100 mm and speeds slow / medium / fast /
  rapid. Click = one step, **press and hold = continuous** jog cancelled on
  release (`$J=` + jog-cancel 0x85). Arrow keys jog X/Y and PgUp/PgDn jog Z
  the same way while the panel has focus.
- **Work zero**: Zero X / Y / Z / XY / all (`G10 L20`), "→ X0 Y0" (raise to
  safe Z, then rapid to work zero), Home, Unlock.
- **BitSetter** (tool-length probe): configure the button's *machine*
  coordinates ("use current" while parked over it), safe Z, fast/slow probe
  feeds and a tool-change spot. *Measure tool* goes there, probes twice
  (`G38.2`, fast then slow), and comes back. The first measurement after
  *Zero Z* is the **reference tool**; every later measurement applies the
  length difference as a `G43.1` offset, so a new tool cuts to the same Z0
  without re-zeroing. The offset survives resets and unlocks (re-applied
  automatically) and the reference is remembered between sessions.
- **Streaming with tool changes**: every `M0 ;T<n>` the post emits parks
  the machine (spindle off, safe Z, tool-change spot), prompts for the tool,
  measures it on the BitSetter, applies the offset and continues — no manual
  Resume needed. Character-counting protocol, feed-hold / resume / soft-reset,
  realtime feed override, progress bar and console.
- **Air-cut mode**: rehearse any program with all spindle commands stripped
  and every Z lifted by a chosen amount; every run shows a stats +
  spindle-warning confirmation first.
- Verification status: connection, jogging, zeroing and streaming are
  hardware-verified on a Shapeoko 5 Pro; the BitSetter measurement and the
  automatic tool-change flow have so far been verified against
  `tools/grblsim.py` only (`tools/flowtest.sh`). Do an air-cut run with a real
  tool change before trusting them on a job.

**Isometric preview** (Preview tab): the generated route drawn in 3D over the
stock — rapids dashed, cuts coloured by depth — with orbit / zoom, and an
animated tool marker with play / pause / speed and a position slider. The
machine's live work position is drawn on it while connected.

## Building (Ubuntu / Debian)

```sh
sudo apt install build-essential cmake qt6-base-dev qt6-base-dev-tools \
     libqt6sql6-sqlite qt6-serialport-dev zlib1g-dev libgl1-mesa-dev \
     libboost-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/phobicccp yourfile.c2d
```

Install (binary + start-menu launcher + icon + `.c2d` file association):

```sh
sudo cmake --install build                                  # system-wide, /usr/local
cmake --install build --prefix ~/.local                     # or per-user, no root
```

After that **phobiCCCP** shows up in the application menu (search "phobic")
and `.c2d` files open with it on double-click. For the Machine dock you also
need serial access: `sudo usermod -aG dialout $USER`, then log out and in.

Clipper2 is fetched at configure time (disable with
`-DPHOBICCCP_WITH_CLIPPER2=OFF`). `libboost-dev` provides the header-only
Boost.Polygon Voronoi builder for the medial-axis v-carve; without it the
build falls back to depth-graded ring approximation.

## CLI

```sh
phobicccp file.c2d                      # open the GUI on a file
phobicccp --export file.c2d out.nc      # headless g-code export
phobicccp --selftest in.c2d out.c2d     # 10-stage create/save/export self-check
phobicccp --shot file.c2d out.png [preview]  # render the GUI to a PNG
phobicccp --grbl-check /dev/ttyACM0     # safe GRBL handshake (no motion)
phobicccp --grbl-probe /dev/ttyACM0 -20 -20 [safeZ]   # BitSetter measurement (machine XY)
phobicccp --grbl-run /dev/ttyACM0 job.nc [bsX bsY]    # stream with automatic tool changes
python3 tools/grblsim.py                 # GRBL 1.1h simulator on a pty for testing
```

## Format notes

The `.c2d` container is SQLite: `params` (key/value), `items` (zlib-compressed
J1 JSON payloads for layers, elements, toolpaths, models), `sqlar` (previews,
encrypted g-code — regenerated by CC, safely blanked on save). Carbide Create
is itself a Qt6 QtWidgets app, which is why the format is full of Qt idioms
(QFont descriptors, row-major QTransform arrays). Full documentation,
specimens and tools live in the
[shapeoko-c2d](https://github.com/phobicdotno/shapeoko-c2d) repo.

Safety: this is an independent implementation — offsets, depths and orderings
are close to CC's but not identical. Preview the `.nc` (e.g. ncviewer.com)
and use the Machine dock's air-cut mode before trusting a new program.
