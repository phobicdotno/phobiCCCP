# Changelog

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
