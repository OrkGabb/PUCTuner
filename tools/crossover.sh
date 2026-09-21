#!/system/bin/sh
# Interleaved crossover measurement.
#
# In an open-world game the scene moves the metric more than any tuning does: a teleport, a
# menu, or simply pointing the camera at the sky changes the frame rate. That makes two long
# blocks -- observe for ten minutes, then active for ten -- uncomparable, because whatever
# the player happened to be doing is confounded with the arm.
#
# Alternating reduces that bias; it does not guarantee identical scenes or eliminate
# carryover. Blocks run ABBA to balance linear time drift within each quad, and the order flips
# every quad (observe-active-active-observe, then active-observe-observe-active). With a fixed
# order and a quad as long as one lap of a looped route, the active arm fell on the same part of
# the route every time and scene and arm were confounded (2026-09-21, NTE). Pick a block length
# that does not divide the lap.
#
#   crossover.sh [seconds_per_block] [quads]
set -eu
D=/data/adb/m54tuner
LOG=/data/local/tmp/crossover.log
PERIOD=${1:-60}
QUADS=${2:-5}

set_mode() {
  sed -i "s/^adaptive_mode=.*/adaptive_mode=$1/" "$D/config"
}

# Samples faster than the 6 s decision window and lets the analysis de-duplicate by the
# engine's own `at=` timestamp, so no window is counted twice or missed at a block boundary.
record() {
  arm=$1
  until=$(( $(date +%s) + PERIOD ))
  skipped=0
  while [ "$(date +%s)" -lt "$until" ]; do
    sleep 2
    # The window straddling a mode change belongs to neither arm: the controller restores its
    # writes on the transition, so that window measures the handover and not either setting.
    if [ "$skipped" -lt 4 ]; then skipped=$((skipped + 1)); continue; fi
    printf '%s ' "$arm" >> "$LOG"
    grep -E "^(at|app|cadence|frames|frame_time_ms|slow_frames_50ms|p95_ms|jank|temp|battery_temp|cpu|gpu|watts|action|measured_action|windows|samples|reason|credit|regime|deficit|deficit_stall|can_learn|can_control|passive_windows)=" \
      "$D/adaptive_status" | tr '\n' ' ' >> "$LOG"
    echo >> "$LOG"
  done
}

: > "$LOG"
echo "# crossover period=${PERIOD}s quads=${QUADS} started=$(date +%T)" >> "$LOG"
i=0
while [ "$i" -lt "$QUADS" ]; do
  if [ $((i % 2)) = 0 ]; then first=observe; second=active; else first=active; second=observe; fi
  set_mode "$first";  record "$first"
  set_mode "$second"; record "$second"
  set_mode "$second"; record "$second"
  set_mode "$first";  record "$first"
  i=$((i + 1))
  echo "# quad $i done $(date +%T)" >> "$LOG"
done
set_mode active
echo "# crossover finished $(date +%T)" >> "$LOG"
