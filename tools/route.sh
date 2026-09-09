#!/system/bin/sh
# Records one run of a repeatable in-game route.
#
# Free play cannot be compared run to run: a teleport, a menu, or pointing the camera at the
# sky moves the frame rate more than any tuning does. A fixed autopilot route removes all of
# that -- same camera path, same geometry, same asset streaming, no human input -- which makes
# two runs comparable if, and only if, they also start from a similar thermal state.
#
#   route.sh <label> [seconds]
#
# Run the same route under each setting, alternating the order (A B B A) so that whatever heat
# accumulates across the session does not land on one setting only.
set -eu
D=/data/adb/m54tuner
LABEL=${1:?usage: route.sh <label> [seconds]}
SECONDS_TO_RUN=${2:-180}
LOG=/data/local/tmp/route.log

start_temp=0
for z in /sys/class/thermal/thermal_zone*; do
  t=$(cat "$z/type" 2>/dev/null || echo)
  case "$t" in BIG|LITTLE|G3D)
    v=$(( $(cat "$z/temp") / 1000 ))
    [ "$v" -gt "$start_temp" ] && start_temp=$v
  ;; esac
done

{
  echo "# run label=$LABEL start=$(date +%T) start_temp=${start_temp}C"
  echo "# profile=$(sed -n 's/^profile=//p' "$D/config") mode=$(sed -n 's/^adaptive_mode=//p' "$D/config")"
  echo "# swappiness=$(cat /proc/sys/vm/swappiness) readahead=$(cat /sys/block/sda/queue/read_ahead_kb)"
  echo "# swapfree_mib=$(awk '/^SwapFree/{print int($2/1024)}' /proc/meminfo)"
} >> "$LOG"

pid=$(pgrep -f com.hottagames.nte | head -1 || echo)
faults0=0; reads0=0
if [ -n "$pid" ]; then
  faults0=$(awk '{print $12}' "/proc/$pid/stat")
  reads0=$(awk '/^read_bytes:/{print $2}' "/proc/$pid/io")
fi

until=$(( $(date +%s) + SECONDS_TO_RUN ))
while [ "$(date +%s)" -lt "$until" ]; do
  sleep 2
  printf '%s ' "$LABEL" >> "$LOG"
  grep -E "^(at|app|cadence|frames|p95_ms|jank|temp|battery_temp|cpu|gpu|watts|action|windows|samples|reason)=" \
    "$D/adaptive_status" | tr '\n' ' ' >> "$LOG"
  echo >> "$LOG"
done

if [ -n "$pid" ] && [ -d "/proc/$pid" ]; then
  faults1=$(awk '{print $12}' "/proc/$pid/stat")
  reads1=$(awk '/^read_bytes:/{print $2}' "/proc/$pid/io")
  echo "# majfaults=$((faults1 - faults0)) read_mib=$(( (reads1 - reads0) / 1048576 ))" >> "$LOG"
fi
echo "# run label=$LABEL end=$(date +%T)" >> "$LOG"
echo "run $LABEL recorded"
