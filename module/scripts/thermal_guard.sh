#!/system/bin/sh
# Independent fail-safe for aggressive thermal mode. Default thresholds are conservative and the
# feature only runs while thermal=aggressive. It does not depend on the app staying alive.
DIR=$(dirname "$0")
. "$DIR/lib.sh"

PIDF="$M54_DIR/thermal_guard_pid"
STATE="$M54_DIR/thermal_guard_state"
if pid_record_alive "$PIDF" thermal_guard.sh; then exit 0; fi
rm -f "$PIDF"
pid_record_write "$PIDF" thermal_guard.sh
cleanup_guard() {
  trap - EXIT INT TERM
  [ "$(pid_record_pid "$PIDF")" = "$$" ] && rm -f "$PIDF"
  exit 0
}
trap cleanup_guard EXIT INT TERM

HIGH=$(clamp_int "$(read_cfg thermal_guard_high 78000)" 65000 95000 78000)
LOW=$(clamp_int "$(read_cfg thermal_guard_low 70000)" 55000 90000 70000)
INTERVAL=$(clamp_int "$(read_cfg thermal_guard_interval 2)" 1 10 2)
TIMEOUT=$(clamp_int "$(read_cfg thermal_guard_timeout 900)" 60 3600 900)
[ "$LOW" -ge "$HIGH" ] && LOW=$((HIGH - 8000))
START=$(uptime_s)
TIMED_OUT=0

set_compute_modes() {
  local mode="$1" z ty
  for z in /sys/class/thermal/thermal_zone*; do
    [ -e "$z/mode" ] || continue
    ty=$(cat "$z/type" 2>/dev/null | tr 'A-Z' 'a-z')
    case "$ty" in big|little|g3d|*cpu*|*gpu*) ( echo "$mode" > "$z/mode" ) 2>/dev/null;; esac
  done
}

while [ "$(read_cfg thermal aggressive)" = aggressive ]; do
  MAX=0; BATT=0
  for z in /sys/class/thermal/thermal_zone*; do
    ty=$(cat "$z/type" 2>/dev/null | tr 'A-Z' 'a-z')
    temp=$(cat "$z/temp" 2>/dev/null); case "$temp" in ''|*[!0-9]*) continue;; esac
    case "$ty" in *batt*) [ "$temp" -gt "$BATT" ] && BATT=$temp;;
      big|little|g3d|*cpu*|*gpu*) [ "$temp" -gt "$MAX" ] && MAX=$temp;; esac
  done
  NOW=$(uptime_s); ELAPSED=$((NOW - START))
  [ "$ELAPSED" -ge "$TIMEOUT" ] && TIMED_OUT=1
  if [ "$MAX" -ge "$HIGH" ] || [ "$BATT" -ge 45000 ] || [ "$TIMED_OUT" = 1 ]; then
    touch "$M54_DIR/thermal_guard_forced"
    set_compute_modes enabled
    echo "guarded|max=$MAX|battery=$BATT|elapsed=$ELAPSED" | atomic_write "$STATE" 0600
  elif [ -e "$M54_DIR/thermal_guard_forced" ] && [ "$MAX" -le "$LOW" ]; then
    rm -f "$M54_DIR/thermal_guard_forced"
    set_compute_modes disabled
    echo "aggressive|max=$MAX|battery=$BATT|elapsed=$ELAPSED" | atomic_write "$STATE" 0600
  fi
  sleep "$INTERVAL"
done

rm -f "$M54_DIR/thermal_guard_forced"
set_compute_modes enabled
echo moderate | atomic_write "$STATE" 0600
