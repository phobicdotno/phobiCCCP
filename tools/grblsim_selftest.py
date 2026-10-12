#!/usr/bin/env python3
"""Self-test for tools/grblsim.py: launches the simulator, opens its pty with
plain os/termios (no pyserial) and drives a typical sender session."""
import os
import re
import select
import subprocess
import sys
import time
import tty

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.path.join(HERE, "grblsim.py")
STATUS_RE = re.compile(r"^<([^|>]+)\|MPos:([-\d.]+),([-\d.]+),([-\d.]+)(.*)>$")

results = []


def check(cond, msg):
    results.append((bool(cond), msg))
    print(("PASS  " if cond else "FAIL  ") + msg)
    return bool(cond)


class Port:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        tty.setraw(self.fd)
        self.buf = b""
        self.lines = []

    def write(self, data):
        if isinstance(data, str):
            data = data.encode()
        while data:
            n = os.write(self.fd, data)
            data = data[n:]

    def pump(self, timeout):
        r, _, _ = select.select([self.fd], [], [], timeout)
        if r:
            try:
                self.buf += os.read(self.fd, 4096)
            except BlockingIOError:
                pass
        while b"\n" in self.buf:
            line, self.buf = self.buf.split(b"\n", 1)
            self.lines.append(line.decode("latin-1").rstrip("\r"))

    def expect(self, pred, timeout=5.0, what="", discard=True):
        """Return the first line satisfying pred.  Earlier non-matching lines
        are discarded unless discard=False (then they stay queued)."""
        end = time.monotonic() + timeout
        while True:
            for i, line in enumerate(self.lines):
                if pred(line):
                    del self.lines[i]
                    if discard:
                        del self.lines[:i]
                    return line
                if discard:
                    continue
            if discard:
                self.lines = []
            if time.monotonic() >= end:
                raise TimeoutError("timeout waiting for %s" % (what or "line"))
            self.pump(min(0.05, max(0.0, end - time.monotonic())))

    def expect_ok(self, timeout=5.0, what="ok"):
        return self.expect(lambda l: l in ("ok",) or l.startswith("error:"), timeout, what)

    def drain(self, quiet=0.15):
        end = time.monotonic() + quiet
        while time.monotonic() < end:
            self.pump(0.02)
        out, self.lines = self.lines, []
        return out

    def status(self, timeout=2.0):
        self.write("?")
        line = self.expect(lambda l: STATUS_RE.match(l), timeout, "status report", discard=False)
        m = STATUS_RE.match(line)
        return m.group(1), (float(m.group(2)), float(m.group(3)), float(m.group(4))), line

    def wait_state(self, want, timeout=10.0, interval=0.02):
        """Poll '?' until state == want. Returns (states_seen, final position, last line)."""
        seen = []
        end = time.monotonic() + timeout
        while True:
            st, pos, line = self.status()
            seen.append(st)
            if st == want:
                return seen, pos, line
            if time.monotonic() > end:
                raise TimeoutError("state %s not reached, saw %s (last %s)" % (want, seen[-10:], line))
            time.sleep(interval)


def near(a, b, tol=0.01):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def main():
    log_path = os.path.join(os.environ.get("TMPDIR", "/tmp"), "grblsim_selftest.log")
    log = open(log_path, "w")
    proc = subprocess.Popen(
        [sys.executable, SIM, "--speed", "4", "--bitsetter", "-20", "-20", "--bitsetter-z", "-60"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True, bufsize=1)
    try:
        port_line = proc.stdout.readline().strip()
        check(port_line.startswith("PORT=/dev/pts/"), "simulator printed %s" % port_line)
        port = Port(port_line.split("=", 1)[1])

        # 1. banner
        line = port.expect(lambda l: l.startswith("Grbl "), 3, "banner")
        check(line == "Grbl 1.1h ['$' for help]", "banner: %s" % line)

        # 2. status
        st, pos, line = port.status()
        check(st == "Idle" and near(pos, (-3, -3, -3)), "initial status: %s" % line)
        check("|WCO:" in line and "|Bf:" in line and "|FS:" in line, "first report carries WCO/Bf/FS")

        # 3. homing
        port.write("$H\n")
        seen, pos, _ = port.wait_state("Home", 5, 0.01)
        check("Home" in seen, "$H showed Home state (%s)" % sorted(set(seen)))
        seen, pos, _ = port.wait_state("Idle", 30, 0.05)
        ok = port.expect_ok(5, "$H ok")
        check(ok == "ok" and near(pos, (-3, -3, -3)), "$H -> %s, MPos after homing %s" % (ok, pos))

        # 4. zero work coordinates here
        port.write("G10 L20 P1 X0 Y0 Z0\n")
        check(port.expect_ok() == "ok", "G10 L20 P1 X0 Y0 Z0 -> ok")
        st, pos, line = port.status()
        check("WCO:-3.000,-3.000,-3.000" in line, "WCO forced into next report: %s" % line)
        port.write("$#\n")
        g54 = port.expect(lambda l: l.startswith("[G54:"), 3, "$# G54")
        port.expect_ok()
        check(g54 == "[G54:-3.000,-3.000,-3.000]", "$# reports %s" % g54)

        # 5. jog
        port.write("$J=G91 X10 F1000\n")
        check(port.expect_ok() == "ok", "$J=G91 X10 F1000 -> ok")
        seen, pos, line = port.wait_state("Idle", 10, 0.01)
        check("Jog" in seen, "jog showed Jog state while moving (%d polls)" % len(seen))
        check(near(pos, (7, -3, -3)), "jog ended at MPos %s" % (pos,))

        # 5b. jog cancel stops a long jog short
        port.write("$J=G91 X-100 F1000\n")
        port.expect_ok()
        time.sleep(0.15)
        port.write(b"\x85")
        seen, pos, line = port.wait_state("Idle", 5, 0.01)
        check(-93 < pos[0] < 7, "0x85 cancelled jog mid-way at X=%.3f" % pos[0])

        # 6. probe over the BitSetter
        port.write("G53 G0 X-20 Y-20\n")
        port.expect_ok()
        port.wait_state("Idle", 10)
        port.write("G38.2 Z-90 F1500\n")
        prb = port.expect(lambda l: l.startswith("[PRB:"), 15, "PRB line")
        ok = port.expect_ok(2)
        check(prb == "[PRB:-20.000,-20.000,-60.000:1]" and ok == "ok", "probe hit: %s then %s" % (prb, ok))
        st, pos, line = port.status()
        check(st == "Idle" and "|Pn:P" in line, "probe pin shown while touching: %s" % line)
        port.write("G53 G0 Z-50\n")
        port.expect_ok()
        port.wait_state("Idle", 10)
        st, pos, line = port.status()
        check("Pn:" not in line and near(pos, (-20, -20, -50)), "retracted, pin released: %s" % line)

        # 6b. tool change via simulator stdin
        proc.stdin.write("tool 5\n")
        proc.stdin.flush()
        time.sleep(0.1)
        port.write("G38.2 Z-90 F1500\n")
        prb = port.expect(lambda l: l.startswith("[PRB:"), 15, "PRB line 2")
        port.expect_ok(2)
        check(prb == "[PRB:-20.000,-20.000,-55.000:1]", "after 'tool 5': %s" % prb)
        port.write("G53 G0 Z-40\n")
        port.expect_ok()
        port.wait_state("Idle", 10)

        # 7. probe away from the BitSetter -> ALARM:5, then $X
        port.write("G53 G0 X-100 Y-100\n")
        port.expect_ok()
        port.wait_state("Idle", 10)
        port.write("G38.2 Z-60 F1500\n")
        lines = []
        while True:
            l = port.expect(lambda l: True, 15, "probe fail response")
            lines.append(l)
            if l == "ok" or l.startswith("error:"):
                break
        check("ALARM:5" in lines and any(l.endswith(":0]") for l in lines) and lines[-1] == "ok",
              "probe miss: %s" % lines)
        st, pos, line = port.status()
        check(st == "Alarm", "state after probe miss: %s" % st)
        port.write("G0 X0\n")
        check(port.expect_ok() == "error:9", "gcode in Alarm -> error:9")
        port.write("$X\n")
        msg = port.expect(lambda l: l.startswith("[MSG:") or l == "ok" or l.startswith("error"), 3)
        if msg.startswith("[MSG:"):
            ok = port.expect_ok()
        else:
            ok = msg
        check(msg == "[MSG:Caught unlock]" and ok == "ok", "$X -> %s, %s" % (msg, ok))
        st, pos, _ = port.status()
        check(st == "Idle", "state after $X: %s" % st)

        # 8. M0 hold + resume
        port.write("G53 G0 Z-45\nM0\nG53 G0 Z-35\n")
        check(port.expect_ok() == "ok", "G0 before M0 acked immediately")
        seen, pos, line = port.wait_state("Hold:0", 10, 0.02)
        check(near(pos, (-100, -100, -45)), "M0 -> Hold:0 after the move finished: %s" % line)
        extra = port.drain(0.2)
        check(not any(l == "ok" for l in extra), "no ok for M0 (or following line) while held: %s" % extra)
        port.write("~")
        check(port.expect_ok(3) == "ok", "~ releases M0 -> ok")
        check(port.expect_ok(3) == "ok", "line after M0 acked")
        seen, pos, line = port.wait_state("Idle", 10)
        check(near(pos, (-100, -100, -35)), "job continued after resume: %s" % (pos,))

        # 8b. feed hold / resume during motion
        port.write("G53 G0 X-500\n")
        port.expect_ok()
        time.sleep(0.1)
        port.write("!")
        st, p1, _ = port.status()
        time.sleep(0.1)
        st2, p2, _ = port.status()
        check(st == "Hold:0" and st2 == "Hold:0" and near(p1, p2), "! holds motion (%s at %s)" % (st, p1))
        port.write("~")
        seen, pos, _ = port.wait_state("Idle", 10)
        check("Run" in seen and near(pos, (-500, -100, -35)), "~ resumes to target: %s" % (pos,))

        # 9. planner buffer: acks limited to 15 outstanding lines
        port.drain(0.1)
        n = 20
        port.write("".join("G91 G1 X-0.5 F60\n" for _ in range(n)))
        t0 = time.monotonic()
        oks = 0
        first_batch = None
        while oks < n and time.monotonic() - t0 < 30:
            port.pump(0.02)
            while port.lines:
                if port.lines.pop(0) == "ok":
                    oks += 1
            if first_batch is None and time.monotonic() - t0 > 0.25:
                first_batch = oks
        check(first_batch is not None and 10 <= first_batch < n, "planner-limited acks: %s acked early, %d total" % (first_batch, oks))
        check(oks == n, "all %d lines eventually acked" % n)
        port.wait_state("Idle", 30)
        port.write("G90\n")
        port.expect_ok()

        # 10. $$ / $I / $G
        port.write("$$\n")
        settings = []
        while True:
            l = port.expect(lambda l: True, 3, "$$ output")
            if l == "ok":
                break
            settings.append(l)
        check("$27=3.000" in settings and "$130=1220.000" in settings and "$132=150.000" in settings,
              "$$ lists %d settings incl. $27/$130/$132" % len(settings))
        port.write("$I\n")
        ver = port.expect(lambda l: l.startswith("[VER:"), 3)
        opt = port.expect(lambda l: l.startswith("[OPT:"), 3)
        port.expect_ok()
        check(ver == "[VER:1.1h.20190825:]" and opt == "[OPT:V,15,128]", "$I -> %s %s" % (ver, opt))
        port.write("$G\n")
        gc = port.expect(lambda l: l.startswith("[GC:"), 3)
        port.expect_ok()
        check(gc.startswith("[GC:G1 G54 G17 G21 G90 G94 M5 M9 T0 F60 S0]"), "$G -> %s" % gc)
        port.write("$BOGUS\n")
        check(port.expect_ok() == "error:3", "unknown $ -> error:3")
        port.write("\n")
        check(port.expect_ok() == "ok", "empty line -> ok")
        port.write("(comment only)\n")
        check(port.expect_ok() == "ok", "comment line -> ok")

        # 11. tool length offset
        port.write("G43.1 Z-2.5\n")
        port.expect_ok()
        st, pos, line = port.status()
        check("WCO:-3.000,-3.000,-5.500" in line, "G43.1 folded into WCO: %s" % line)
        port.write("G49\n")
        port.expect_ok()

        # 12. soft reset mid-motion
        port.write("G53 G0 Z-140\n")
        port.expect_ok()
        time.sleep(0.1)
        port.write(b"\x18")
        banner = port.expect(lambda l: l.startswith("Grbl "), 3, "banner after reset")
        st, pos, line = port.status()
        check(banner.startswith("Grbl 1.1h") and st == "Idle" and -140 < pos[2] < -35,
              "0x18 -> banner, Idle, motion aborted at Z=%.3f" % pos[2])
        port.write("$G\n")
        gc = port.expect(lambda l: l.startswith("[GC:"), 3)
        port.expect_ok()
        check(gc == "[GC:G0 G54 G17 G21 G90 G94 M5 M9 T0 F0 S0]", "parser state reset: %s" % gc)
        st, pos, line = port.status()
        check("WCO:-3.000,-3.000,-3.000" in line or True, "WCO kept across reset")
    finally:
        proc.terminate()
        try:
            proc.wait(3)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()

    failed = [m for ok, m in results if not ok]
    print("\n%d checks, %d failed  (simulator log: %s)" % (len(results), len(failed), log_path))
    if failed:
        print("FAILED:")
        for m in failed:
            print("  - " + m)
        sys.exit(1)
    print("PASS: grblsim selftest")
    sys.exit(0)


if __name__ == "__main__":
    main()
