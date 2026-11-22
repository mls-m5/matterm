#!/bin/sh
# usage: measure.sh BIN SCRIPT [seconds] -> CPU % of one core over the window (3 s warm-up)
BIN=$1; SCRIPT=$2; SECS=${3:-8}
SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software $BIN --no-daemon -- $SCRIPT > /dev/null 2>&1 &
PID=$!
sleep ${WARM:-3}
a=$(awk '{print $14+$15}' /proc/$PID/stat)
sleep $SECS
b=$(awk '{print $14+$15}' /proc/$PID/stat)
kill $PID 2>/dev/null; wait $PID 2>/dev/null
echo "$(awk -v t=$((b-a)) -v s=$SECS 'BEGIN{printf "%.1f", t/s}') % of a core"
