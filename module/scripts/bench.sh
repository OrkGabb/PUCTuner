#!/system/bin/sh
# M54 Tuner — automatic sweep of the GPU timing knobs, scored from SurfaceFlinger TimeStats.
#
# Three controls with five choices each is 125 combinations; nobody finds a sweet spot in that by
# hand. The compositor already times every frame, so the device can rank the settings itself while
# you play. Two decisions carry the whole thing, and both come from a first run that FAILED to
# produce a usable answer:
#
# 1. THE METRIC. Measured live on Honkai Star Rail, the two obvious counters are useless here.
#    jankyFrames stayed at 0-4 for every setting. missedFrames sat at ~55% no matter what, which is
#    what you get when it counts panel refreshes that received no new frame: a 60 fps game on a
#    120 Hz panel misses half of them by construction, and that has nothing to do with stutter.
#    So the score comes from the presentToPresent histogram: take the MEDIAN interval (the game's
#    own cadence, whatever it happens to be) and count the intervals that ran at least twice that
#    long. Those are stutters under any definition, and the measure does not care whether the game
#    is targeting 60 or 120.
#
# 2. THE PROTOCOL. One pass per value cannot tell the setting apart from the scene: in the first
#    run js=25 scored 347 per mille while the game was loading and 543-568 later in combat — same
#    value, different moment. So candidates are INTERLEAVED and repeated. Every round tests all of
#    them in order and the rounds are pooled, so slow drift (a heavier scene, a warming phone) hits
#    every candidate about equally instead of landing on whichever happened to go first.
#
# Usage: sh bench.sh [seconds_per_step] [rounds] [min_frames]     defaults: 45 2 400
# Output: /data/adb/m54tuner/bench.csv     Progress: /data/adb/m54tuner/bench_state

DIR=$(dirname "$0")
. "$DIR/lib.sh"

STEP=${1:-45}
ROUNDS=${2:-2}
MIN_FRAMES=${3:-400}
CSV="$M54_DIR/bench.csv"
STATE="$M54_DIR/bench_state"
ACK="$M54_DIR/bench_restore_ok"

# Only the main script may restore. Learned the hard way: `pkill -f bench.sh` matches the subshells
# too, one of them ran this trap, put the config back and deleted the backup — and the surviving
# main process then wrote the sweep's values over it. The PID guard makes cleanup idempotent, and
# bench_pid gives anyone stopping the sweep a single correct target.
MAIN_PID=$$
if pid_record_alive "$M54_DIR/bench_pid" bench.sh; then echo "benchmark already running"; exit 1; fi
rm -f "$M54_DIR/bench_pid"
rm -f "$ACK"
pid_record_write "$M54_DIR/bench_pid" bench.sh || { echo "benchmark PID publication failed"; exit 1; }

cleanup() {
  [ "$$" = "$MAIN_PID" ] || return 0
  local rc=0
  sh "$DIR/apply_profile.sh" >/dev/null 2>&1 || rc=1
  dumpsys SurfaceFlinger --timestats -disable >/dev/null 2>&1 || rc=1
  if [ "$rc" = 0 ]; then
    rm -f "$M54_DIR/bench_pid"
    if sh "$DIR/adaptive_start.sh" >/dev/null 2>&1; then touch "$ACK" || rc=1
    else rc=1; fi
  fi
  if [ "$rc" = 0 ]; then echo "done" > "$STATE"
  else echo "restore_failed" > "$STATE"; fi
  return "$rc"
}
trap cleanup EXIT INT TERM

# stutters <histogram line> — echoes "total median stutter_count" from the presentToPresent bins.
# Bins arrive as "<ms>=<count>". The median bin is the game's cadence; 2x it is the stutter line.
stutters() {
  echo "$1" | tr ' ' '\n' | grep 'ms=' | sed 's/ms=/ /' | awk '
    { ms[NR] = $1 + 0; c[NR] = $2 + 0; total += $2 }
    END {
      if (total == 0) { print "0 0 0"; exit }
      acc = 0
      for (i = 1; i <= NR; i++) { acc += c[i]; if (acc >= total / 2) { med = ms[i]; break } }
      if (med < 1) med = 1
      thr = med * 2
      st = 0
      for (i = 1; i <= NR; i++) if (ms[i] >= thr) st += c[i]
      print total, med, st
    }'
}

# step_run <round> <phase> <dvfs> <polling> <js>
step_run() {
  local round="$1" phase="$2" dvfs="$3" pol="$4" js="$5"
  M54_BENCH_DVFS="$dvfs" M54_BENCH_POLLING="$pol" M54_BENCH_JS="$js" \
    sh "$DIR/apply_profile.sh" >/dev/null 2>&1

  dumpsys SurfaceFlinger --timestats -clear  >/dev/null 2>&1
  dumpsys SurfaceFlinger --timestats -enable >/dev/null 2>&1
  echo "$phase|r$round|$dvfs|$pol|$js|$STEP" > "$STATE"
  sleep "$STEP"

  local d hist parsed total med st score
  d=$(dumpsys SurfaceFlinger --timestats -dump 2>/dev/null)
  hist=$(echo "$d" | grep -A1 'presentToPresent histogram' | tail -1)
  dumpsys SurfaceFlinger --timestats -disable >/dev/null 2>&1

  parsed=$(stutters "$hist")
  total=$(echo "$parsed" | awk '{print $1}')
  med=$(echo "$parsed"   | awk '{print $2}')
  st=$(echo "$parsed"    | awk '{print $3}')

  case "$total" in ''|*[!0-9]*) total=0;; esac
  if [ "$total" -ge "$MIN_FRAMES" ]; then
    score=$(( st * 1000 / total ))
  else
    score=-1
    log "bench $phase r$round: apenas $total frames (< $MIN_FRAMES), passo descartado"
  fi
  echo "$round,$phase,$dvfs,$pol,$js,$total,$med,$st,$score" >> "$CSV"
  log "bench r$round $phase dvfs=$dvfs pol=$pol js=$js frames=$total mediana=${med}ms travadas=$st (${score}/mil)"
}

# winner <csv> <phase> <field 1..3> — pools every valid round per candidate and echoes the field of
# the one with the lowest stutter ratio. Pooling by summed counts weights longer samples more.
winner() {
  awk -F, -v p="$2" -v f="$3" '
    $2 == p && $9 >= 0 { key = $3 "," $4 "," $5; tot[key] += $6; stt[key] += $8 }
    END {
      for (k in tot) { r = stt[k] * 1000 / tot[k]; if (best == "" || r < best) { best = r; win = k } }
      if (win == "") exit
      split(win, a, ","); print a[f]
    }' "$1"
}

valid_candidates() {
  awk -F, -v p="$2" '$2 == p && $9 >= 0 { seen[$3 "," $4 "," $5] = 1 }
    END { n = 0; for (k in seen) n++; print n }' "$1"
}

# spread <csv> <phase> — percentage gap between the best and worst candidate. A few percent is scene
# noise, not a result, and the report says so instead of crowning someone.
spread() {
  awk -F, -v p="$2" '$2 == p && $9 >= 0 { k = $3 "," $4; t[k] += $6; s[k] += $8 }
    END {
      for (x in t) { r = s[x] * 1000 / t[x]; if (lo == "" || r < lo) lo = r; if (r > hi) hi = r }
      printf "%d", (lo > 0 ? (hi - lo) * 100 / lo : 0)
    }' "$1"
}

# ---------------------------------------------------------------------------
: > "$CSV"
echo "rodada,fase,dvfs_period,polling_speed,js_period,frames,mediana_ms,travadas,travadas_por_mil" >> "$CSV"
log "=== bench start: ${STEP}s x ${ROUNDS} rodadas, intercalado ==="

r=1
while [ "$r" -le "$ROUNDS" ]; do
  if [ $((r % 2)) = 1 ]; then JS_ORDER="25 50 75 100"; else JS_ORDER="100 75 50 25"; fi
  for js in $JS_ORDER; do
    step_run "$r" js 50 15 "$js"
  done
  r=$((r + 1))
done
BEST_JS=$(winner "$CSV" js 3)
NJS=$(valid_candidates "$CSV" js)
if [ -z "$BEST_JS" ] || [ "$NJS" -lt 2 ]; then
  BEST_JS=50
  log "bench: fase js inconclusiva ($NJS candidatos validos) — mantendo o preset 50"
fi
log "bench: js_scheduling_period escolhido = $BEST_JS"

r=1
while [ "$r" -le "$ROUNDS" ]; do
  if [ $((r % 2)) = 1 ]; then DV_ORDER="32:10 50:15 75:20 100:30"
  else DV_ORDER="100:30 75:20 50:15 32:10"; fi
  for pair in $DV_ORDER; do
    dv=${pair%%:*}; pol=${pair#*:}
    step_run "$r" dvfs "$dv" "$pol" "$BEST_JS"
  done
  r=$((r + 1))
done
BEST_DVFS=$(winner "$CSV" dvfs 1)
BEST_POL=$(winner "$CSV" dvfs 2)
NDV=$(valid_candidates "$CSV" dvfs)
if [ -z "$BEST_DVFS" ] || [ "$NDV" -lt 2 ]; then
  BEST_DVFS=50; BEST_POL=15
  log "bench: fase dvfs inconclusiva ($NDV candidatos validos) — mantendo o preset 50/15"
fi

SPREAD_JS=$(spread "$CSV" js)
SPREAD_DV=$(spread "$CSV" dvfs)
MIN_SPREAD=$(clamp_int "$(read_cfg bench_min_spread_pct 5)" 1 50 5)
case "$SPREAD_JS" in ''|*[!0-9]*) SPREAD_JS=0;; esac
case "$SPREAD_DV" in ''|*[!0-9]*) SPREAD_DV=0;; esac
if [ "$SPREAD_JS" -lt "$MIN_SPREAD" ]; then BEST_JS=inconclusivo; fi
if [ "$SPREAD_DV" -lt "$MIN_SPREAD" ]; then BEST_DVFS=inconclusivo; BEST_POL=inconclusivo; fi
echo "melhor,$BEST_DVFS,$BEST_POL,$BEST_JS,,,,,dispersao_js=${SPREAD_JS}%_dvfs=${SPREAD_DV}%" >> "$CSV"
log "=== bench done: dvfs=$BEST_DVFS polling=$BEST_POL js=$BEST_JS (dispersao js ${SPREAD_JS}% / dvfs ${SPREAD_DV}%) ==="
echo "done" > "$STATE"
