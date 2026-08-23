# phobicCC-linux

A Linux/Qt6 viewer, editor and generator for Carbide Create `.c2d` files, built
on the reverse-engineered format documented in
[phobicdotno/shapeoko-c2d](https://github.com/phobicdotno/shapeoko-c2d).

The goal is a Carbide-Create-free CAD/CAM path on Linux: read and write the
native `.c2d` container, and (later) drive a Shapeoko directly over GRBL — no
Carbide Motion, no Windows/Mac.

## Status — Tier 1 (in progress)

- [x] Open the modern (v7/v8 SQLite) container: `params` + `items`
- [x] Decompress element payloads (plain zlib — **not** Qt `qCompress`; see note)
- [x] Parse all five element types into `QPainterPath` (circle, rectangle,
      regular_polygon, path, text — text via its pre-flattened `rendered` outlines)
- [x] Render board + geometry on a pan/zoom canvas (Y-up, mm)
- [x] List params, element histogram and toolpaths in a side panel
- [ ] Geometry editing (move/scale/create)
- [ ] Toolpath-parameter editing (all 7 types are captured as JSON already)
- [ ] Write/save `.c2d` (INSERT elements, UPDATE params — recipe is proven)

## Roadmap

| Tier | Scope | Blockers |
|---|---|---|
| **1** | Viewer + geometry editor + toolpath-parameter editor. Files open in Carbide Create for CAM. | none — format is documented |
| **2** | Own G-code for pocket / contour / drilling (skip V-carve/texture). Stream to GRBL over serial. | pocket/contour offsetting via [Clipper2] |
| **3** | Full Carbide Create parity | V-carve needs a medial-axis engine; 3D modelling (`model_2` heightmap) |

## Why not just use Carbide Motion?

Carbide Motion is a machine controller, not a CAM program — it streams the
**pre-computed, encrypted** `gcode.egc` embedded in the `.c2d`. Since only CC can
produce that `.egc`, a Linux-generated file will *open* in CC but won't *run* in
CM. The answer isn't to break the encryption — it's to skip CM: the Shapeoko
controller is a GRBL-family board speaking plain g-code over USB serial
(`/dev/ttyACM*`), which any sender can drive. The encryption protects the file at
rest, not the wire; observing a CM session (`usbmon`/Wireshark, or a pty shim)
shows the plaintext g-code if you want CM's CAM output as a reference.

## The one Qt gotcha

`items.data` and compressed `sqlar` blobs are **raw zlib** streams
(`0x78 0x01`). Qt's `qUncompress()` expects a 4-byte big-endian size prefix and
will fail on them — call zlib directly (see `src/zlibutil.cpp`). The `sz` column
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
  mainwindow.{h,cpp}  window, file open, info sidebar
  main.cpp
```

## License

TBD.

[Clipper2]: https://github.com/AngusJohnson/Clipper2
