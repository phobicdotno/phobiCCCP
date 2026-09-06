#!/bin/bash
# Streamer failure-path test against tools/grblsim.py (no hardware). Each case
# used to hang the sender for ever or carry on cutting after a refusal:
#   1. a line longer than the char-counting window
#   2. a line GRBL rejects with error: mid-program
#   3. a critical alarm (soft limit), which GRBL never acks and never reboots from
# Usage: tools/streamtest.sh [path/to/phobicccp]   (default ./build/phobicccp)
set -u
cd "$(dirname "$0")/.."
BIN=${1:-./build/phobicccp}
fail=0
T=$(mktemp -d)

SIM=0
PORT=""
start_sim() {   # sets PORT and SIM in this shell (no subshell: SIM must survive)
    : > $T/sim.out
    python3 tools/grblsim.py --speed 30 --quiet > $T/sim.out 2> $T/sim.err &
    SIM=$!
    PORT=""
    for i in $(seq 1 50); do
        PORT=$(grep -m1 '^PORT=' $T/sim.out | cut -d= -f2)
        [ -n "$PORT" ] && break
        sleep 0.1
    done
    [ -n "$PORT" ] || { echo "FAIL: simulator did not start"; exit 1; }
}
stop_sim() { kill $SIM 2>/dev/null; wait $SIM 2>/dev/null; }

# ---- 1. a line at/over the window (120 bytes on the wire) -------------------
python3 - "$T/long.nc" <<'PY'
import sys
base = 'G1X2.000Y2.000Z-1.000F100.0;'
line = base + 'A' * (120 - len(base))
assert len(line) == 120
open(sys.argv[1], 'w').write('G90\nG21\nG0X1Y1Z2.54\n' + line + '\nM02\n')
PY
start_sim
timeout 25 $BIN --grbl-run $PORT $T/long.nc > $T/long.log 2>&1
rc=$?
if [ $rc -eq 124 ]; then
    echo "FAIL over-long line hung the sender"; fail=1
else
    echo "PASS over-long line does not stall the stream (rc=$rc)"
fi
stop_sim

# ---- 2. a rejected line must stop the program, not cut on -------------------
printf 'G90\nG21\nG0X0Y0Z5\nG1Z-3F100\nG1X50F400\nG5Z5\nG0X0Y0\nM02\n' > $T/err.nc
start_sim
timeout 30 $BIN --grbl-run $PORT $T/err.nc > $T/err.log 2>&1
rc=$?
if [ $rc -eq 124 ]; then
    echo "FAIL rejected line hung the sender"; fail=1
elif grep -q 'program stopped' $T/err.log && grep -q 'RUN_FAILED' $T/err.log; then
    echo "PASS a rejected line stops the program"
else
    echo "FAIL rejected line did not stop the program"; tail -4 $T/err.log; fail=1
fi
stop_sim

# ---- 3. a critical alarm must end the stream, not wedge it ------------------
printf '$20=1\nG90\nG21\nG0X-10Y-10Z-5\nG0X-5000Y-5000\nG1X-10F200\nG0Z-1\nM02\n' > $T/alarm.nc
start_sim
timeout 30 $BIN --grbl-run $PORT $T/alarm.nc > $T/alarm.log 2>&1
rc=$?
if [ $rc -eq 124 ]; then
    echo "FAIL an alarm wedged the sender (no completion, UI would stay locked)"; fail=1
elif grep -q 'ALARM' $T/alarm.log && grep -q 'RUN_FAILED' $T/alarm.log; then
    echo "PASS a critical alarm ends the stream"
else
    echo "FAIL alarm was not reported as a stopped program"; tail -4 $T/alarm.log; fail=1
fi
stop_sim

[ $fail -eq 0 ] && echo "PASS: streamer failure-path test" || echo "FAIL: streamer failure-path test (logs in $T)"
exit $fail
