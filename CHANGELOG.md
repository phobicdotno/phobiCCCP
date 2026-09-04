# Changelog

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
