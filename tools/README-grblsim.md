# grblsim - GRBL 1.1h simulator on a pty

`tools/grblsim.py` pretends to be a GRBL 1.1h board (Shapeoko-style, negative
machine space, home switches at back-right-top) behind a pseudo-terminal so the
Qt sender can be exercised with no hardware.  Python 3 stdlib only.

```
python3 tools/grblsim.py --speed 10            # prints PORT=/dev/pts/N, keeps running
python3 tools/grblsim.py --link /tmp/ttyGRBL   # stable path for the sender's port setting
python3 tools/grblsim_selftest.py              # drives a full session, exits 0 on PASS
```

Open the printed path in the sender as a serial port (any baud).  All RX/TX
is logged to stderr with `<<` / `>>` prefixes (`--quiet` to silence).
Ctrl-C (or `quit` on stdin) shuts it down cleanly.

## Flags

| Flag | Default | Meaning |
|---|---|---|
| `--speed N` | 1 | Simulated time runs N times faster than real time (motion, dwell, homing). |
| `--rapid MM_MIN` | 5000 | G0 / jog-cap rate. |
| `--require-homing` | off | Boot in `Alarm` with `[MSG:'$H'\|'$X' to unlock]`, MPos 0,0,0 (like `$22=1` + HOMING_INIT_LOCK). Default boots `Idle` at MPos -3,-3,-3. |
| `--strict` | off | Faithful reset semantics: 0x18 during motion prints `ALARM:3` (`ALARM:6` while homing) and leaves `Alarm`; an existing Alarm survives a reset. Default: reset always lands in `Idle`. |
| `--no-ack-comments` | off | Lines beginning with `(` get no response at all (Carbide-style) instead of `ok`. |
| `--merge-crlf` | off | Treat `\r\n` as one terminator. Stock GRBL treats `\r` and `\n` separately, so `\r\n` yields an extra `ok` for the empty line - the default reproduces that. |
| `--bitsetter X Y` | -20 -20 | Machine XY of the BitSetter button. |
| `--bitsetter-z Z` | -60 | Machine Z of the button surface with a zero-length tool. |
| `--bitsetter-radius R` | 10 | XY tolerance around the button. |
| `--tool-delta D` | 0 | Extra tool stick-out; surface is at `bitsetter-z + delta`. |
| `--m6 error\|ok\|hold` | error | `M6` -> `error:20` (stock GRBL), plain `ok`, or pause like `M0`. |
| `--link PATH` | | Symlink PATH to the pty (removed on exit). |
| `--quiet` | off | No RX/TX logging. |

## Runtime commands (simulator stdin)

- `tool <delta>` - simulate a tool change (moves the BitSetter contact height).
- `probe on|off|auto` - force the probe input, or return to the BitSetter model.
- `pos` - dump state / MPos / WCO / TLO / queue depth to stderr.
- `quit`

## What is modelled

- Banner `Grbl 1.1h ['$' for help]` on start and after 0x18.
- Realtime bytes: `?` `!` `~` `0x18` `0x85` (jog cancel) `0x84` (door) and the
  0x90-0xA1 override bytes (feed/rapid/spindle, reflected in `Ov:`).
- Status `<State|MPos:x,y,z|Bf:blocks,rx|FS:feed,rpm[|Pn:P][|WCO:...|Ov:...]>`.
  `WCO:` on the first report, every 10th, and forced after any offset change;
  `Ov:` every 20th (never together with WCO, like GRBL).  States: Idle, Run,
  Jog, Hold:0, Door:0, Home, Alarm, Check, Sleep.
- Planner buffer of 15 blocks / 128-byte RX buffer: lines are acked `ok` on
  parse, but a motion line is not acked until the planner has room.  Motion
  executes sequentially in a background thread at the commanded feed (rapids at
  `--rapid`), so `?` shows a moving MPos in `Run`/`Jog`, then `Idle`.
- Blocking commands behave like GRBL: `M3/M4/M5/M7/M8/M9`, `G4`, `G10`, `M0`,
  `M2/M30` and `G38.x` wait for queued motion to finish before completing
  (`M0`'s `ok` only arrives after `~`; the state during a `G4` dwell is `Idle`).
- `$H` homes Z then XY (seek at `$25`, locate at `$24`, pull-off `$27`), ends
  at MPos -3,-3,-3, `Home` state meanwhile.  `$X` -> `[MSG:Caught unlock]`.
- Offsets: G54-G59 via `G10 L2/L20`, `G92`/`G92.1`, `G43.1 Z`/`G49`.  Reported
  `WCO` = G5x + G92 + TLO(Z), exactly as GRBL does, so WPos = MPos - WCO.
  WCO survives a reset; TLO and G92 do not.
- Motion: `G0 G1 G2 G3` (arcs go straight to the endpoint), `G90/G91`,
  `G20/G21`, `G53`, `G28/G30`, `$J=` with `G91/G53/G20/G21/F`, `F` modal
  (`error:22` if a G1 has no feed).  Soft limits (`$20=1`) give `error:15` for
  jogs and the critical `ALARM:2` for g-code.
- Probing: `G38.2/.3/.4/.5`.  Over the BitSetter and moving down, Z stops at
  the surface: `[PRB:x,y,z:1]` then `ok`, `Pn:P` shown while touching.  Miss:
  `ALARM:5`, `[PRB:x,y,z:0]`, `ok` (GRBL's real ordering), `Alarm` state.
  Probe already triggered at start: `ALARM:4`.
- `$$`, `$#`, `$G`, `$I` (`[VER:1.1h.20190825:]`, `[OPT:V,15,128]`), `$N`,
  `$C`, `$SLP`, `$RST=`, `$n=value`; unknown `$` -> `error:3`; `$` commands
  needing Idle during a job -> `error:8`; g-code in Alarm/Jog -> `error:9`.

Not modelled: acceleration ramps (holds and jog cancels stop instantly),
arcs as arcs, real spindle/coolant outputs, hard limits, safety-door pins.
