#!/system/bin/sh
cat /data/adb/modules/ProjectRaco/uninstall.sh
cat /data/adb/modules/ProjectRaco/post-fs-data.sh
ls -la /data/ProjectRaco
cat /data/ProjectRaco/raco.txt
pm list packages | grep -i raco
ps -A -o PID,PPID,NAME,ARGS | grep -Ei 'raco|anya|kobo|zetamin|rswap|Rshoot|Ayunda'
settings get secure enabled_accessibility_services
settings get secure enabled_notification_listeners
cat /proc/swaps
ls -la /data/adb/service.d /data/adb/post-fs-data.d
ls -la /data/adb/modules/encore
