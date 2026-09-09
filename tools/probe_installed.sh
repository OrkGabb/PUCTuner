#!/system/bin/sh
MODDIR=/data/adb/modules/m54tuner
cat /data/adb/m54tuner/adaptive_pid /data/adb/m54tuner/adaptive_status
sh "$MODDIR/scripts/status.sh" | grep -E '^(adaptive\.|factory|module|cpu.*min)'
/data/adb/ksud module list
/data/adb/ksud profile --help
pm list packages -U com.orkgabb.m54tuner
