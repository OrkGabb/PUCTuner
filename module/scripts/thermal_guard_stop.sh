#!/system/bin/sh
DIR=$(dirname "$0")
. "$DIR/lib.sh"
PIDF="$M54_DIR/thermal_guard_pid"
if pid_record_alive "$PIDF" thermal_guard.sh; then
  PID=$(pid_record_pid "$PIDF")
  kill -TERM "$PID" 2>/dev/null
  i=0; while [ "$i" -lt 5 ] && kill -0 "$PID" 2>/dev/null; do sleep 1; i=$((i + 1)); done
fi
rm -f "$PIDF" "$M54_DIR/thermal_guard_forced"
