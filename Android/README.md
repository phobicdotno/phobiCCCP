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
- **Sent from the computer to edit.** On the desktop, **File › Send to pad…** lists the pads
  that are open on the network and sends the open design to one of them; it opens there within
  a few seconds, and edits made on the pad come back to that file as above.
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
| `info` | `pad` {id, name} (optional) | host, version, id, `machines` [{id, name, active, size}], `activeMachine`, `status` {connected, state, homed, busy, streaming, job}, `allowStart`, `document` |
| `tools` | | `materials` [{id, name}], `tools` [{number, name, kind, diameter, angle, flutes, materials}] |
| `designs` | | `designs` [{id, name, modified, deleted, vv}] |
| `get` | `id` | `design` |
| `put` | `design` (with `base` and `vv`), `versions` | `stored`: kept (it was newer); `modified`: its copy's time now; `forked`: the id of the design one of two copies edited apart was kept as, with `moved` when that was the computer's copy (the one put took its place); or `conflict`: a deletion refused |
| `machine` | `id` | as `info`, after switching (refused while a machine is connected, unless pads may start cutting) |
| `send` | `design`, `machine` (id or ""), `start`, `from`, `versions` | `started`, `message`, `notes`; as for `put`, `modified`, `forked` and `moved` (what opens is the pad's version: the fork unless `moved`, or the pad's copy on its own, unlinked, when that fork was deleted since; the computer's later copy when it already had one made from the pad's) |
| `opened` | `seq`, `pad` (its id) | (nothing more) |

**Sent from the computer** (File › Send to pad…): the pad never listens, so a design for it
waits on the computer. Each sync round's `info` names the pad (`pad`: its own id, the one in its vvs,
and its name); the computer lists the pads heard in the last 20 seconds, and when a design
waits for that pad, the answer carries `open` {seq, design, name, from}. The design is an
ordinary synced one, so the same round pulls it; then the pad opens it and answers `opened`
with the `seq`. A request not taken in two minutes is given up. A pad that already opened a
`seq` (its `opened` answer lost) says `opened` again without opening it twice.

The design format is described in `src/padlink.h`; `tests/data/pad-design.json` is an
example both the app's tests and the desktop's `test_padlink` check against.

### Versions: several pads, several computers

Pads carry designs between computers that never talk to each other, so "the newer time
wins" alone could let one edit replace another made meanwhile elsewhere. Every design
therefore carries `vv`, a version vector: `{"<id>": <modified>, ...}`, the time of the
latest edit each pad or phobiCCCP (by a random id of its own) made to it. Two devices
with one id would each take the other's edit as made from its own, so the id stays on the
device it was made on: the pad keeps it in `.replica` beside its designs with a token from
a folder that is not backed up or moved to a new tablet (Android's `no_backup`, iOS's
`Caches`; Android backups also leave `.replica` out), and phobiCCCP keeps it in its local
app data (not in Documents, which may be synced or copied) tied to the computer and that
place. Designs restored from a backup, copied or synced elsewhere come without a matching
token there, so that device makes an id of its own (an id phobiCCCP kept in Documents
before 1.3.1 is dropped for a new one too, as a copy of Documents cannot be told from the
original). A new id only adds an entry to the vvs of the designs edited from then on. An edit sets its own entry to its new `modified`, so the newest
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
or phobiCCCP from before vvs (before 1.3.1) keeps the vv it got (as an unknown key) but
stamps a new time. That vv still names the copy it edited: against a copy with a believed
vv, one with an entry above it was not made from it, so the two were edited apart and both
are kept as above; otherwise the older one's edit was made from it and replaces it. A copy
with no vv at all (a design an older pad made, or one from before vvs) is likewise an edit
made from nothing a newer pad or phobiCCCP did. An edit of such a copy by a newer one keeps
its vv and notes the older edit under an id made from the copy's time and vv
(`olderEditId`, `e-` and 16 hex digits), so it counts as made from it. At most 64 entries
are kept: past that the oldest `e-` entries are dropped, so a design edited in turn by an
older and a newer client keeps a vv that is believed. Against a full vv, an `e-` entry it
lacks at or below the oldest `e-` time it holds counts as one of those dropped, known to it. Two copies neither of
which has a believed vv, or a deletion from an older pad (which keeps no vv), follow the
rules below (`base`, newer time wins).

An older phobiCCCP lists no vvs and keeps the later copy or forks by the base alone. A pad
asks it for the whole copy of each design that differs from its own, and settles two
copies edited apart there itself, the way every newer phobiCCCP does: the copy that loses
becomes a design of its own on the pad, under the id the others would give it, and the
winner goes to that computer with its copy as the base, so it replaces it. A copy made
from the computer's goes with that copy as its base too, so it is not forked. An older
pad sees `forked` with or without `moved` as before and takes the computer's copy, which
is then its own version when `moved`. Pads from before this keep one base for all
computers, so an older pad that syncs with more than one computer can still lose edits
as before: update every pad. phobiCCCP says so in its status bar the first time a pad
from before 1.3.1 puts or sends a design (newer pads add `"versions": 1` to `put` and
`send`). A 1.3.0 phobiCCCP with newer pads can still leave a design twice
(the same copy, once as "(pad)"): update every computer too.

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
