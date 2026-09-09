#!/system/bin/sh
# A/B on the big cluster's frequency ceiling.
#
# The captured "factory" snapshot on this device records cpu_policy4_max=2208000 while the
# hardware reports 2400000 and the residency table shows the cluster has run at 2400000 before.
# The snapshot was taken under temp-root after other tuners had run, so it is not proof of
# factory state -- and the module has been faithfully restoring that cap ever since.
#
# Alternating ABBA rather than running two long blocks: even in a fixed Roblox place the scene
# is not identical minute to minute, and ABBA cancels linear thermal drift inside each quad.
#
# The original ceiling is restored on exit, including on interrupt.
set -u
D=/data/adb/m54tuner
P=/sys/devices/system/cpu/cpufreq/policy4
LOG=/data/local/tmp/ceiling.log
PERIOD=${1:-60}
QUADS=${2:-2}

ORIGINAL=$(cat "$P/scaling_max_freq")
HARDWARE=$(cat "$P/cpuinfo_max_freq")
restore() {
  echo "$ORIGINAL" > "$P/scaling_max_freq" 2>/dev/null
  echo "# restored ceiling to $(cat "$P/scaling_max_freq") at $(date +%T)" >> "$LOG"
}
trap restore EXIT INT TERM

: > "$LOG"
echo "# ceiling A/B original=$ORIGINAL hardware=$HARDWARE period=${PERIOD}s quads=$QUADS start=$(date +%T)" >> "$LOG"

block() {
  want=$1
  echo "$want" > "$P/scaling_max_freq" 2>/dev/null
  got=$(cat "$P/scaling_max_freq")
  echo "# block want=$want got=$got at=$(date +%T)" >> "$LOG"
  [ "$got" != "$want" ] && { echo "# kernel refused the ceiling; aborting" >> "$LOG"; return 1; }
  until=$(( $(date +%s) + PERIOD ))
  skipped=0
  while [ "$(date +%s)" -lt "$until" ]; do
    sleep 2
    # Discard the first windows after a ceiling change: the governor needs a moment, and the
    # window straddling the switch measures the handover rather than either setting.
    if [ "$skipped" -lt 4 ]; then skipped=$((skipped + 1)); continue; fi
    printf '%s ' "$got" >> "$LOG"
    grep -E "^(at|app|cadence|frames|p95_ms|jank|temp|cpu|cpu_peak|thread_peak|gpu|watts|action|queue_ms|queue_peak_ms|reason)=" \
      "$D/adaptive_status" | tr '\n' ' ' >> "$LOG"
    printf 'big_cur=%s ' "$(cat "$P/scaling_cur_freq")" >> "$LOG"
    echo >> "$LOG"
  done
  return 0
}

i=0
while [ "$i" -lt "$QUADS" ]; do
  block "$ORIGINAL" || break
  block "$HARDWARE" || break
  block "$HARDWARE" || break
  block "$ORIGINAL" || break
  i=$((i + 1))
  echo "# quad $i done $(date +%T)" >> "$LOG"
done
echo "# finished $(date +%T)" >> "$LOG"
