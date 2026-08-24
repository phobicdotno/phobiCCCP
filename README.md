# phobicCC-linux

A Linux/Qt6 viewer, editor and generator for Carbide Create `.c2d` files, built
on the reverse-engineered format documented in
[phobicdotno/shapeoko-c2d](https://github.com/phobicdotno/shapeoko-c2d).

The goal is a Carbide-Create-free CAD/CAM path on Linux: read and write the
native `.c2d` container, and (later) drive a Shapeoko directly over GRBL - no
Carbide Motion, no Windows/Mac.

## Carbide Create is itself a Qt (6) app

The macOS bundle ships `QtCore`, `QtGui`, `QtWidgets`, `QtSvg`, `QtOpenGL` +
`QtOpenGLWidgets` (Qt6-only), `QtConcurrent`, `QtNetwork`, `QtPrintSupport`,
`QtDBus`. That's not incidental - it's why the `.c2d` format is full of Qt
serializations, and it makes a faithful Linux port largely a matter of
*deserializing Qt objects back into the same classes*:

| In the file | Is really | We rebuild with |
|---|---|---|
| element `points`/`cp1`/`cp2` | a bezier path | `QPainterPath` |
| text `qtfont` | `QFont::toString()` | `QFont::fromString()` |
| text `transform` (3×3) | a `QTransform` | `QTransform` |
| `sqlar/preview.svg` | `QSvgGenerator` output | `QtSvg` |
| background CAM | `QtConcurrent` jobs | (Tier 2) |
| 3D model view (`model_2`) | `QtOpenGLWidgets` | (Tier 3) |

No `QtSql` is bundled, so CC links SQLite directly (matching the `sqlar` usage);
this port may use `QtSql` or the C API interchangeably. The practical takeaway:
geometry - and even those SVG previews - can be regenerated with byte-level
fidelity using the identical Qt calls CC uses.

## Status - Tier 1 (complete)

- [x] Open the modern (v7/v8 SQLite) container: `params` + `items`
- [x] Decompress element payloads (plain zlib - **not** Qt `qCompress`; see note)
- [x] Parse all five element types into `QPainterPath` (circle, rectangle,
      regular_polygon, path, text - text via its pre-flattened `rendered` outlines)
- [x] Render board + geometry on a pan/zoom canvas (Y-up, mm)
- [x] List params, element histogram and toolpaths in a side panel
- [x] **Write/save `.c2d`** - clone-and-rewrite recipe, round-trip verified
      (36/36 elements byte-identical, SQLite integrity ok); `src/c2ddocument.cpp`
- [x] **GRBL post-processor** emitting plaintext `.nc` in CC's own dialect
      (`src/post_grbl.{h,cpp}`) - the Carbide-Motion-free machine path
- [x] Geometry editing - drag to move (rubber-band or click select,
      middle-button pan), Edit menu scale/add circle/add rectangle/delete;
      every edit mutates `Element::raw` so save round-trips it. Headless
      checks in `tests/test_geometry.cpp` (`-DPHOBICCC_TESTS=ON`)
- [x] Toolpath-parameter editing - Toolpaths dock lists every toolpath with
      editable parameters (nested `tool`/`speeds` included); values keep
      their original JSON type, so depth strings stay strings. Saved via
      the same delete-and-reinsert recipe as elements
- [x] Editing infrastructure - undo/redo (Ctrl+Z / Ctrl+Shift+Z, 50 steps),
      unsaved-changes prompt on close/open; deleting an element strips its
      uuid from every toolpath's reference list (CC requires references to
      resolve)
- [x] Multi-screen UI - canvas, document info and toolpaths are all dock
      widgets: drag any of them to another screen, resize freely, maximize
      (floating docks get native window frames), View menu show/hide +
      Re-dock All + F11 fullscreen; window and dock layout persist across
      sessions
## Status - Tier 2 (in progress)

- [x] CAM core (`src/cam.{h,cpp}`, Clipper2 2.0.1 vendored in `third_party/`):
  contour (inside/outside/on-line via `ofset_dir`, plus `cutout` with
  `flip_inside_outside` and `cut_depth`/`depth_per_pass`), pocket (inward
  offset rings by `stepover`, cut inside-out), drilling (pecks by `stepdown`
  at circle centers). Depth passes follow the build 853 positive-down
  convention; `stock_to_leave` honoured; retract at a fixed 3 mm
- [x] File > Export G-code (Ctrl+G): all enabled supported toolpaths through
  the GRBL post to a plaintext `.nc`
- [x] Machine > Send to GRBL: USB serial at 115200, one line in flight,
  ok/error tracking, progress, abort via soft reset (0x18). Protocol logic is
  the headless-testable `GrblSender`
- [ ] Holding tabs (a contour/cutout at full depth frees the part; the export
  warns loudly). Ramped entry, climb-direction control and rest machining are
  also ignored for now
- [ ] Live testing against a real Shapeoko (everything above is verified
  headlessly: 56 checks incl. CAM geometry bounds and sender sequencing)
| 3D model view (`model_2`) | `QtOpenGLWidgets` | (Tier 3) |

No `QtSql` is bundled, so CC links SQLite directly (matching the `sqlar` usage);
this port may use `QtSql` or the C API interchangeably. The practical takeaway:
geometry - and even those SVG previews - can be regenerated with byte-level
fidelity using the identical Qt calls CC uses.

## Status - Tier 1 (complete)

- [x] Open the modern (v7/v8 SQLite) container: `params` + `items`
- [x] Decompress element payloads (plain zlib - **not** Qt `qCompress`; see note)
- [x] Parse all five element types into `QPainterPath` (circle, rectangle,
      regular_polygon, path, text - text via its pre-flattened `rendered` outlines)
- [x] Render board + geometry on a pan/zoom canvas (Y-up, mm)
- [x] List params, element histogram and toolpaths in a side panel
- [x] **Write/save `.c2d`** - clone-and-rewrite recipe, round-trip verified
      (36/36 elements byte-identical, SQLite integrity ok); `src/c2ddocument.cpp`
- [x] **GRBL post-processor** emitting plaintext `.nc` in CC's own dialect
      (`src/post_grbl.{h,cpp}`) - the Carbide-Motion-free machine path
- [x] Geometry editing - drag to move (rubber-band or click select,
      middle-button pan), Edit menu scale/add circle/add rectangle/delete;
      every edit mutates `Element::raw` so save round-trips it. Headless
      checks in `tests/test_geometry.cpp` (`-DPHOBICCC_TESTS=ON`)
- [x] Toolpath-parameter editing - Toolpaths dock lists every toolpath with
      editable parameters (nested `tool`/`speeds` included); values keep
      their original JSON type, so depth strings stay strings. Saved via
      the same delete-and-reinsert recipe as elements
- [x] Editing infrastructure - undo/redo (Ctrl+Z / Ctrl+Shift+Z, 50 steps),
      unsaved-changes prompt on close/open; deleting an element strips its
      uuid from every toolpath's reference list (CC requires references to
      resolve)
- [x] Multi-screen UI - canvas, document info and toolpaths are all dock
      widgets: drag any of them to another screen, resize freely, maximize
      (floating docks get native window frames), View menu show/hide +
      Re-dock All + F11 fullscreen; window and dock layout persist across
      sessions
- [ ] CAM: compute cutter paths → feed the GRBL post (Tier 2)

## Roadmap

| Tier | Scope | Blockers |
|---|---|---|
| **1** | Viewer + geometry editor + toolpath-parameter editor. Files open in Carbide Create for CAM. | none - format is documented |
| **2** | Own G-code for pocket / contour / drilling (skip V-carve/texture). Stream to GRBL over serial. | pocket/contour offsetting via [Clipper2] |
| **3** | Full Carbide Create parity | V-carve needs a medial-axis engine; 3D modelling (`model_2` heightmap) |

## Why not just use Carbide Motion?

Carbide Motion is a machine controller, not a CAM program. The `.c2d` stores an
**encrypted** `gcode.egc` cache, but that's not what reaches the machine - when
CC cuts or exports it writes **plaintext `.nc`** (the binary has
`sendToCarbideMotion`, `tmp_gcode_%1.nc`, filter `*.nc *.txt *.tap`). So the
encryption only protects the toolpath copy *at rest inside the design file*; the
program on the wire is plain g-code. See the dialect + evidence in
[shapeoko-c2d/docs/GCODE-AND-CRYPTO.md](https://github.com/phobicdotno/shapeoko-c2d/blob/main/docs/GCODE-AND-CRYPTO.md).

That means we skip CM entirely: emit standard `.nc` (`src/post_grbl.cpp`
implements CC's own GRBL dialect) and stream it to the GRBL-family controller over
USB serial (`/dev/ttyACM*` on Linux, `/dev/cu.usbmodem*` on macOS) - or just hand
the `.nc` to Carbide Motion. No `.egc`, no decryption, ever.

Carbide Motion itself only runs on macOS/Windows, so if you want CM's CAM output
as a reference, capture it there. On macOS the cleanest tap is `dtrace` on CM's
serial writes - it prints the plaintext g-code without a kext or hardware:

```sh
sudo dtrace -q -n 'syscall::write:entry
  /execname == "Carbide Motion"/ { printf("%s", copyinstr(arg1, arg2)); }'
```

(Pair with a `read:entry` probe for the controller's `ok`/status replies. May
require loosening SIP for dtrace.) A `socat` PTY shim, Wireshark USB capture, or
a hardware serial tap are alternatives. Since the controller is GRBL, one capture
confirms the dialect - after that, talk to it directly and drop CM entirely.

## The one Qt gotcha

`items.data` and compressed `sqlar` blobs are **raw zlib** streams
(`0x78 0x01`). Qt's `qUncompress()` expects a 4-byte big-endian size prefix and
will fail on them - call zlib directly (see `src/zlibutil.cpp`). The `sz` column
is the uncompressed length, handy for sizing the inflate buffer.

## Build

Requires Qt 6 (Widgets + Sql) and zlib.

```sh
sudo apt install qt6-base-dev zlib1g-dev cmake g++   # Debian/Ubuntu
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/phobiccc path/to/file.c2d
```

## Layout

```
src/
  zlibutil.{h,cpp}    raw-zlib inflate/deflate (the qUncompress workaround)
  element.{h,cpp}     J1 element JSON  ->  QPainterPath (point model + text)
  c2ddocument.{h,cpp} open container, read params/items, decode payloads
  canvas.{h,cpp}      QGraphicsView board + geometry renderer (Y-up)
  post_grbl.{h,cpp}   CAM ops -> plaintext .nc g-code (CC's GRBL dialect)
  mainwindow.{h,cpp}  window, file open/save-as, info sidebar
  main.cpp
```

## License

TBD.

[Clipper2]: https://github.com/AngusJohnson/Clipper2
