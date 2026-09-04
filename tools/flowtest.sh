#!/bin/bash
# End-to-end machine-control test against tools/grblsim.py (no hardware):
#   1. --grbl-probe: BitSetter measurement macro (fast + slow G38.2)
#   2. --grbl-run:  program with two tool changes; the simulated second tool
#                   is 5 mm longer, so the streamer must apply G43.1 Z5.000.
# Usage: tools/flowtest.sh [path/to/phobicccp]   (default ./build/phobicccp)
set -u
cd "$(dirname "$0")/.."
BIN=${1:-./build/phobicccp}
T=$(mktemp -d); FIFO=$T/sim.stdin; mkfifo $FIFO
python3 tools/grblsim.py --speed 20 --bitsetter -20 -20 --bitsetter-z -60 --quiet < $FIFO > $T/sim.out 2> $T/sim.err &
SIM=$!; exec 3>$FIFO
for i in $(seq 1 50); do PORT=$(grep -m1 '^PORT=' $T/sim.out | cut -d= -f2); [ -n "$PORT" ] && break; sleep 0.1; done
[ -n "$PORT" ] || { echo "FAIL: simulator did not start"; kill $SIM; exit 1; }
fail=0
echo "## probe on $PORT"
timeout 60 $BIN --grbl-probe $PORT -20 -20 -5 > $T/probe.log 2>&1
grep -q 'PROBE_OK contactZ=-60.000' $T/probe.log && echo "PASS probe contact at -60.000" || { echo "FAIL probe"; tail -5 $T/probe.log; fail=1; }
echo "## program with two tool changes"
printf '%s\n' G90 G21 'M0 ;T601' M03S1000 G0X10Y10Z2.54 G1Z-1F200 X20 G0Z2.54 M05 'M0 ;T602' M03S1000 G0X30Y30Z2.54 G1Z-1F200 X40 G0Z2.54 M05 M02 > $T/twotools.nc
timeout 120 $BIN --grbl-run $PORT $T/twotools.nc -20 -20 > $T/run.log 2>&1 &
RUN=$!
for i in $(seq 1 600); do grep -q 'TOOLCHANGE T602' $T/run.log && break; sleep 0.1; done
echo "tool 5" >&3
wait $RUN
grep -q 'REF contactZ=-60.000' $T/run.log && echo "PASS reference tool measured" || { echo "FAIL reference"; fail=1; }
grep -q 'TLO 5.000' $T/run.log && echo "PASS second tool offset +5.000 applied" || { echo "FAIL offset"; fail=1; }
grep -q 'RUN_OK toolchanges=2 tlo=5.000' $T/run.log && echo "PASS program finished" || { echo "FAIL run"; tail -8 $T/run.log; fail=1; }
exec 3>&-; kill $SIM 2>/dev/null; wait $SIM 2>/dev/null
[ $fail -eq 0 ] && echo "PASS: machine flow test" || echo "FAIL: machine flow test (logs in $T)"
exit $fail
