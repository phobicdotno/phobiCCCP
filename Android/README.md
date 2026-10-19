# PhobiCCCP for pads

Design on an Android tablet or an iPad and cut on any phobiCCCP on the same network. The
app is written in Flutter, so the same code builds for both. It looks like the desktop
program: the same menu bar, 1 · Design / 2 · Toolpaths / 3 · Machine switch, tool bar,
drawing with its grid, Properties / Document / Toolpaths panels, colours and Inter font,
laid out for a pad in landscape.

- **Syncs by itself, both ways.** While the app is open it looks for phobiCCCP every few
  seconds and keeps designs in step with every copy it finds. A design made or changed on
  one pad reaches every computer, and through them every other pad; the newest copy wins,
  and a deletion travels the same way. The tool library and materials come from phobiCCCP.
- **Edits on the desktop come back.** A design sent from a pad opens on the computer as an
  ordinary `.c2d`. Saving it there sends the change back to the pads. A later pad edit
  rebuilds that `.c2d` (and reloads it, when it is open with nothing unsaved), as long as
  the file holds only what the pad can: once something desktop-only is added (V-carves,
  curves, tabs, feeds set by hand, a 3D model), the pads still get the change, but a pad
  edit no longer overwrites the file, and the status bar says so.
- **Several machines on one phobiCCCP.** The Machine page lists each computer's machines
  (desktop: Machine › Add machine…) with their state; tap one to make it the one in use.
- **Several phobiCCCPs.** Install phobiCCCP on as many computers as you like; Send offers
  every one that is open, and every machine on it.
- **Send and start.** Tick *Start cutting right away* when sending. The pad asks once more,
  and the computer only starts when **Machine › Pads may start cutting** is on there (off by
  default) and the machine is connected, homed, idle and not busy; otherwise the design is
  opened and the pad is told why it did not start.

Designs are 2.5D, cut from the top: rectangles, circles, polygons, text, paths and drill
points, with contour (outside / inside / on), pocket, drill and engrave toolpaths, each of
which can be ticked off as on the desktop.

The fonts in `fonts/` (Inter, Liberation Sans for text on the drawing, DejaVu Sans Mono)
are the ones the desktop uses; their licences are in `fonts/LICENSE.txt`.

## Install

**Android:** on the tablet, open the repository's **Releases** page, download the newest
`PhobiCCCP-x.y.z.apk` and open it (allow installing from that app when asked). Pushing a
version tag (`git tag v1.2.3 && git push origin v1.2.3`) builds and publishes that release.
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
| `designs` | | `designs` [{id, name, modified, deleted}] |
| `get` | `id` | `design` |
| `put` | `design` | `stored`: kept (it was newer) |
| `machine` | `id` | as `info`, after switching |
| `send` | `design`, `machine` (id or ""), `start`, `from` | `started`, `message`, `notes` |

The design format is described in `src/padlink.h`; `tests/data/pad-design.json` is an
example both the app's tests and the desktop's `test_padlink` check against.

## Keeping the two apps in step

The pad and the desktop share one design format, so a change to what a design can hold
goes into both in the same pull request: the Dart model (`lib/model.dart`) and editor, and
on the desktop `padlink::check`, `buildDocument` and `designFromDocument` (`src/padlink.*`),
with the shared fixture and both apps' tests updated.
