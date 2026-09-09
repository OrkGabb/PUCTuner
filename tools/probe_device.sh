#!/system/bin/sh
id
for d in /data/adb/ksu/modules /data/adb/modules /data/adb/modules_update; do
  ls -ld "$d" 2>/dev/null
done
cat /data/adb/m54tuner/config
head -100 /data/adb/m54tuner/factory
cat /proc/uptime
ps -A -o PID,NAME,ARGS | grep -E 'fas-rs|m54tuner|definitive|raco' | head -15
for p in /sys/devices/system/cpu/cpufreq/policy*; do
  echo "$p"
  cat "$p/scaling_available_frequencies" "$p/scaling_min_freq" "$p/scaling_max_freq"
done
for k in gpu_freq_table gpu_busy gpu_min_clock gpu_max_clock; do
  echo "$k=$(cat /sys/kernel/gpu/$k 2>/dev/null)"
done
for k in temp current_now voltage_now status capacity; do
  echo "battery.$k=$(cat /sys/class/power_supply/battery/$k 2>/dev/null)"
done
for z in /sys/class/thermal/thermal_zone*; do
  echo "$(cat "$z/type") $(cat "$z/temp")"
done
dumpsys activity activities | grep -m1 mResumedActivity
dumpsys power | grep -E 'mWakefulness=|Display Power'
dumpsys SurfaceFlinger --list | head -20
