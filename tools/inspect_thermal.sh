#!/system/bin/sh
ls -l /sys/class/thermal/thermal_zone0/mode /sys/class/thermal/thermal_zone1/mode /sys/class/thermal/thermal_zone2/mode
for z in /sys/class/thermal/thermal_zone0 /sys/class/thermal/thermal_zone1 /sys/class/thermal/thermal_zone2; do
  readlink -f "$z/mode"
done
cat /proc/self/mountinfo | grep -E 'thermal|cooling|/sys/devices/system/cpu|/sys/kernel/gpu' | head -30
grep -E 'CapEff|CapPrm' /proc/self/status
lsattr /sys/class/thermal/thermal_zone0/mode 2>/dev/null
dumpsys activity activities | grep -m1 -E 'topResumedActivity=|mResumedActivity'
logcat -d -t 100 -s M54Tuner libsu Shell AndroidRuntime
