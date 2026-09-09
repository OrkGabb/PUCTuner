#!/system/bin/sh
set -eu
MODDIR=/data/adb/modules/m54tuner
for s in "$MODDIR"/*.sh "$MODDIR/scripts"/*.sh; do sh -n "$s"; done
echo NATIVE
cat /data/adb/m54tuner/adaptive_pid /data/adb/m54tuner/adaptive_status
pid=$(sed -n 's/^pid=//p' /data/adb/m54tuner/adaptive_pid)
grep -E 'VmRSS|Threads' "/proc/$pid/status"
echo THERMAL
for z in /sys/class/thermal/thermal_zone*; do
  type=$(cat "$z/type")
  case "$type" in BIG|LITTLE|G3D|battery) echo "$type $(cat "$z/mode" 2>/dev/null) $(cat "$z/temp")";; esac
done
echo ARTIFACTS
sha256sum "$MODDIR/bin/m54-adaptive" /data/local/tmp/m54tuner-module.zip
dumpsys package com.orkgabb.m54tuner | grep -E 'versionName=|versionCode=|pkgFlags=' | head -4
echo CHECK_ROOT
id
