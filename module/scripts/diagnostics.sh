#!/system/bin/sh
# Privacy-conscious support bundle: package lists are redacted, while tuning/kernel state remains.
DIR=$(dirname "$0")
. "$DIR/lib.sh"
OUT=/sdcard/Download/M54Tuner-diagnostics.txt
TMP="$M54_DIR/diagnostics.$$"
{
  echo "M54 Tuner diagnostics v3"
  echo "generated=$(date '+%Y-%m-%dT%H:%M:%S%z')"
  echo "fingerprint=$(getprop ro.build.fingerprint)"
  echo "kernel=$(uname -r)"
  echo "boot_id=$(boot_id)"
  echo "uptime=$(cat /proc/uptime)"
  echo "module=$(grep -E '^(version|versionCode)=' "${DIR%/*}/module.prop" 2>/dev/null | tr '\n' ' ')"
  echo "CONFIG_REDACTED"
  sed -E 's/^(games|protect_list|render_apps)=.*/\1=<redacted>/' "$CONFIG" 2>/dev/null
  echo "FACTORY"
  cat "$FACTORY" 2>/dev/null
  echo "STATUS"
  sh "$DIR/status.sh" --full 2>/dev/null | sed 's/^adaptive.app=.*/adaptive.app=<redacted>/'
  echo "ADAPTIVE_HISTORY (hashed app IDs, observed samples only)"
  tail -30 "$M54_DIR/adaptive_history.csv" 2>/dev/null
  echo "PRESSURE"
  cat /proc/pressure/cpu /proc/pressure/memory /proc/pressure/io 2>/dev/null
  echo "SWAP"
  cat /proc/swaps 2>/dev/null
  cat /sys/block/zram0/mm_stat 2>/dev/null
  echo "COOLING"
  for c in /sys/class/thermal/cooling_device*; do
    [ -r "$c/type" ] && echo "$(basename "$c")|$(cat "$c/type")|$(cat "$c/cur_state")/$(cat "$c/max_state")"
  done
  echo "LOG"
  tail -200 "$LOG" 2>/dev/null
} | atomic_write "$TMP" 0600 || { rm -f "$TMP"; exit 1; }
if ! cp -f "$TMP" "$OUT" || ! chmod 0644 "$OUT"; then
  rm -f "$TMP"
  exit 1
fi
rm -f "$TMP" || exit 1
echo "$OUT"
