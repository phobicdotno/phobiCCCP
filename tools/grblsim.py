#!/usr/bin/env python3
"""grblsim.py - GRBL 1.1h controller simulator on a pseudo-terminal.

Python 3 stdlib only.  Opens a pty pair, prints ``PORT=/dev/pts/N`` on stdout
and then behaves (closely) like a GRBL 1.1h board on a Shapeoko-style machine
with negative machine space and the home switches at back-right-top.

    python3 tools/grblsim.py [--speed 10] [--require-homing] [--strict] ...

Runtime commands on the simulator's own stdin:
    tool <delta>     extra length (mm) of the loaded tool (BitSetter surface moves up)
    probe on|off|auto  force the probe input, or let the BitSetter model drive it
    pos              print machine state
    quit             exit
"""
import argparse
import collections
import math
import os
import queue
import re
import signal
import sys
import threading
import time
import tty

BANNER = "Grbl 1.1h ['$' for help]"
PLANNER_BLOCKS = 15
RX_BUFFER = 128
TICK = 0.005  # real seconds per motion tick
AX = "XYZ"

DEFAULT_SETTINGS = [
    ("0", "10"), ("1", "255"), ("2", "0"), ("3", "0"), ("4", "0"), ("5", "0"),
    ("6", "0"), ("10", "255"), ("11", "0.020"), ("12", "0.002"), ("13", "0"),
    ("20", "0"), ("21", "0"), ("22", "1"), ("23", "0"), ("24", "100.000"),
    ("25", "2000.000"), ("26", "25"), ("27", "3.000"), ("30", "10000"),
    ("31", "0"), ("32", "0"),
    ("100", "40.000"), ("101", "40.000"), ("102", "40.000"),
    ("110", "5000.000"), ("111", "5000.000"), ("112", "5000.000"),
    ("120", "400.000"), ("121", "400.000"), ("122", "400.000"),
    ("130", "1220.000"), ("131", "1220.000"), ("132", "150.000"),
]

NUM_RE = re.compile(r"[-+]?(\d+\.?\d*|\.\d+)")


def f3(v):
    s = "%.3f" % v
    return "0.000" if s == "-0.000" else s


def fmt3(vec):
    return ",".join(f3(v) for v in vec)


class GErr(Exception):
    """G-code / system error -> 'error:N'."""

    def __init__(self, code):
        super().__init__(code)
        self.code = code


class Aborted(Exception):
    """Soft reset happened while a line was being executed."""


class Item:
    __slots__ = ("kind", "target", "feed", "line")

    def __init__(self, kind, target=None, feed=0.0, line=""):
        self.kind = kind      # rapid | feed | jog | probe_toward | probe_away | home
        self.target = target
        self.feed = feed
        self.line = line


class Sim:
    def __init__(self, a):
        self.a = a
        self.lock = threading.RLock()
        self.cv = threading.Condition(self.lock)
        self.gen = 0                      # bumped by every soft reset
        self.stop = False

        # machine model
        self.rapid = a.rapid
        self.settings = collections.OrderedDict(DEFAULT_SETTINGS)
        self.bitsetter = (a.bitsetter[0], a.bitsetter[1])
        self.bitsetter_z = a.bitsetter_z
        self.bitsetter_r = a.bitsetter_radius
        self.tool_delta = a.tool_delta
        self.probe_force = None           # None=auto, True/False = forced input

        self.alarm = a.require_homing
        self.homing = False
        self.hold = False
        self.door = False
        self.sleep = False
        self.check_mode = False
        self.critical = False
        self.motion_state = "Idle"        # Idle | Run | Jog
        self.mpos = [0.0, 0.0, 0.0] if a.require_homing else [-3.0, -3.0, -3.0]
        self.plan_pos = list(self.mpos)   # planner end position (machine coords)
        self.feed_actual = 0.0
        self.spindle_rpm = 0.0
        self.ov = [100, 100, 100]
        self.force_wco = True
        self.force_ov = False
        self.report_count = 0
        self.rx_pending = 0               # bytes received but not yet parsed

        self.coord = [[0.0, 0.0, 0.0] for _ in range(6)]  # G54..G59
        self.g28 = [0.0, 0.0, 0.0]
        self.g30 = [0.0, 0.0, 0.0]
        self.g92 = [0.0, 0.0, 0.0]
        self.tlo = 0.0
        self.prb = [0.0, 0.0, 0.0]
        self.prb_ok = 0
        self.reset_modal()

        self.q = collections.deque()      # planner queue of Item
        self.pending = queue.Queue()      # complete RX lines waiting for the parser
        self.jog_cancel = False
        self.exec_busy = False
        self.parser_busy = False
        self.tx = queue.Queue()

        self.master, self.slave = os.openpty()
        tty.setraw(self.slave)
        self.port = os.ttyname(self.slave)

    # ------------------------------------------------------------ helpers
    def reset_modal(self):
        self.modal = dict(motion=0.0, coord=54, plane=17, units=21, distance=90,
                          feedmode=94, spindle=5, coolant=9, F=0.0, S=0.0, T=0,
                          flow=0)

    def log(self, prefix, text):
        if not self.a.quiet:
            sys.stderr.write("%s %s\n" % (prefix, text))
            sys.stderr.flush()

    def send(self, line):
        self.log(">>", line)
        self.tx.put((line + "\r\n").encode("latin-1", "replace"))

    def aborted(self, gen):
        return self.gen != gen

    def wco_total(self):
        w = self.coord[self.modal["coord"] - 54]
        return [w[0] + self.g92[0], w[1] + self.g92[1], w[2] + self.g92[2] + self.tlo]

    def probe_triggered(self):
        if self.probe_force is not None:
            return self.probe_force
        x, y, z = self.mpos
        surface = self.bitsetter_z + self.tool_delta
        return (math.hypot(x - self.bitsetter[0], y - self.bitsetter[1]) <= self.bitsetter_r
                and z <= surface + 0.01)

    def state_str(self):
        if self.sleep:
            return "Sleep"
        if self.homing:
            return "Home"
        if self.alarm:
            return "Alarm"
        if self.check_mode:
            return "Check"
        if self.door:
            return "Door:0"
        if self.hold:
            return "Hold:0"
        return self.motion_state

    def state_idle_or_alarm(self):
        return (self.motion_state == "Idle" and not self.q and not self.hold
                and not self.homing and not self.door)

    # ------------------------------------------------------------ threads
    def start(self):
        for fn in (self.tx_loop, self.rx_loop, self.parser_loop, self.exec_loop, self.stdin_loop):
            threading.Thread(target=fn, daemon=True).start()
        self.send(BANNER)
        if self.alarm:
            self.send("[MSG:'$H'|'$X' to unlock]")

    def tx_loop(self):
        while not self.stop:
            try:
                data = self.tx.get(timeout=0.2)
            except queue.Empty:
                continue
            try:
                while data:
                    n = os.write(self.master, data)
                    data = data[n:]
            except OSError:
                return

    def rx_loop(self):
        buf = bytearray()
        last = None
        while not self.stop:
            try:
                data = os.read(self.master, 4096)
            except OSError:
                return
            if not data:
                return
            for b in data:
                if b == 0x3F:            # ?
                    self.status_report()
                elif b == 0x21:          # !
                    self.log("<<", "[!]")
                    self.feed_hold()
                elif b == 0x7E:          # ~
                    self.log("<<", "[~]")
                    self.cycle_start()
                elif b == 0x18:
                    self.log("<<", "[0x18 soft-reset]")
                    buf.clear()
                    self.soft_reset()
                elif b == 0x85:
                    self.log("<<", "[0x85 jog-cancel]")
                    self.jog_cancel_cmd()
                elif b == 0x84:
                    self.log("<<", "[0x84 safety-door]")
                    self.safety_door()
                elif 0x90 <= b <= 0xA1:
                    self.log("<<", "[0x%02X override]" % b)
                    self.override(b)
                elif b in (0x0A, 0x0D):
                    if b == 0x0A and last == 0x0D and self.a.merge_crlf:
                        last = b
                        continue
                    line = buf.decode("latin-1")
                    buf.clear()
                    self.log("<<", repr(line))
                    with self.lock:
                        self.rx_pending += len(line) + 1
                    self.pending.put((self.gen, line))
                else:
                    buf.append(b)
                last = b

    def stdin_loop(self):
        try:
            for raw in sys.stdin:
                cmd = raw.strip().split()
                if not cmd:
                    continue
                c = cmd[0].lower()
                try:
                    if c == "tool" and len(cmd) > 1:
                        with self.lock:
                            self.tool_delta = float(cmd[1])
                        sys.stderr.write("## tool delta = %.3f (BitSetter surface at Z%.3f)\n"
                                         % (self.tool_delta, self.bitsetter_z + self.tool_delta))
                    elif c == "probe" and len(cmd) > 1:
                        with self.lock:
                            self.probe_force = {"on": True, "off": False}.get(cmd[1].lower())
                        sys.stderr.write("## probe input = %s\n" % cmd[1])
                    elif c == "pos":
                        with self.lock:
                            sys.stderr.write("## %s MPos=%s WCO=%s TLO=%.3f queue=%d\n" % (
                                self.state_str(), fmt3(self.mpos), fmt3(self.wco_total()),
                                self.tlo, len(self.q)))
                    elif c in ("quit", "q", "exit"):
                        self.stop = True
                        os.kill(os.getpid(), signal.SIGINT)
                    else:
                        sys.stderr.write("## commands: tool <delta> | probe on|off|auto | pos | quit\n")
                    sys.stderr.flush()
                except ValueError:
                    sys.stderr.write("## bad value\n")
        except (OSError, ValueError):
            pass

    # ------------------------------------------------------------ realtime
    def status_report(self):
        with self.lock:
            parts = [self.state_str(), "MPos:" + fmt3(self.mpos)]
            parts.append("Bf:%d,%d" % (PLANNER_BLOCKS - len(self.q), max(0, RX_BUFFER - self.rx_pending)))
            parts.append("FS:%d,%d" % (round(self.feed_actual), round(self.spindle_rpm)))
            if self.probe_triggered():
                parts.append("Pn:P")
            n = self.report_count
            self.report_count += 1
            if self.force_wco or n % 10 == 0:
                self.force_wco = False
                parts.append("WCO:" + fmt3(self.wco_total()))
            elif self.force_ov or n % 20 == 1:
                self.force_ov = False
                parts.append("Ov:%d,%d,%d" % tuple(self.ov))
                acc = ""
                if self.modal["spindle"] == 3:
                    acc += "S"
                elif self.modal["spindle"] == 4:
                    acc += "C"
                if self.modal["coolant"] == 8:
                    acc += "F"
                elif self.modal["coolant"] == 7:
                    acc += "M"
                if acc:
                    parts.append("A:" + acc)
        self.send("<" + "|".join(parts) + ">")

    def feed_hold(self):
        with self.lock:
            if self.alarm or self.check_mode or self.sleep or self.homing:
                return
            if self.motion_state == "Jog" or any(i.kind == "jog" for i in self.q):
                self._cancel_jogs()
                return
            self.hold = True
            self.cv.notify_all()

    def cycle_start(self):
        with self.lock:
            self.hold = False
            self.door = False
            self.cv.notify_all()

    def _cancel_jogs(self):
        # called with lock held
        for it in [i for i in self.q if i.kind == "jog"]:
            self.q.remove(it)
        if self.motion_state == "Jog":
            self.jog_cancel = True
        elif self.motion_state == "Idle" and not self.q:
            self.plan_pos = list(self.mpos)
        self.cv.notify_all()

    def jog_cancel_cmd(self):
        with self.lock:
            self._cancel_jogs()

    def safety_door(self):
        with self.lock:
            if self.alarm or self.homing:
                return
            self.door = True
            self.cv.notify_all()

    def override(self, b):
        with self.lock:
            f, r, s = self.ov
            if b == 0x90: f = 100
            elif b == 0x91: f = min(200, f + 10)
            elif b == 0x92: f = max(10, f - 10)
            elif b == 0x93: f = min(200, f + 1)
            elif b == 0x94: f = max(10, f - 1)
            elif b == 0x95: r = 100
            elif b == 0x96: r = 50
            elif b == 0x97: r = 25
            elif b == 0x99: s = 100
            elif b == 0x9A: s = min(200, s + 10)
            elif b == 0x9B: s = max(10, s - 10)
            elif b == 0x9C: s = min(200, s + 1)
            elif b == 0x9D: s = max(10, s - 1)
            elif b == 0xA0: self.modal["coolant"] = 9 if self.modal["coolant"] == 8 else 8
            elif b == 0xA1: self.modal["coolant"] = 9 if self.modal["coolant"] == 7 else 7
            if [f, r, s] != self.ov:
                self.ov = [f, r, s]
                self.force_ov = True

    def soft_reset(self):
        with self.lock:
            was_homing = self.homing
            was_moving = self.motion_state != "Idle" or was_homing
            self.gen += 1
            self.hold = False
            self.door = False
            self.jog_cancel = False
            self.q.clear()
            while True:
                try:
                    self.pending.get_nowait()
                except queue.Empty:
                    break
            self.rx_pending = 0
            self.cv.notify_all()
            deadline = time.monotonic() + 2.0
            while (self.parser_busy or self.exec_busy) and time.monotonic() < deadline:
                self.cv.wait(0.02)
            # finalize
            if self.a.strict:
                if was_moving:
                    self.alarm = True
                    self.send("ALARM:6" if was_homing else "ALARM:3")
            else:
                self.alarm = False
            self.homing = False
            self.sleep = False
            self.check_mode = False
            self.critical = False
            self.motion_state = "Idle"
            self.feed_actual = 0.0
            self.spindle_rpm = 0.0
            self.tlo = 0.0
            self.g92 = [0.0, 0.0, 0.0]
            self.ov = [100, 100, 100]
            self.reset_modal()
            self.plan_pos = list(self.mpos)
            self.force_wco = True
            self.report_count = 0
            self.send(BANNER)
            if self.alarm:
                self.send("[MSG:'$H'|'$X' to unlock]")

    # ------------------------------------------------------------ executor
    def exec_loop(self):
        while not self.stop:
            with self.lock:
                while not self.q and not self.stop:
                    self.cv.wait(0.2)
                if self.stop:
                    return
                item = self.q.popleft()
                self.exec_busy = True
                gen = self.gen
            try:
                res = self.run_motion(item, gen)
                if res == "cancel":
                    with self.lock:
                        self.plan_pos = list(self.mpos)
                        self.jog_cancel = False
            finally:
                with self.lock:
                    self.exec_busy = False
                    if not self.q:
                        self.motion_state = "Idle"
                        self.feed_actual = 0.0
                    self.cv.notify_all()

    def run_motion(self, item, gen):
        """Move mpos toward item.target over simulated time.  Returns
        'done' | 'abort' | 'cancel' | 'probe'."""
        kind = item.kind
        with self.lock:
            start = list(self.mpos)
            target = list(item.target)
            if kind == "jog":
                self.motion_state = "Jog"
            elif kind != "home":
                self.motion_state = "Run"
        total = math.dist(start, target)
        progress = 0.0
        last = time.monotonic()
        while True:
            time.sleep(TICK)
            now = time.monotonic()
            dt = (now - last) * self.a.speed
            last = now
            with self.lock:
                if self.aborted(gen):
                    self.feed_actual = 0.0
                    return "abort"
                if kind == "jog" and self.jog_cancel:
                    self.feed_actual = 0.0
                    return "cancel"
                if (self.hold or self.door) and kind != "home":
                    if kind == "jog":
                        self.jog_cancel = True
                        return "cancel"
                    self.feed_actual = 0.0
                    continue
                if kind == "rapid":
                    rate = self.rapid * self.ov[1] / 100.0
                elif kind in ("feed", "jog"):
                    rate = item.feed * self.ov[0] / 100.0
                else:
                    rate = item.feed
                self.feed_actual = rate
                if total <= 0:
                    self.mpos = target
                    return "done"
                progress = min(total, progress + rate / 60.0 * dt)
                frac = progress / total
                newpos = [s + (t - s) * frac for s, t in zip(start, target)]
                if kind == "probe_toward":
                    prev_z = self.mpos[2]
                    self.mpos = newpos
                    if self.probe_triggered():
                        # snap to the exact contact height when we crossed the surface
                        surface = self.bitsetter_z + self.tool_delta
                        if self.probe_force is None and prev_z > surface > target[2]:
                            f = (surface - start[2]) / (target[2] - start[2])
                            self.mpos = [s + (t - s) * f for s, t in zip(start, target)]
                        return "probe"
                elif kind == "probe_away":
                    self.mpos = newpos
                    if not self.probe_triggered():
                        return "probe"
                else:
                    self.mpos = newpos
                if progress >= total:
                    self.mpos = list(target)
                    return "done"

    # ------------------------------------------------------------ parser
    def parser_loop(self):
        while not self.stop:
            try:
                gen, line = self.pending.get(timeout=0.2)
            except queue.Empty:
                continue
            with self.lock:
                if gen != self.gen:
                    continue
                self.rx_pending = max(0, self.rx_pending - len(line) - 1)
                self.parser_busy = True
                self.cur_gen = gen
            try:
                self.process_line(line)
            except Aborted:
                pass
            except GErr as e:
                self.send("error:%d" % e.code)
            except Exception as e:  # pragma: no cover
                sys.stderr.write("## internal error on %r: %r\n" % (line, e))
                self.send("error:1")
            finally:
                with self.lock:
                    self.parser_busy = False
                    self.cv.notify_all()

    def check_abort(self):
        if self.aborted(self.cur_gen):
            raise Aborted()

    def wait_until(self, pred):
        """Block the parser thread until pred() (checked under lock) or reset."""
        with self.lock:
            while not pred():
                self.check_abort()
                self.cv.wait(0.02)
            self.check_abort()

    def sync(self):
        """protocol_buffer_synchronize(): wait for the planner to drain."""
        self.wait_until(lambda: not self.q and not self.exec_busy)

    def enqueue(self, item):
        self.wait_until(lambda: len(self.q) < PLANNER_BLOCKS)
        with self.lock:
            if self.critical:
                raise Aborted()
            self.q.append(item)
            self.plan_pos = list(item.target)
            self.cv.notify_all()

    def sim_sleep(self, seconds):
        end = time.monotonic() + seconds / self.a.speed
        while time.monotonic() < end:
            self.check_abort()
            time.sleep(min(0.02, max(0.0, end - time.monotonic())))

    def process_line(self, raw):
        if self.a.no_ack_comments and raw.lstrip().startswith("("):
            return
        # strip comments and whitespace, uppercase (like GRBL's protocol loop)
        out = []
        depth = 0
        for ch in raw:
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth = max(0, depth - 1)
            elif ch == ";" and depth == 0:
                break
            elif depth == 0 and not ch.isspace():
                out.append(ch.upper())
        line = "".join(out)
        if len(line) > 79:
            raise GErr(14)
        if self.critical:
            raise Aborted()          # GRBL is stuck in the critical-alarm loop until reset
        if self.sleep:
            raise Aborted()
        if not line:
            self.send("ok")
            return
        if line[0] == "$":
            self.system_cmd(line)
            self.send("ok")
            return
        with self.lock:
            if self.alarm or self.motion_state == "Jog" or any(i.kind == "jog" for i in self.q):
                raise GErr(9)
        self.gcode(line, jog=False)
        self.send("ok")

    # ------------------------------------------------------------ $ commands
    def system_cmd(self, line):
        body = line[1:]
        with self.lock:
            idle_or_alarm = self.state_idle_or_alarm()
        if body == "":
            self.send("[HLP:$$ $# $G $I $N $x=val $Nx=line $J=line $SLP $C $X $H ~ ! ? ctrl-x]")
        elif body == "$":
            with self.lock:
                if self.motion_state == "Run" or self.hold:
                    raise GErr(8)
            for k, v in self.settings.items():
                self.send("$%s=%s" % (k, v))
        elif body == "G":
            self.send(self.parser_state())
        elif body == "X":
            with self.lock:
                if self.alarm:
                    self.alarm = False
                    self.send("[MSG:Caught unlock]")
        elif body == "C":
            with self.lock:
                if not idle_or_alarm:
                    raise GErr(8)
                if self.check_mode:
                    self.check_mode = False
                    self.send("[MSG:Disabled]")
                else:
                    self.check_mode = True
                    self.send("[MSG:Enabled]")
        elif body.startswith("J="):
            with self.lock:
                if self.alarm or self.motion_state == "Run" or self.hold or self.homing or self.door:
                    raise GErr(8)
            self.gcode(body[2:], jog=True)
        else:
            if not idle_or_alarm:
                raise GErr(8)
            if body == "#":
                self.report_ngc()
            elif body == "I":
                self.send("[VER:1.1h.20190825:]")
                self.send("[OPT:V,%d,%d]" % (PLANNER_BLOCKS, RX_BUFFER))
            elif body == "H":
                if self.settings.get("22", "0") == "0":
                    raise GErr(5)
                self.home()
            elif body == "N":
                self.send("$N0=")
                self.send("$N1=")
            elif body.startswith("N") and "=" in body:
                pass
            elif body == "SLP":
                self.send("[MSG:Sleeping]")
                with self.lock:
                    self.sleep = True
            elif body.startswith("RST="):
                if body[4:] not in ("$", "#", "*"):
                    raise GErr(3)
                if body[4:] in ("$", "*"):
                    self.settings = collections.OrderedDict(DEFAULT_SETTINGS)
                if body[4:] in ("#", "*"):
                    with self.lock:
                        self.coord = [[0.0] * 3 for _ in range(6)]
                        self.g28 = [0.0] * 3
                        self.g30 = [0.0] * 3
                        self.force_wco = True
                self.send("[MSG:Restoring defaults]")
            elif "=" in body:
                k, _, v = body.partition("=")
                if not k.isdigit() or k not in self.settings:
                    raise GErr(3)
                try:
                    fv = float(v)
                except ValueError:
                    raise GErr(2)
                old = self.settings[k]
                self.settings[k] = ("%.3f" % fv) if "." in old else str(int(fv))
            else:
                raise GErr(3)

    def parser_state(self):
        m = self.modal
        motion = {0.0: "G0", 1.0: "G1", 2.0: "G2", 3.0: "G3", 38.2: "G38.2", 38.3: "G38.3",
                  38.4: "G38.4", 38.5: "G38.5", 80.0: "G80"}[m["motion"]]
        parts = [motion, "G%d" % m["coord"], "G%d" % m["plane"], "G%d" % m["units"],
                 "G%d" % m["distance"], "G%d" % m["feedmode"]]
        if m["flow"]:
            parts.append("M%d" % m["flow"])
        parts.append("M%d" % m["spindle"])
        parts.append("M%d" % m["coolant"])
        parts.append("T%d" % m["T"])
        f = m["F"] / (25.4 if m["units"] == 20 else 1.0)
        parts.append("F%d" % round(f) if m["units"] == 21 else "F%.1f" % f)
        parts.append("S%d" % round(m["S"]))
        return "[GC:" + " ".join(parts) + "]"

    def report_ngc(self):
        with self.lock:
            for i in range(6):
                self.send("[G%d:%s]" % (54 + i, fmt3(self.coord[i])))
            self.send("[G28:%s]" % fmt3(self.g28))
            self.send("[G30:%s]" % fmt3(self.g30))
            self.send("[G92:%s]" % fmt3(self.g92))
            self.send("[TLO:%s]" % f3(self.tlo))
            self.send("[PRB:%s:%d]" % (fmt3(self.prb), self.prb_ok))

    def home(self):
        seek = float(self.settings.get("25", "2000"))
        feed = float(self.settings.get("24", "100"))
        pulloff = float(self.settings.get("27", "3"))
        with self.lock:
            self.homing = True
            self.hold = False
            self.motion_state = "Idle"
        try:
            gen = self.cur_gen

            def leg(target, rate):
                res = self.run_motion(Item("home", target, rate), gen)
                if res != "done":
                    raise Aborted()

            # Z first, then X and Y together (GRBL default homing sequence)
            for axes in ((2,), (0, 1)):
                def tgt(val):
                    with self.lock:
                        t = list(self.mpos)
                    for ax in axes:
                        t[ax] = val
                    return t
                leg(tgt(0.0), seek)               # seek to the switches
                leg(tgt(-pulloff), seek)          # pull off
                leg(tgt(0.0), feed)               # locate slowly
                leg(tgt(-pulloff), seek)          # final pull-off
            with self.lock:
                self.mpos = [-pulloff, -pulloff, -pulloff]
                self.plan_pos = list(self.mpos)
                self.alarm = False
                self.force_wco = True
        finally:
            with self.lock:
                self.homing = False
                self.feed_actual = 0.0
                self.motion_state = "Idle"

    # ------------------------------------------------------------ g-code
    def tokenize(self, line):
        toks = []
        i = 0
        while i < len(line):
            c = line[i]
            if not ("A" <= c <= "Z"):
                raise GErr(1)
            m = NUM_RE.match(line, i + 1)
            if not m:
                raise GErr(2)
            toks.append((c, float(m.group(0))))
            i = m.end()
        return toks

    def gcode(self, line, jog):
        toks = self.tokenize(line)
        gw, mw, vals = [], [], {}
        for letter, v in toks:
            if letter == "G":
                gw.append(round(v, 1))
            elif letter == "M":
                mw.append(int(round(v)))
            else:
                if letter in vals:
                    raise GErr(25)
                vals[letter] = v

        motion = nonmodal = distance = units = plane = coord = feedmode = tlo_cmd = None

        def once(cur, g):
            if cur is not None:
                raise GErr(24)
            return g

        for g in gw:
            if g in (0.0, 1.0, 2.0, 3.0, 38.2, 38.3, 38.4, 38.5, 80.0):
                motion = once(motion, g)
            elif g in (4.0, 10.0, 28.0, 28.1, 30.0, 30.1, 53.0, 92.0, 92.1):
                nonmodal = once(nonmodal, g)
            elif g in (90.0, 91.0):
                distance = once(distance, int(g))
            elif g in (20.0, 21.0):
                units = once(units, int(g))
            elif g in (17.0, 18.0, 19.0):
                plane = once(plane, int(g))
            elif 54.0 <= g <= 59.0 and g == int(g):
                coord = once(coord, int(g))
            elif g in (93.0, 94.0):
                feedmode = once(feedmode, int(g))
            elif g in (43.1, 49.0):
                tlo_cmd = once(tlo_cmd, g)
            else:
                raise GErr(20)
        if jog:
            if mw or motion is not None or plane or coord or feedmode or tlo_cmd is not None \
                    or (nonmodal not in (None, 53.0)):
                raise GErr(32)
            if not any(a in vals for a in AX):
                raise GErr(32)
        for m in mw:
            if m not in (0, 1, 2, 30, 3, 4, 5, 6, 7, 8, 9):
                raise GErr(20)

        m_ = self.modal
        cur_units = units if units is not None else m_["units"]
        scale = 25.4 if cur_units == 20 else 1.0
        cur_dist = distance if distance is not None else m_["distance"]
        axis_words = {a: vals[a] * scale for a in AX if a in vals}

        if "F" in vals:
            feed = vals["F"] * scale
            if not jog:
                m_["F"] = feed
        else:
            feed = m_["F"]
            if jog:
                raise GErr(22)

        if self.check_mode and not jog:
            return

        if jog:
            target = self.compute_target(axis_words, cur_dist, nonmodal == 53.0)
            if self.settings.get("20") == "1" and not self.within_travel(target):
                raise GErr(15)
            self.enqueue(Item("jog", target, feed, line))
            return

        # ---- non-jog execution, in GRBL's order
        if units is not None:
            m_["units"] = units
        if "S" in vals:
            if m_["S"] != vals["S"] and m_["spindle"] != 5:
                self.sync()
                with self.lock:
                    self.spindle_rpm = vals["S"]
            m_["S"] = vals["S"]
        if "T" in vals:
            m_["T"] = int(vals["T"])
        for m in mw:
            if m == 6:
                if self.a.m6 == "error":
                    raise GErr(20)
                elif self.a.m6 == "hold":
                    self.program_pause()
            elif m in (3, 4, 5):
                if m_["spindle"] != m:
                    self.sync()
                    m_["spindle"] = m
                    with self.lock:
                        self.spindle_rpm = 0.0 if m == 5 else m_["S"]
                        self.force_ov = True
            elif m in (7, 8, 9):
                if m_["coolant"] != m:
                    self.sync()
                    m_["coolant"] = m
                    with self.lock:
                        self.force_ov = True
        if plane is not None:
            m_["plane"] = plane
        if feedmode is not None:
            m_["feedmode"] = feedmode
        if distance is not None:
            m_["distance"] = distance
        if coord is not None and coord != m_["coord"]:
            with self.lock:
                m_["coord"] = coord
                self.force_wco = True
        if tlo_cmd == 43.1:
            if "Z" not in axis_words:
                raise GErr(38)
            with self.lock:
                self.tlo = axis_words.pop("Z")
                self.force_wco = True
        elif tlo_cmd == 49.0:
            with self.lock:
                self.tlo = 0.0
                self.force_wco = True

        consumed = False
        if nonmodal == 4.0:
            if "P" not in vals:
                raise GErr(38)
            self.sync()
            self.sim_sleep(max(0.0, vals["P"]))
        elif nonmodal == 10.0:
            l = int(vals.get("L", 0))
            p = int(vals.get("P", 0))
            if l not in (2, 20) or not 0 <= p <= 6:
                raise GErr(38)
            idx = (m_["coord"] - 54) if p == 0 else p - 1
            self.sync()
            with self.lock:
                cs = self.coord[idx]
                for i, a in enumerate(AX):
                    if a in axis_words:
                        if l == 20:
                            cs[i] = self.plan_pos[i] - self.g92[i] - axis_words[a]
                            if i == 2:
                                cs[i] -= self.tlo
                        else:
                            cs[i] = axis_words[a]
                if idx == m_["coord"] - 54:
                    self.force_wco = True
            consumed = True
        elif nonmodal in (28.0, 30.0):
            store = self.g28 if nonmodal == 28.0 else self.g30
            if axis_words:
                self.enqueue(Item("rapid", self.compute_target(axis_words, cur_dist, False), 0.0, line))
            with self.lock:
                tgt = list(store)
            self.enqueue(Item("rapid", tgt, 0.0, line))
            consumed = True
        elif nonmodal in (28.1, 30.1):
            with self.lock:
                pos = list(self.plan_pos)
                if nonmodal == 28.1:
                    self.g28 = pos
                else:
                    self.g30 = pos
            consumed = True
        elif nonmodal == 92.0:
            with self.lock:
                cs = self.coord[m_["coord"] - 54]
                for i, a in enumerate(AX):
                    if a in axis_words:
                        self.g92[i] = self.plan_pos[i] - cs[i] - axis_words[a]
                        if i == 2:
                            self.g92[i] -= self.tlo
                self.force_wco = True
            consumed = True
        elif nonmodal == 92.1:
            with self.lock:
                self.g92 = [0.0, 0.0, 0.0]
                self.force_wco = True

        if motion is not None:
            m_["motion"] = motion
        if motion is not None or (axis_words and not consumed):
            mm = m_["motion"]
            if nonmodal == 53.0 and mm not in (0.0, 1.0):
                raise GErr(30)
            if mm in (0.0, 1.0, 2.0, 3.0):
                if axis_words and not consumed:
                    target = self.compute_target(axis_words, cur_dist, nonmodal == 53.0)
                    if mm == 0.0:
                        self.soft_limit_check(target)
                        self.enqueue(Item("rapid", target, 0.0, line))
                    else:
                        if feed <= 0:
                            raise GErr(22)
                        self.soft_limit_check(target)
                        self.enqueue(Item("feed", target, feed, line))
            elif mm in (38.2, 38.3, 38.4, 38.5):
                if not axis_words:
                    raise GErr(35)
                if feed <= 0:
                    raise GErr(22)
                target = self.compute_target(axis_words, cur_dist, False)
                self.probe(target, feed, mm, line)

        for m in mw:
            if m == 0:
                m_["flow"] = 0
                self.program_pause()
            elif m == 1:
                pass
            elif m in (2, 30):
                self.sync()
                with self.lock:
                    m_["coord"] = 54
                    m_["plane"] = 17
                    m_["distance"] = 90
                    m_["feedmode"] = 94
                    m_["spindle"] = 5
                    m_["coolant"] = 9
                    m_["motion"] = 1.0
                    m_["flow"] = 0
                    self.spindle_rpm = 0.0
                    self.force_wco = True
                self.send("[MSG:Pgm End]")

    def program_pause(self):
        self.sync()
        with self.lock:
            self.hold = True
        self.wait_until(lambda: not self.hold)

    def compute_target(self, axis_words, dist_mode, g53):
        with self.lock:
            target = list(self.plan_pos)
            wco = self.wco_total()
        for i, a in enumerate(AX):
            if a in axis_words:
                v = axis_words[a]
                if dist_mode == 91:
                    target[i] = target[i] + v
                elif g53:
                    target[i] = v
                else:
                    target[i] = v + wco[i]
        return target

    def within_travel(self, target):
        for i, k in enumerate(("130", "131", "132")):
            travel = float(self.settings.get(k, "1000"))
            if target[i] > 0.0005 or target[i] < -travel - 0.0005:
                return False
        return True

    def soft_limit_check(self, target):
        if self.settings.get("20") == "1" and not self.within_travel(target):
            self.sync()
            with self.lock:
                self.alarm = True
                self.critical = True
                self.hold = False
            self.send("ALARM:2")
            self.send("[MSG:Reset to continue]")
            raise Aborted()

    def probe(self, target, feed, mode, line):
        toward = mode in (38.2, 38.3)
        no_error = mode in (38.3, 38.5)
        self.sync()
        with self.lock:
            initial = self.probe_triggered()
            if initial == toward:
                self.alarm = True
                self.send("ALARM:4")
                return
        gen = self.cur_gen
        res = self.run_motion(Item("probe_toward" if toward else "probe_away", target, feed, line), gen)
        with self.lock:
            self.motion_state = "Idle"
            self.feed_actual = 0.0
            self.plan_pos = list(self.mpos)
            if res == "abort":
                raise Aborted()
            hit = res == "probe"
            self.prb = list(self.mpos)
            self.prb_ok = 1 if hit else 0
            if not hit and not no_error:
                self.alarm = True
                self.send("ALARM:5")
            self.send("[PRB:%s:%d]" % (fmt3(self.prb), self.prb_ok))


def main():
    p = argparse.ArgumentParser(description="GRBL 1.1h simulator on a pty")
    p.add_argument("--speed", type=float, default=1.0, help="simulation time multiplier (10 = 10x faster than real)")
    p.add_argument("--rapid", type=float, default=5000.0, help="rapid (G0) rate in mm/min")
    p.add_argument("--require-homing", action="store_true", help="start in Alarm until $H / $X (like $22=1 + HOMING_INIT_LOCK)")
    p.add_argument("--strict", action="store_true", help="soft reset mid-motion -> ALARM:3 (ALARM:6 while homing) and Alarm state persists across reset")
    p.add_argument("--no-ack-comments", action="store_true", help="swallow lines starting with '(' without any response")
    p.add_argument("--merge-crlf", action="store_true", help="treat CR+LF as one line terminator (stock GRBL answers an extra 'ok' for the empty line)")
    p.add_argument("--bitsetter", type=float, nargs=2, default=[-20.0, -20.0], metavar=("X", "Y"), help="BitSetter machine XY")
    p.add_argument("--bitsetter-z", type=float, default=-60.0, help="BitSetter surface machine Z (with tool delta 0)")
    p.add_argument("--bitsetter-radius", type=float, default=10.0, help="BitSetter XY tolerance radius")
    p.add_argument("--tool-delta", type=float, default=0.0, help="initial extra tool length (mm); change at runtime with 'tool <delta>' on stdin")
    p.add_argument("--m6", choices=("error", "ok", "hold"), default="error", help="M6 handling: error:20 (stock), plain ok, or pause like M0")
    p.add_argument("--link", metavar="PATH", help="create a symlink PATH -> the pty slave (removed on exit)")
    p.add_argument("--quiet", action="store_true", help="do not log RX/TX to stderr")
    a = p.parse_args()

    sim = Sim(a)
    if a.link:
        try:
            os.unlink(a.link)
        except FileNotFoundError:
            pass
        os.symlink(sim.port, a.link)
    print("PORT=%s" % sim.port, flush=True)
    if a.link:
        print("LINK=%s" % a.link, flush=True)
    sys.stderr.write("## grblsim: %s  speed=%gx  bitsetter=(%g,%g) z=%g r=%g  tool-delta=%g\n" % (
        sim.port, a.speed, a.bitsetter[0], a.bitsetter[1], a.bitsetter_z, a.bitsetter_radius, a.tool_delta))
    sys.stderr.flush()

    done = threading.Event()

    def on_sig(signum, frame):
        done.set()

    signal.signal(signal.SIGINT, on_sig)
    signal.signal(signal.SIGTERM, on_sig)
    sim.start()
    try:
        while not done.is_set() and not sim.stop:
            done.wait(0.2)
    finally:
        sim.stop = True
        if a.link:
            try:
                os.unlink(a.link)
            except OSError:
                pass
        for fd in (sim.master, sim.slave):
            try:
                os.close(fd)
            except OSError:
                pass
        sys.stderr.write("## grblsim: bye\n")


if __name__ == "__main__":
    main()
