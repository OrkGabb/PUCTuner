#!/system/bin/sh
ls /data/adb/modules
ls /data/adb/modules/ProjectRaco
ls /data/adb/modules/ProjectRaco/CoreSys
cat /data/adb/modules/ProjectRaco/service.sh
pm path com.orkgabb.m54tuner
dumpsys activity activities | grep -E 'Resumed|topResumed|mFocused|mState=RESUMED' | head -12
dumpsys window windows | grep -E 'mCurrentFocus|mFocusedApp' | head -4
dumpsys SurfaceFlinger --latency | head -8
dumpsys SurfaceFlinger --list | grep -E 'SurfaceView|com.orkgabb|Launcher|launcher|StatusBar' | head -14
cat /sys/class/power_supply/battery/uevent
