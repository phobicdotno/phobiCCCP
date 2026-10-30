# toollib: the tool library

phobiCCCP's bits live here, one JSON file per bit, in a folder per kind:

    toollib/<kind>/<6 hex id>.json     one tool: id, number, name, diameter, feeds per material, ...
    toollib/<kind>/kind.png            the side-view sketch of that kind
    toollib/materials.json             the materials the feeds are listed for
    toollib/index.json                 sha1 of every file (made by make_index.py)

The id is six hex digits, unique, and equal to the file name. `number` is the
tool number saved in .c2d files and G-code; keep it unique too.

**Add a bit:** copy a file in the right kind folder, give it a new id (file name
and `"id"` inside), a new `number`, edit the numbers, then run

    python3 toollib/make_index.py

and commit. The script refuses duplicate ids or a file whose `id` differs from
its name. The tests fail if index.json or data/tools.qrc is out of date.

**Add a kind:** make a new folder, add tools in it, and drop a `kind.png`
sketch (400 x 600, dark background) next to them. The app draws kinds it knows
(endmill, ball, vbit, engraver, roundover) itself; for any other kind it shows
that picture. `phobicccp --export-tool-sketches <dir>` writes the five built-in
sketches.

**How the app gets it:** the folder is built into every release, and at every
start the app fetches `index.json` from GitHub and downloads only the files
whose sha1 differs, into `~/.local/share/phobicdotno/phobiCCCP/toollib/`. Those
files win over the built-in ones. Offline changes nothing. Turn it off with the
setting `tools/syncAtStart=false`.

The bit stock counts and the Collections are not here: they are synced between
copies of the app on the same network (see src/stocksync.h).
