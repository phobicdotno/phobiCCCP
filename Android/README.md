# PhobiCCCP for pads

Design on an Android tablet or an iPad and cut on any phobiCCCP on the same network. The
app is written in Flutter, so the same code builds for both. It looks like the desktop
program: the same menu bar, 1 · Design / 2 · Toolpaths / 3 · Machine switch, tool bar,
drawing with its grid, Properties / Document / Toolpaths panels, colours and Inter font,
laid out for a pad in landscape.

- **Syncs by itself, both ways.** While the app is open it looks for phobiCCCP every few
  seconds and keeps designs in step with every copy it finds. A design made or changed on
  one pad reaches every computer, and through them every other pad; a copy made from
  another replaces it, and a deletion travels the same way. A design changed in two places
  apart (on a computer and a pad, or on two pads that went through different computers)
  is not overwritten: both versions are kept, the later edit under its name and the other
  as a new design, “<name> (conflict)”, the same on every pad and computer, and the status
  line says so (a deletion made apart from an edit loses to a later edit; an earlier edit
  it would drop is kept as the new design). The tool library and materials come from
  phobiCCCP, each computer's own; Send warns when a toolpath's tool is not in the library
  of the computer it goes to.
- **Edits on the desktop come back.** A design sent from a pad opens on the computer as an
  ordinary `.c2d`. Saving it there sends the change back to the pads. A later pad edit
  rebuilds that `.c2d` (and reloads it, when it is open with nothing unsaved), as long as
  the file holds only what the pad can: once something desktop-only is added (V-carves,
  curves, tabs, feeds set by hand, a 3D model), or the file is changed by another program
  (Carbide Create), the pads still get the change, but a pad edit no longer overwrites the
  file: it goes to a copy beside it, `<name> (pad).c2d`, and the status bar says so. The pad
  says this too when such a design is opened or edited there.
- **Several machines on one phobiCCCP.** The Machine page lists each computer's machines
  (desktop: Machine › Add machine…) with their state; tap one to make it the one in use.
  Switching drops the machine's connection and homing, so while one is connected a pad
  may only switch when **Machine › Pads may start cutting** is on there.
- **Several phobiCCCPs.** Install phobiCCCP on as many computers as you like; Send offers
  every one that is open, and every machine on it.
- **Send and start.** Tick *Start cutting right away* when sending. The pad asks once more,
  and the computer only starts when **Machine › Pads may start cutting** is on there (off by
  default) and the machine is connected, homed, idle and not busy; otherwise the design is
  opened and the pad is told why it did not start.

Designs are 2.5D, cut from the top: rectangles, circles, polygons, text, paths and drill
points, with contour (outside / inside / on), pocket, drill and engrave toolpaths, each of
which can be ticked off as on the desktop. The Area tool picks just the area tapped, like a fill:
where shapes overlap, the tapped piece becomes a closed path, so a pocket cuts only that piece.

The fonts in `fonts/` (Inter, Liberation Sans for text on the drawing, DejaVu Sans Mono)
are the ones the desktop uses; their licences are in `fonts/LICENSE.txt`.

## Install

**Android:** on the tablet, open the repository's **Releases** page, download the newest
`PhobiCCCP-x.y.z.apk` and open it (allow installing from that app when asked). A release
is made by pushing a version tag (`git tag v1.2.3 && git push origin v1.2.3`), or by running
**release-apk** from the **Actions** tab, which releases the version in `VERSION`.
Every CI run also builds a debug APK (**PhobiCCCP-android** under the run in **Actions**).

**iPad:** iOS builds need a Mac with Xcode and an Apple developer account:

```sh
cd Android
flutter build ipa              # or open ios/Runner.xcworkspace in Xcode and Run on the iPad
```

The first time the app looks for phobiCCCP, iPadOS asks to allow devices on the local
network; say yes (Settings › Privacy › Local Network if it was refused).

**Building it yourself** needs Flutter 3.44 (and the Android SDK for Android):

```sh
cd Android
flutter pub get
flutter analyze
flutter test                   # the unit tests
flutter build apk --debug      # build/app/outputs/flutter-apk/app-debug.apk
flutter run                    # on a connected tablet or emulator
```

## Finding phobiCCCP

On the desktop, **File › Receive from Syntexophobic and pads** must be on (it is by
default). The pad and computer must be on the same network, and port 47652 (TCP and UDP)
open in the computer's firewall. The app broadcasts and also asks every address on its
own /24 network, so it finds phobiCCCP on iPad too, where broadcasts need a special Apple
entitlement. Over Tailscale or across subnets, add the computer by name or address on the
Machine page (or Machine › Add computer…).

## The link

Port 47652, shared with Syntexophobic's sender (`src/projectreceiver.h`):

- **Discovery**: UDP datagram `PHOBICCCP-DISCOVER 1`; the answer is JSON
  `{"app": "phobiCCCP", "version", "host", "port", "id", "api": 1, "machine"}`.
- **Requests**: TCP, `PHOBICCCP-API 1\n`, then one JSON object per line, each answered by
  one JSON line `{"ok": true, ...}` or `{"ok": false, "error": "..."}`:

| op | sends | answer |
|---|---|---|
| `info` | | host, version, id, `machines` [{id, name, active, size}], `activeMachine`, `status` {connected, state, homed, busy, streaming, job}, `allowStart`, `document` |
| `tools` | | `materials` [{id, name}], `tools` [{number, name, kind, diameter, angle, flutes, materials}] |
| `designs` | | `designs` [{id, name, modified, deleted, vv}] |
| `get` | `id` | `design` |
| `put` | `design` (with `base` and `vv`) | `stored`: kept (it was newer); `modified`: its copy's time now; `forked`: the id of the design one of two copies edited apart was kept as, with `moved` when that was the computer's copy (the one put took its place); or `conflict`: a deletion refused |
| `machine` | `id` | as `info`, after switching (refused while a machine is connected, unless pads may start cutting) |
| `send` | `design`, `machine` (id or ""), `start`, `from` | `started`, `message`, `notes`; as for `put`, `modified`, `forked` and `moved` (what opens is the pad's version: the fork unless `moved`; the computer's later copy when it already had one made from the pad's) |

The design format is described in `src/padlink.h`; `tests/data/pad-design.json` is an
example both the app's tests and the desktop's `test_padlink` check against.

### Versions: several pads, several computers

Pads carry designs between computers that never talk to each other, so "the newer time
wins" alone could let one edit replace another made meanwhile elsewhere. Every design
therefore carries `vv`, a version vector: `{"<id>": <modified>, ...}`, the time of the
latest edit each pad or phobiCCCP (by a random id of its own, kept in `.replica` beside
its designs) made to it. An edit sets its own entry to its new `modified`, so the newest
entry is always the copy's `modified`. A copy whose entries are all at least another's
was made from it and replaces it; equal vvs are the same copy. Two copies neither of
which was made from the other were edited apart: the computer keeps both, the later edit
(by `modified`, then by the vv as text) under the design's id and the other as a new
design, "<name> (conflict)", whose id comes only from that copy (its id, `modified` and
vv). Every computer settles the same two copies the same way whichever comes first, so
the forks of one conflict do not multiply as pads carry them around, and every copy ends
up the same. A deletion that loses is dropped (an edit made apart from it stays); one that
wins keeps the edit it would have dropped as the fork. When the computer's copy moves to
the fork (`moved`), its linked files go with it. The pad pushes a copy made from the
computer's or edited apart from it, pulls one made from its own, and after a conflict
takes the computer's copy under the id and the fork.

Compatibility: a vv is believed only when its newest entry is the copy's `modified`. A pad
or phobiCCCP from before vvs keeps the vv it got (as an unknown key) but stamps a new time,
so its edit is not believed to be made from anything, and the rules below (`base`, newer
time wins) apply to that pair, as they do to designs without a vv. An older phobiCCCP
ignores `vv` and uses the pad's `base`, which is now the one for that computer. An older
pad sees `forked` with or without `moved` as before and takes the computer's copy, which
is then its own version when `moved`. Pads from before this keep one base for all
computers, so with several computers they can still lose edits as before.

A design the pad sends carries `base`, the `modified` of the copy it last got from or gave
to that phobiCCCP (the pad keeps one per computer, by its id, in `bases`, which is never
sent; a computer it never exchanged the design with gets the oldest of them). When the
computer's own copy changed after that (and is not the copy sent), and either copy has no
vv to tell by,
it keeps the pad's version as a new design and answers `forked`; the pad then takes the
computer's copy in place of its own, and the fork comes with the next sync. The fork's id
comes from the design's id, `modified` and `base`, so the same edit put again (its answer
lost, or sent by both a `send` and a sync) is answered with the fork already made. A `send`
keeps the design the same way and answers likewise; when the answer's `modified` is the
sent copy's, or a sync finds the computer's copy at the same time as the pad's, the pad
moves its base to that time. A computer that cannot read its own copy of a design (made
by a newer phobiCCCP) refuses a put of it rather than write over it. A base newer
than the computer's copy only means the computer is behind. A `get` answer may carry
`desktopOnly`: the computer's name, when it will not rebuild its linked file from a pad
edit. Times more than a year ahead of the receiver's clock are refused or ignored by both
sides, so a clock gone wrong cannot freeze a design.

## Keeping the two apps in step

The pad and the desktop share one design format, so a change to what a design can hold
goes into both in the same pull request: the Dart model (`lib/model.dart`) and editor, and
on the desktop `padlink::check`, `buildDocument` and `designFromDocument` (`src/padlink.*`),
with the shared fixture and both apps' tests updated.
