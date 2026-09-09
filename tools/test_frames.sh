#!/system/bin/sh
am start -a android.settings.SETTINGS >/dev/null
sleep 2
(
  sleep 1
  /data/local/tmp/m54-adaptive --probe > /data/local/tmp/m54-frame-probe.txt
) &
probe_pid=$!
i=0
while [ "$i" -lt 5 ]; do
  input swipe 500 1700 500 600 500
  i=$((i + 1))
done
wait "$probe_pid"
cat /data/local/tmp/m54-frame-probe.txt
dumpsys SurfaceFlinger --list | grep -E 'com.android.settings.*\$_' | head -3
input keyevent KEYCODE_HOME
