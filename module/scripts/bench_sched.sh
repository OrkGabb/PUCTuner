#!/system/bin/sh
# M54 Tuner — controlled PELT x EAS frame-pacing experiment.
#
# Tests all six combinations in alternating order and pools repeated samples. It does NOT install
# the winner: EAS-off may win one scene while costing heat and battery elsewhere. The exact live
# values found at start are restored on every normal exit, Ctrl-C and TERM.
#
# Usage: sh bench_sched.sh [seconds_per_step] [rounds] [min_frames]
# Defaults: 45 seconds, 2 rounds, 400 frames (about 9 minutes total).
# Output: /data/adb/m54tuner/bench_sched.csv
# State:  /data/adb/m54tuner/bench_sched_state

DIR=$(dirname "$0")
. "$DIR/lib.sh"

STEP=${1:-45}
ROUNDS=${2:-2}
MIN_FRAMES=${3:-400}
CSV="$M54_DIR/bench_sched.csv"
STATE="$M54_DIR/bench_sched_state"
PELT_NODE=/proc/sys/kernel/sched_pelt_multiplier
EAS_NODE=/proc/sys/kernel/sched_energy_aware
ORIG_FILE="$M54_DIR/bench_sched_original"
ACK="$M54_DIR/bench_sched_restore_ok"

case "$STEP" in ''|*[!0-9]*) STEP=45;; esac
case "$ROUNDS" in ''|*[!0-9]*) ROUNDS=2;; esac
case "$MIN_FRAMES" in ''|*[!0-9]*) MIN_FRAMES=400;; esac
[ "$STEP" -lt 15 ] && STEP=15
[ "$ROUNDS" -lt 2 ] && ROUNDS=2

[ -w "$PELT_NODE" ] || { echo "unsupported|pelt" > "$STATE"; exit 1; }
[ -w "$EAS_NODE" ] || { echo "unsupported|eas" > "$STATE"; exit 1; }

MAIN_PID=$$
if pid_record_alive "$M54_DIR/bench_sched_pid" bench_sched.sh; then
  echo "benchmark already running"
  exit 1
fi
rm -f "$M54_DIR/bench_sched_pid"
rm -f "$ACK"
pid_record_write "$M54_DIR/bench_sched_pid" bench_sched.sh || { echo "benchmark PID publication failed"; exit 1; }
if ! sh "$DIR/adaptive_stop.sh"; then
  rm -f "$M54_DIR/bench_sched_pid"
  exit 1
fi

ORIG_PELT=$(cat "$PELT_NODE" 2>/dev/null)
ORIG_EAS=$(cat "$EAS_NODE" 2>/dev/null)
case "$ORIG_PELT:$ORIG_EAS" in 1:0|1:1|2:0|2:1|4:0|4:1) ;;
  *) rm -f "$M54_DIR/bench_sched_pid"; sh "$DIR/adaptive_start.sh" >/dev/null 2>&1; exit 1;; esac
printf '%s\n%s\n' "$ORIG_PELT" "$ORIG_EAS" | atomic_write "$ORIG_FILE" 0600 || {
  rm -f "$M54_DIR/bench_sched_pid"
  sh "$DIR/adaptive_start.sh" >/dev/null 2>&1
  exit 1
}

cleanup() {
  [ "$$" = "$MAIN_PID" ] || return 0
  local rc=0
  dumpsys SurfaceFlinger --timestats -disable >/dev/null 2>&1 || rc=1
  sh "$DIR/apply_profile.sh" >/dev/null 2>&1 || rc=1
  case "$ORIG_PELT" in 1|2|4) echo "$ORIG_PELT" > "$PELT_NODE" 2>/dev/null || rc=1;; *) rc=1;; esac
  case "$ORIG_EAS" in 0|1) echo "$ORIG_EAS" > "$EAS_NODE" 2>/dev/null || rc=1;; *) rc=1;; esac
  [ "$(cat "$PELT_NODE" 2>/dev/null)" = "$ORIG_PELT" ] || rc=1
  [ "$(cat "$EAS_NODE" 2>/dev/null)" = "$ORIG_EAS" ] || rc=1
  if [ "$rc" = 0 ]; then
    rm -f "$M54_DIR/bench_sched_pid" "$ORIG_FILE"
    if sh "$DIR/adaptive_start.sh" >/dev/null 2>&1; then touch "$ACK" || rc=1
    else rc=1; fi
  fi
  [ "$rc" = 0 ] || echo "restore_failed" > "$STATE"
  return "$rc"
}
trap cleanup EXIT INT TERM

stutters() {
  echo "$1" | tr ' ' '\n' | grep 'ms=' | sed 's/ms=/ /' | awk '
    { ms[NR] = $1 + 0; c[NR] = $2 + 0; total += $2 }
    END {
      if (total == 0) { print "0 0 0"; exit }
      acc = 0
      for (i = 1; i <= NR; i++) { acc += c[i]; if (acc >= total / 2) { med = ms[i]; break } }
      if (med < 1) med = 1
      for (i = 1; i <= NR; i++) if (ms[i] >= med * 2) st += c[i]
      print total, med, st
    }'
}

max_temp_c() {
  for z in /sys/class/thermal/thermal_zone*; do
    [ -r "$z/temp" ] || continue
    ty=$(cat "$z/type" 2>/dev/null | tr 'A-Z' 'a-z')
    case "$ty" in *cpu*|*gpu*|big|little|g3d) cat "$z/temp";; esac
  done | awk 'BEGIN { m=0 } { v=$1+0; if (v>m) m=v } END { printf "%.1f", (m>1000 ? m/1000 : m) }'
}

step_run() {
  local round="$1" pelt="$2" eas="$3" t0 d hist parsed total med st score t1
  M54_BENCH_PELT="$pelt" M54_BENCH_EAS="$eas" \
    sh "$DIR/apply_profile.sh" >/dev/null 2>&1
  if [ "$(cat "$PELT_NODE" 2>/dev/null)" != "$pelt" ] || \
     [ "$(cat "$EAS_NODE" 2>/dev/null)" != "$eas" ]; then
    log "bench sched r$round: kernel rejected pelt=$pelt eas=$eas"
    echo "$round,$pelt,$eas,0,0,0,-1,0,0,rejected" >> "$CSV"
    return
  fi

  t0=$(max_temp_c)
  dumpsys SurfaceFlinger --timestats -clear >/dev/null 2>&1
  dumpsys SurfaceFlinger --timestats -enable >/dev/null 2>&1
  echo "running|r$round|pelt=$pelt|eas=$eas|${STEP}s" > "$STATE"
  sleep "$STEP"
  d=$(dumpsys SurfaceFlinger --timestats -dump 2>/dev/null)
  hist=$(echo "$d" | grep -A1 'presentToPresent histogram' | tail -1)
  dumpsys SurfaceFlinger --timestats -disable >/dev/null 2>&1
  parsed=$(stutters "$hist")
  total=$(echo "$parsed" | awk '{print $1}')
  med=$(echo "$parsed" | awk '{print $2}')
  st=$(echo "$parsed" | awk '{print $3}')
  t1=$(max_temp_c)
  case "$total" in ''|*[!0-9]*) total=0;; esac
  if [ "$total" -ge "$MIN_FRAMES" ]; then score=$((st * 1000 / total)); note=ok
  else score=-1; note=too_few_frames; fi
  echo "$round,$pelt,$eas,$total,$med,$st,$score,$t0,$t1,$note" >> "$CSV"
  log "bench sched r$round pelt=$pelt eas=$eas frames=$total med=${med}ms stutter=$score/mil temp=${t0}-${t1}C"
}

winner() {
  awk -F, '$7 >= 0 { k=$2 "," $3; total[k]+=$4; st[k]+=$6 }
    END { for (k in total) { r=st[k]*1000/total[k]; if (best=="" || r<best) { best=r; win=k } }
          if (win!="") print win "," int(best) }' "$CSV"
}

spread() {
  awk -F, '$7 >= 0 { k=$2 "," $3; total[k]+=$4; st[k]+=$6 }
    END { for (k in total) { r=st[k]*1000/total[k]; if (lo=="" || r<lo) lo=r; if (r>hi) hi=r; n++ }
          if (n<2) print 0; else printf "%d", (lo>0 ? (hi-lo)*100/lo : 0) }' "$CSV"
}

: > "$CSV"
echo "round,pelt,eas,frames,median_ms,stutters,stutters_per_mille,temp_start_c,temp_end_c,note" >> "$CSV"
log "=== bench sched start: PELT 1/2/4 x EAS 0/1, ${STEP}s x ${ROUNDS} rounds ==="
r=1
while [ "$r" -le "$ROUNDS" ]; do
  if [ $((r % 2)) = 1 ]; then ORDER="1:1 2:0 4:1 1:0 2:1 4:0"
  else ORDER="4:0 2:1 1:0 4:1 2:0 1:1"; fi
  for pair in $ORDER; do step_run "$r" "${pair%%:*}" "${pair#*:}"; done
  r=$((r + 1))
done

WIN=$(winner)
SPREAD=$(spread)
MIN_SPREAD=$(clamp_int "$(read_cfg bench_min_spread_pct 5)" 1 50 5)
if [ -z "$WIN" ] || [ "$SPREAD" -lt "$MIN_SPREAD" ]; then
  echo "done|inconclusive|spread=${SPREAD}%" > "$STATE"
  log "=== bench sched inconclusive: spread ${SPREAD}% (< ${MIN_SPREAD}%) ==="
else
  BEST_PELT=$(echo "$WIN" | cut -d, -f1)
  BEST_EAS=$(echo "$WIN" | cut -d, -f2)
  BEST_SCORE=$(echo "$WIN" | cut -d, -f3)
  echo "done|pelt=$BEST_PELT|eas=$BEST_EAS|score=$BEST_SCORE/mille|spread=${SPREAD}%" > "$STATE"
  log "=== bench sched frame-time winner: pelt=$BEST_PELT eas=$BEST_EAS score=$BEST_SCORE/mille spread=${SPREAD}% ==="
fi
