#!/system/bin/sh
set -eu
LAB=/data/adb/m54tuner-validation
mkdir -p "$LAB"
chmod 0700 "$LAB"
snapshot() {
  for n in /sys/devices/system/cpu/cpufreq/policy*/scaling_min_freq /sys/kernel/gpu/gpu_min_clock /sys/kernel/gpu/gpu_max_clock /sys/class/devfreq/*mif*/min_freq /proc/sys/kernel/sched_pelt_multiplier; do
    echo "$n=$(cat "$n")"
  done
}
snapshot > "$LAB/before"
cat > "$LAB/config" <<'CFG'
profile=balanced
adaptive_mode=active
adaptive_learning=1
adaptive_target_fps=120
adaptive_thermal_limit=82
CFG
am start -a android.settings.SETTINGS >/dev/null
/data/local/tmp/m54-adaptive --run "$LAB" >/dev/null 2>&1 &
engine_pid=$!
cleanup() {
  kill -TERM "$engine_pid" 2>/dev/null || true
  wait "$engine_pid" || true
  /data/local/tmp/m54-adaptive --restore "$LAB" || true
  input keyevent KEYCODE_HOME
}
trap cleanup EXIT INT TERM
for tier in balanced game powersave; do
  sed "s/^profile=.*/profile=$tier/" "$LAB/config" > "$LAB/config.next"
  mv "$LAB/config.next" "$LAB/config"
  i=0
  while [ "$i" -lt 20 ]; do
    input swipe 500 1750 500 500 450
    input swipe 500 500 500 1750 450
    i=$((i + 1))
  done
  echo "TIER=$tier"
  cat "$LAB/adaptive_status"
done
kill -TERM "$engine_pid"
wait "$engine_pid"
snapshot > "$LAB/after"
echo RESTORATION
diff "$LAB/before" "$LAB/after" || true
cat "$LAB/adaptive_status"
tail -8 "$LAB/adaptive_history.csv"
id
