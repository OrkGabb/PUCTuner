#!/system/bin/sh
sleep 2
echo CPU_IDLE
cat /sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq /sys/devices/system/cpu/cpufreq/policy4/scaling_min_freq
echo ROOT_MANAGER
/data/adb/ksud --help
echo RACO_REMAINDERS
find /data/adb /data/local/tmp /sdcard/Android/data /sdcard/Android/obb -maxdepth 3 -iname '*raco*' 2>/dev/null | head -20
echo KERNEL_ROOT
uname -r
id
