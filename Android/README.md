# PhobiCCCP for Android

Design on a phone or tablet and cut on any phobiCCCP on the same network.

- **Syncs by itself.** While the app is open it looks for phobiCCCP every few seconds and
  keeps designs in step with every copy it finds. A design made or changed on one phone
  reaches every computer, and through them every other phone; the newest copy wins, and a
  deletion travels the same way. The tool library and materials come from phobiCCCP.
- **Several machines on one phobiCCCP.** The Machines tab lists each computer's machines
  (desktop: Machine › Add machine…) with their state; tap one to make it the one in use.
- **Several phobiCCCPs.** Install phobiCCCP on as many computers as you like; Send offers
  every one that is open, and every machine on it.
- **Send and start.** Tick *Start cutting right away* when sending. The phone asks once more,
  and the computer only starts when **Machine › Phones may start cutting** is on there (off by
  default) and the machine is connected, homed, idle and not busy; otherwise the design is
  opened and the phone is told why it did not start.

Designs are 2.5D, cut from the top: rectangles, circles, polygons, text, paths and drill
points, with profile (outside / inside / on), pocket, drill and engrave toolpaths. phobiCCCP
turns them into an ordinary `.c2d`, so anything else is done there.

## Install

Every CI run builds a debug APK: open the run under the repository's **Actions** tab and
download **PhobiCCCP-android**, then open `app-debug.apk` on the phone (allow installing from
that app when asked). Or build it yourself:

```sh
cd Android
./gradlew assembleDebug          # needs JDK 17 and the Android SDK (ANDROID_HOME or local.properties)
adb install app/build/outputs/apk/debug/app-debug.apk
./gradlew testDebugUnitTest      # the unit tests
```

Android Studio opens the `Android` folder as a project as well.

## Finding phobiCCCP

On the desktop, **File › Receive from Syntexophobic and phones** must be on (it is by
default). The phone and computer must be on the same network, and port 47652 (TCP and UDP)
open in the computer's firewall. Over Tailscale or across subnets, add the computer by name
or address on the Machines tab.

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
