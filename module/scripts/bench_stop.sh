#!/system/bin/sh
# Stops a running sweep at the ONE correct process, so its trap restores the user's config.
# Signalling by name (pkill -f bench.sh) hits the subshells as well and leaves the config in the
# sweep's state — which is exactly what happened once.
DIR=$(dirname "$0")
. "$DIR/lib.sh"

case "$1" in
  sched) PIDF="$M54_DIR/bench_sched_pid"; NAME=bench_sched.sh; STATE="$M54_DIR/bench_sched_state" ;;
  *)     PIDF="$M54_DIR/bench_pid"; NAME=bench.sh; STATE="$M54_DIR/bench_state" ;;
esac
if [ ! -f "$PIDF" ]; then
  echo "nenhuma varredura em andamento"
  exit 0
fi
if pid_record_alive "$PIDF" "$NAME"; then PID=$(pid_record_pid "$PIDF")
else rm -f "$PIDF"; echo "registro obsoleto removido"; exit 0; fi
kill -TERM "$PID" 2>/dev/null
i=0
while [ "$i" -lt 15 ]; do
  kill -0 "$PID" 2>/dev/null || break
  sleep 1
  i=$((i + 1))
done
if kill -0 "$PID" 2>/dev/null; then
  # It refused to go; force it and restore by hand, because SIGKILL never runs the trap.
  kill -9 "$PID" 2>/dev/null
  dumpsys SurfaceFlinger --timestats -disable >/dev/null 2>&1
  sh "$DIR/apply_profile.sh" >/dev/null 2>&1
  if [ "$NAME" = bench_sched.sh ] && [ -f "$M54_DIR/bench_sched_original" ]; then
    OP=$(sed -n '1p' "$M54_DIR/bench_sched_original")
    OE=$(sed -n '2p' "$M54_DIR/bench_sched_original")
    case "$OP" in 1|2|4) echo "$OP" > /proc/sys/kernel/sched_pelt_multiplier 2>/dev/null;; esac
    case "$OE" in 0|1) echo "$OE" > /proc/sys/kernel/sched_energy_aware 2>/dev/null;; esac
  fi
fi
echo "stopped" > "$STATE"
rm -f "$PIDF" "$M54_DIR/bench_sched_original"
echo "varredura parada, config restaurada"
